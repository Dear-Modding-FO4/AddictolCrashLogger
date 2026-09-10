#include "ThreadDump/ThreadDump.h"

#include "Analysis/Analysis.h"
#include "Capture/ProcessSnapshot.h"
#include "Capture/SnapshotStackWalker.h"
#include "CommonHeader/CommonHeader.h"
#include "Introspection/Introspection.h"
#include "Introspection/ReadonlyIntrospection.h"
#include "PDB/PdbHandler.h"
#include "Settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <thread>

namespace Crash
{
	namespace
	{
		std::atomic<bool> s_stopHotkeyThread{ false };
		std::atomic_flag s_captureInFlight = ATOMIC_FLAG_INIT;
		std::jthread s_hotkeyThread;

		class CaptureClaim
		{
		public:
			CaptureClaim() : m_owned(!s_captureInFlight.test_and_set()) {}
			~CaptureClaim()
			{
				if (m_owned)
					s_captureInFlight.clear();
			}
			[[nodiscard]] explicit operator bool() const noexcept { return m_owned; }

		private:
			bool m_owned{};
		};

		[[nodiscard]] const Capture::ModuleImage* module_for(
			std::span<const Capture::ModuleImage> a_modules,
			std::uint64_t a_address) noexcept
		{
			for (const auto& module : a_modules)
				if (module.contains(a_address))
					return std::addressof(module);
			return nullptr;
		}

		[[nodiscard]] std::string module_name(const Capture::ModuleImage& a_module)
		{
			try
			{
				const std::filesystem::path path(a_module.path);
				const auto filename = path.filename().string();
				return filename.empty() ? "<captured image>" : filename;
			}
			catch (...)
			{
				return "<captured image path unavailable>";
			}
		}

		[[nodiscard]] std::string module_path(const Capture::ModuleImage& a_module)
		{
			try
			{
				return a_module.path.empty() ?
					"<path unavailable>" :
					std::filesystem::path(a_module.path).string();
			}
			catch (...)
			{
				return "<path conversion unavailable>";
			}
		}

		[[nodiscard]] std::optional<std::string> symbol_path(
			const Capture::ModuleImage& a_module)
		{
			if (a_module.path.empty())
				return std::nullopt;
			if (!a_module.pathIsNtDevice)
			{
				try
				{
					return std::filesystem::path(a_module.path).string();
				}
				catch (...)
				{
					return std::nullopt;
				}
			}

			std::array<wchar_t, 512> drives{};
			const auto driveLength = ::GetLogicalDriveStringsW(
				static_cast<DWORD>(drives.size()),
				drives.data());
			if (driveLength == 0 || driveLength >= drives.size())
				return std::nullopt;

			for (const wchar_t* drive = drives.data();
				 *drive != L'\0';
				 drive += std::wcslen(drive) + 1)
			{
				std::array<wchar_t, 3> driveName{
					drive[0],
					L':',
					L'\0'
				};
				std::array<wchar_t, 32768> deviceNames{};
				const auto deviceLength = ::QueryDosDeviceW(
					driveName.data(),
					deviceNames.data(),
					static_cast<DWORD>(deviceNames.size()));
				if (deviceLength == 0)
					continue;

				for (const wchar_t* device = deviceNames.data();
					 *device != L'\0';
					 device += std::wcslen(device) + 1)
				{
					const auto prefixLength = std::wcslen(device);
					if (a_module.path.size() < prefixLength ||
						::CompareStringOrdinal(
							a_module.path.data(),
							static_cast<int>(prefixLength),
							device,
							static_cast<int>(prefixLength),
							TRUE) != CSTR_EQUAL ||
						(a_module.path.size() > prefixLength &&
						 a_module.path[prefixLength] != L'\\'))
						continue;

					auto candidate = std::wstring(driveName.data());
					candidate.append(a_module.path.substr(prefixLength));
					const auto attributes = ::GetFileAttributesW(
						candidate.c_str());
					if (attributes == INVALID_FILE_ATTRIBUTES ||
						(attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
						continue;
					try
					{
						return std::filesystem::path(candidate).string();
					}
					catch (...)
					{
						return std::nullopt;
					}
				}
			}
			return std::nullopt;
		}

		[[nodiscard]] std::string frame_text(
			Capture::SnapshotOperation& a_operation,
			const Capture::StackFrame& a_frame,
			PDB::SymbolResolver& a_symbols)
		{
			const auto* module = module_for(
				a_operation.modules(),
				a_frame.programCounter);
			std::string location = module ?
				fmt::format(
					"{}+0x{:X}",
					module_name(*module),
					a_frame.programCounter - module->base) :
				"<module unavailable>";
			std::array<std::byte, 15> instruction{};
			auto bytes = module ?
				a_operation.read_owned_page_cached(
					a_frame.programCounter,
					instruction) :
				a_operation.read(
				a_frame.programCounter,
				instruction);
			if (bytes)
			{
				location += " bytes=";
				for (const auto byte : instruction)
					location += fmt::format(
						"{:02X}",
						std::to_integer<unsigned>(byte));
				location += fmt::format(
					" provenance={}",
					static_cast<int>(bytes->provenance));
			}
			else
				location += " bytes=<unavailable>";
			if (module)
			{
				const auto path = symbol_path(*module);
				if (path)
				{
					const auto symbols = a_symbols.resolve(
						*path,
						static_cast<std::uintptr_t>(
							a_frame.programCounter - module->base));
					if (!symbols.text.empty())
					{
						location += fmt::format(
							" | {}",
							symbols.text);
						if (!symbols.parameters.empty())
							location += fmt::format(
								" | params: {}",
								symbols.parameters);
						if (!symbols.issue.empty())
							location += fmt::format(
								" | symbol note: {} (HRESULT=0x{:08X})",
								symbols.issue,
								static_cast<std::uint32_t>(
									symbols.status));
					}
					else
						location += fmt::format(
							" | symbols unavailable: {} (HRESULT=0x{:08X})",
							symbols.issue.empty() ?
								"no symbol details" :
								symbols.issue,
							static_cast<std::uint32_t>(
								symbols.status));
				}
				else
					location += module->pathIsNtDevice ?
						" | symbols unavailable: NT device path has no verified DOS mapping" :
						" | symbols unavailable: module path unavailable";
			}
			return location;
		}

		[[nodiscard]] int thread_priority(
			const Capture::CapturedThread& a_thread,
			std::span<const Capture::ModuleImage> a_modules,
			std::string_view a_processName)
		{
			const auto* module = module_for(a_modules, a_thread.context.Rip);
			if (!module)
				return 0;
			const auto name = module_name(*module);
			if (_stricmp(name.c_str(), std::string(a_processName).c_str()) == 0)
				return 2;
			return name.ends_with(".dll") || name.ends_with(".DLL") ? 1 : 0;
		}

		void write_thread(
			spdlog::logger& a_log,
			const Capture::CapturedThread& a_thread,
			std::size_t a_index,
			Capture::ProcessSnapshot& a_snapshot,
			Capture::SnapshotOperation& a_operation,
			Introspection::ReadOnly::AnalysisSession& a_analysis,
			PDB::SymbolResolver& a_symbols)
		{
			a_log.critical(
				"===== THREAD {} (ID: {}){} =====",
				a_index,
				a_thread.threadId,
				a_thread.threadId == ::GetCurrentThreadId() ?
					" [CAPTURE MONITOR]" :
					"");
			a_log.critical(
				"\tCaptured RIP=0x{:016X} RSP=0x{:016X} TEB=0x{:016X}",
				a_thread.context.Rip,
				a_thread.context.Rsp,
				a_thread.tebAddress);
			if (!a_thread.hasStackBounds)
				a_log.critical("\tStack bounds: <unavailable from captured TEB>");
			else
				a_log.critical(
					"\tStack bounds: [0x{:016X}, 0x{:016X})",
					a_thread.stackLimit,
					a_thread.stackBase);

			const auto [registers, values] = get_register_info(a_thread.context);
			const auto decodedRegisters = Introspection::analyze_data(
				values,
				a_analysis,
				[&](std::size_t a_register) {
					return std::string(registers[a_register].first);
				});
			a_log.critical("\tREGISTERS (READ-ONLY SNAPSHOT DECODING):");
			for (std::size_t index = 0; index < registers.size(); ++index)
				a_log.critical(
					"\t\t{} 0x{:016X} {}",
					registers[index].first,
					registers[index].second,
					decodedRegisters[index]);

			Capture::SnapshotStackWalker walker;
			const auto walk = walker.walk(a_snapshot, a_thread, 128);
			a_log.critical(
				"\tCALLSTACK: status={} detail=\"{}\" frames={} ownedImageBytes={} weakImageReads={}",
				static_cast<int>(walk.status),
				walk.detail,
				walk.frames.size(),
				walk.instrumentation.ownedMetadataBytes,
				walk.instrumentation.weakMetadataReads);
			for (std::size_t index = 0; index < walk.frames.size(); ++index)
			{
				const auto& frame = walk.frames[index];
				a_log.critical(
					"\t\t[{}] PC=0x{:016X} SP=0x{:016X} {}",
					index,
					frame.programCounter,
					frame.stackPointer,
					frame_text(a_operation, frame, a_symbols));
			}

			if (a_thread.hasStackBounds &&
				a_thread.context.Rsp >= a_thread.stackLimit &&
				a_thread.context.Rsp < a_thread.stackBase)
			{
				const auto available = static_cast<std::size_t>(
					std::min<std::uint64_t>(
						a_thread.stackBase - a_thread.context.Rsp,
						512u * sizeof(std::size_t)));
				std::vector<std::size_t> stack(available / sizeof(std::size_t));
				auto stackRead = a_operation.read(
					a_thread.context.Rsp,
					std::span<std::byte>{
						reinterpret_cast<std::byte*>(stack.data()),
						stack.size() * sizeof(std::size_t) });
				if (stackRead)
				{
					a_log.critical(
						"\tSTACK OBJECT CANDIDATES (bounded {} bytes, provenance={}):",
						stackRead->bytesRead,
						static_cast<int>(stackRead->provenance));
					std::size_t emitted{};
					for (std::size_t index = 0;
						index < stack.size() && emitted < 64;
						++index)
					{
						const auto& budget = a_analysis.diagnostics();
						if (budget.readBudgetExceeded ||
							budget.byteBudgetExceeded ||
							budget.objectBudgetExceeded)
							break;
						const auto decoded = a_analysis.analyze_one(
							Introspection::ReadOnly::TargetAddress(stack[index]),
							fmt::format("RSP+{:X}", index * sizeof(std::size_t)));
						if (decoded.empty() || decoded.starts_with("(void*)") ||
							decoded.starts_with("<unavailable:"))
							continue;
						a_log.critical(
							"\t\tRSP+{:04X} 0x{:016X} {}",
							index * sizeof(std::size_t),
							stack[index],
							decoded);
						++emitted;
					}
					if (emitted == 64)
						a_log.critical("\t\t<output budget reached>");
				}
				else
					a_log.critical(
						"\tSTACK: <unavailable: {} ({})>",
						stackRead.error().message,
						stackRead.error().systemError);
			}
			else
				a_log.critical("\tSTACK: <unavailable: captured RSP is outside valid stack bounds>");
			const auto& diagnostics = a_analysis.diagnostics();
			a_log.critical(
				"\tTHREAD ANALYSIS STATUS: {} reads={} bytes={} objects={} weakReads={} unavailableFields={}",
				diagnostics.readBudgetExceeded || diagnostics.byteBudgetExceeded ||
					diagnostics.objectBudgetExceeded ? "PARTIAL (thread budget reached)" : "within budget",
				diagnostics.reads, diagnostics.bytes, diagnostics.objects,
				diagnostics.weakReads, diagnostics.unavailableFields);
			a_log.critical("");
		}
	}

	void WriteAllThreadsDump()
	{
		CaptureClaim claim;
		if (!claim)
		{
			REX::WARN("Manual thread capture is already in progress"sv);
			return;
		}

		try
		{
			auto [log, logPath] =
				get_timestamped_log("threaddump-"sv, "thread dump"s);
			log_common_header_info(
				*log,
				"THREAD DUMP (PSS SNAPSHOT)",
				"TIME:"sv);

			const auto api = Capture::PssApi::load();
			if (!api)
			{
				log->critical(
					"PSS CAPTURE UNAVAILABLE: {} ({})",
					api.error().message,
					api.error().systemError);
				log->flush();
				REX::ERROR(
					"Manual thread capture unavailable: {} ({})"sv,
					api.error().message,
					api.error().systemError);
				return;
			}

			const bool writeDump = Settings::bThreadDumpWriteMiniDump.GetValue();
			auto snapshotResult = Capture::ProcessSnapshot::capture(
				*api,
				Capture::CaptureOptions{ .captureHandleData = writeDump });
			if (!snapshotResult)
			{
				log->critical(
					"PSS CAPTURE FAILED: {} ({})",
					snapshotResult.error().message,
					snapshotResult.error().systemError);
				log->critical("No live-process fallback was attempted.");
				log->flush();
				REX::ERROR(
					"Manual thread capture failed: {} ({})"sv,
					snapshotResult.error().message,
					snapshotResult.error().systemError);
				return;
			}
			auto snapshot = std::move(*snapshotResult);

			bool minidumpWritten{};
			if (writeDump)
			{
				auto dumpPath = logPath;
				dumpPath.replace_extension(".dmp");
				auto dumpType = static_cast<MINIDUMP_TYPE>(
					MiniDumpWithThreadInfo |
					MiniDumpWithHandleData |
					MiniDumpWithUnloadedModules);
				if (Settings::bFullMemoryMiniDump.GetValue())
					dumpType = static_cast<MINIDUMP_TYPE>(
						dumpType | MiniDumpWithFullMemory);
				const auto dump = snapshot.write_minidump(dumpPath, dumpType);
				if (dump)
				{
					minidumpWritten = true;
					log->critical(
						"Same-HPSS minidump written before analysis: {}",
						dumpPath.string());
				}
				else
					log->critical(
						"Same-HPSS minidump failed: {} ({})",
						dump.error().message,
						dump.error().systemError);
				log->flush();
			}

			auto operationResult = snapshot.reader().begin_operation(
				16u * 1024u * 1024u);
			if (!operationResult)
			{
				log->critical(
					"ANALYSIS SESSION FAILED: {} ({})",
					operationResult.error().message,
					operationResult.error().systemError);
				log->flush();
				return;
			}
			auto operation = std::move(*operationResult);
			auto moduleRanges =
				Introspection::ReadOnly::snapshot_module_ranges(operation);
			Introspection::ReadOnly::SnapshotMemoryReader reader(operation);
			const auto version =
				REX::FModule::GetExecutingModule().GetFileVersion();
			const auto profile = Introspection::ReadOnly::runtime_profile(
				version[0], version[1], version[2], version[3]);
			constexpr std::size_t maximumReads = 24'000;
			constexpr std::size_t maximumBytes = 4u * 1024u * 1024u;
			constexpr std::size_t maximumObjects = 2'000;
			Introspection::ReadOnly::AnalysisDiagnostics diagnostics;
			PDB::SymbolResolver symbolResolver;

			std::vector<const Capture::CapturedThread*> ordered;
			ordered.reserve(operation.threads().size());
			for (const auto& thread : operation.threads())
				ordered.push_back(std::addressof(thread));
			std::string processName;
			if (!moduleRanges.empty())
				processName = moduleRanges.front().name;
			std::stable_sort(
				ordered.begin(),
				ordered.end(),
				[&](const auto* a_left, const auto* a_right) {
					return thread_priority(
							   *a_left,
							   operation.modules(),
							   processName) >
					       thread_priority(
							   *a_right,
							   operation.modules(),
							   processName);
				});

			log->critical(
				"Captured Threads: {} (single HPSS, no suspension loop or recapture)",
				ordered.size());
			log->critical(
				"Captured Modules: {} (catalog built from captured image VA)",
				operation.modules().size());
			for (const auto& module : operation.modules())
			{
				log->critical(
					"\t0x{:016X}-0x{:016X} {}{}{}",
					module.base,
					module.base + module.size,
					module_path(module),
					module.pathIsNtDevice ? " [NT device path]" : "",
					module.metadataPartial ?
						" [partial unwind metadata]" :
						"");
				if (module.metadataPartial && !module.metadataStatus.empty())
					log->critical("\t\t{}", module.metadataStatus);
			}
			log->critical("");
			for (std::size_t index = 0; index < ordered.size(); ++index)
			{
				const auto remainingThreads = ordered.size() - index;
				Introspection::ReadOnly::AnalysisSession analysis(
					reader, moduleRanges, profile,
					Introspection::ReadOnly::AnalysisBudgets{
						.maximumReads = (maximumReads - std::min(maximumReads, diagnostics.reads)) / remainingThreads,
						.maximumBytes = (maximumBytes - std::min(maximumBytes, diagnostics.bytes)) / remainingThreads,
						.maximumObjects = (maximumObjects - std::min(maximumObjects, diagnostics.objects)) / remainingThreads,
						.maximumDepth = 8,
						.maximumStringBytes = 512
					});
				write_thread(
					*log,
					*ordered[index],
					index + 1,
					snapshot,
					operation,
					analysis,
					symbolResolver);
				const auto& threadDiagnostics = analysis.diagnostics();
				diagnostics.reads += threadDiagnostics.reads;
				diagnostics.bytes += threadDiagnostics.bytes;
				diagnostics.objects += threadDiagnostics.objects;
				diagnostics.weakReads += threadDiagnostics.weakReads;
				diagnostics.unavailableFields += threadDiagnostics.unavailableFields;
				diagnostics.readBudgetExceeded |= threadDiagnostics.readBudgetExceeded;
				diagnostics.byteBudgetExceeded |= threadDiagnostics.byteBudgetExceeded;
				diagnostics.objectBudgetExceeded |= threadDiagnostics.objectBudgetExceeded;
				log->flush();
			}
			log->critical(
				"READ-ONLY ANALYSIS STATUS: reads={} bytes={} objects={} weakReads={} unavailableFields={}",
				diagnostics.reads,
				diagnostics.bytes,
				diagnostics.objects,
				diagnostics.weakReads,
				diagnostics.unavailableFields);
			if (diagnostics.readBudgetExceeded ||
				diagnostics.byteBudgetExceeded ||
				diagnostics.objectBudgetExceeded)
				log->critical("READ-ONLY ANALYSIS STATUS: PARTIAL (one or more thread budgets reached)");
			log->flush();

			clean_old_files(
				logPath.parent_path(),
				"threaddump-"sv,
				".log",
				Settings::iMaxCrashLogs.GetValue(),
				".dmp");
			clean_old_files(
				logPath.parent_path(),
				"threaddump-"sv,
				".dmp",
				Settings::iMaxMiniDumps.GetValue());

			LOG::INFO(
				"Manual snapshot thread report written to {}{}",
				logPath.string(),
				minidumpWritten ? " with same-HPSS minidump" : "");
			auto_open_log(logPath);
		}
		catch (const std::exception& exception)
		{
			REX::ERROR(
				"Manual snapshot thread report failed: {}"sv,
				exception.what());
		}
		catch (...)
		{
			REX::ERROR("Manual snapshot thread report failed: unknown error"sv);
		}
	}

	void HotkeyMonitorThreadFunction()
	{
		bool wasPressed{};
		while (!s_stopHotkeyThread)
		{
			bool allPressed = true;
			for (const int key : Settings::hotkeyThreadDump)
				allPressed = allPressed && (::GetAsyncKeyState(key) & 0x8000) != 0;
			if (allPressed && !wasPressed)
			{
				wasPressed = true;
				WriteAllThreadsDump();
			}
			else if (!allPressed)
				wasPressed = false;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}

	void StartHotkeyMonitoring()
	{
		if (!Settings::bEnableThreadDumpHotkey.GetValue())
		{
			REX::INFO("Thread dump hotkey disabled"sv);
			return;
		}
		if (Settings::hotkeyThreadDump.empty())
		{
			REX::INFO("Thread dump hotkey not configured"sv);
			return;
		}
		s_stopHotkeyThread = false;
		s_hotkeyThread = std::jthread(HotkeyMonitorThreadFunction);
		REX::INFO("PSS thread dump hotkey monitoring started"sv);
	}

	void StopHotkeyMonitoring()
	{
		s_stopHotkeyThread = true;
		if (s_hotkeyThread.joinable())
			s_hotkeyThread.join();
	}
}
