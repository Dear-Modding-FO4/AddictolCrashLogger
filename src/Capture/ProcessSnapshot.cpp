#include "Capture/ProcessSnapshot.h"

#include "Capture/DbgHelpGate.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <unordered_map>
#include <utility>

namespace Capture
{
	namespace
	{
		std::atomic<std::uint32_t> s_activeSnapshots{};
		std::atomic<std::uint32_t> s_activeWalkMarkers{};
		std::atomic<std::uint32_t> s_snapshotReleaseFailures{};
		std::atomic<std::uint32_t> s_walkMarkerReleaseFailures{};
		std::atomic<DWORD> s_lastSnapshotReleaseError{};
		std::atomic<DWORD> s_lastWalkMarkerReleaseError{};

		[[nodiscard]] Error make_error(
			ErrorCode a_code,
			DWORD a_systemError,
			std::string a_message,
			std::size_t a_bytesCompleted = 0)
		{
			return Error{
				.code = a_code,
				.systemError = a_systemError,
				.message = std::move(a_message),
				.bytesCompleted = a_bytesCompleted
			};
		}

		[[nodiscard]] bool checked_end(
			std::uint64_t a_base,
			std::uint64_t a_size,
			std::uint64_t& a_end) noexcept
		{
			if (a_size > std::numeric_limits<std::uint64_t>::max() - a_base)
				return false;
			a_end = a_base + a_size;
			return true;
		}

		[[nodiscard]] bool readable_protection(DWORD a_protect) noexcept
		{
			if ((a_protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
				return false;

			switch (a_protect & 0xFF)
			{
			case PAGE_READONLY:
			case PAGE_READWRITE:
			case PAGE_WRITECOPY:
			case PAGE_EXECUTE:
			case PAGE_EXECUTE_READ:
			case PAGE_EXECUTE_READWRITE:
			case PAGE_EXECUTE_WRITECOPY:
				return true;
			default:
				return false;
			}
		}

		[[nodiscard]] std::wstring copy_walk_string(const wchar_t* a_value, WORD a_length)
		{
			if (!a_value || a_length == 0)
				return {};

			// PSS string lengths are byte counts. Avoid retaining marker-owned pointers.
			return std::wstring(a_value, a_length / sizeof(wchar_t));
		}

		class WalkMarker
		{
		public:
			WalkMarker() = default;

			[[nodiscard]] static std::expected<WalkMarker, Error> create(const PssApi& a_api)
			{
				HPSSWALK marker{};
				const auto error = a_api.create_walk_marker(&marker);
				if (error != ERROR_SUCCESS)
				{
					return std::unexpected(make_error(
						ErrorCode::kWalkFailed,
						error,
						"PssWalkMarkerCreate failed"));
				}

				WalkMarker result;
				result.m_api = std::addressof(a_api);
				result.m_marker = marker;
				++s_activeWalkMarkers;
				return result;
			}

			~WalkMarker()
			{
				reset();
			}

			WalkMarker(WalkMarker&& a_other) noexcept :
				m_api(std::exchange(a_other.m_api, nullptr)),
				m_marker(std::exchange(a_other.m_marker, nullptr))
			{}

			WalkMarker& operator=(WalkMarker&& a_other) noexcept
			{
				if (this != std::addressof(a_other))
				{
					reset();
					m_api = std::exchange(a_other.m_api, nullptr);
					m_marker = std::exchange(a_other.m_marker, nullptr);
				}
				return *this;
			}

			WalkMarker(const WalkMarker&) = delete;
			WalkMarker& operator=(const WalkMarker&) = delete;

			[[nodiscard]] HPSSWALK get() const noexcept
			{
				return m_marker;
			}

		private:
			void reset() noexcept
			{
				if (m_marker)
				{
					const auto error = m_api->free_walk_marker(m_marker);
					if (error == ERROR_SUCCESS)
					{
						m_marker = nullptr;
						--s_activeWalkMarkers;
					}
					else
					{
						++s_walkMarkerReleaseFailures;
						s_lastWalkMarkerReleaseError = error;
						m_marker = nullptr;
					}
				}
			}

			const PssApi* m_api{};
			HPSSWALK m_marker{};
		};

		struct TebStackPrefix
		{
			void* exceptionList{};
			void* stackBase{};
			void* stackLimit{};
		};

		[[nodiscard]] const MemoryRegion* find_region_at(
			const std::vector<MemoryRegion>& a_regions,
			std::uint64_t a_address,
			Error* a_error = nullptr) noexcept
		{
			const auto it = std::upper_bound(
				a_regions.begin(),
				a_regions.end(),
				a_address,
				[](std::uint64_t a_value, const MemoryRegion& a_region) {
					return a_value < a_region.base;
				});
			if (it == a_regions.begin())
			{
				if (a_error)
					*a_error = make_error(ErrorCode::kInvalidAddress, ERROR_INVALID_ADDRESS, "address is not in captured VA space");
				return nullptr;
			}

			const auto& region = *std::prev(it);
			std::uint64_t regionEnd{};
			if (!checked_end(region.base, region.size, regionEnd) ||
				a_address < region.base ||
				a_address >= regionEnd)
			{
				if (a_error)
					*a_error = make_error(ErrorCode::kInvalidAddress, ERROR_INVALID_ADDRESS, "address is not in captured VA space");
				return nullptr;
			}

			if (region.state != MEM_COMMIT || !readable_protection(region.protect))
			{
				if (a_error)
					*a_error = make_error(ErrorCode::kUnreadable, ERROR_NOACCESS, "captured region is not readable");
				return nullptr;
			}
			return std::addressof(region);
		}

		[[nodiscard]] std::expected<ReadResult, Error> read_clone(
			const SnapshotState& a_state,
			std::uint64_t a_address,
			std::span<std::byte> a_destination);

		[[nodiscard]] std::expected<void, Error> collect_regions(SnapshotState& a_state);
		[[nodiscard]] std::expected<void, Error> collect_threads(SnapshotState& a_state);
		[[nodiscard]] std::expected<void, Error> collect_modules(SnapshotState& a_state);

		[[nodiscard]] MemoryProvenance combine_provenance(
			MemoryProvenance a_left,
			MemoryProvenance a_right) noexcept
		{
			const auto rank = [](MemoryProvenance a_value) {
				switch (a_value)
				{
				case MemoryProvenance::kPrivateFrozen:
					return 0;
				case MemoryProvenance::kOwnedAtAnalysisFromSharedImage:
					return 1;
				case MemoryProvenance::kSharedImageWeak:
					return 2;
				case MemoryProvenance::kSharedMappedWeak:
					return 3;
				default:
					return 4;
				}
			};
			return rank(a_left) >= rank(a_right) ? a_left : a_right;
		}

		BOOL CALLBACK snapshot_dump_callback(
			PVOID,
			const PMINIDUMP_CALLBACK_INPUT a_input,
			PMINIDUMP_CALLBACK_OUTPUT a_output)
		{
			if (a_input && a_output && a_input->CallbackType == IsProcessSnapshotCallback)
				a_output->Status = S_FALSE;
			return TRUE;
		}
	}

	struct SnapshotState
	{
		explicit SnapshotState(const PssApi& a_api) :
			api(a_api)
		{}

		~SnapshotState()
		{
			if (snapshot)
			{
				const auto error = api.free_snapshot(::GetCurrentProcess(), snapshot);
				if (error == ERROR_SUCCESS)
				{
					snapshot = nullptr;
					--s_activeSnapshots;
				}
				else
				{
					++s_snapshotReleaseFailures;
					s_lastSnapshotReleaseError = error;
					snapshot = nullptr;
				}
			}
		}

		PssApi api;
		HPSS snapshot{};
		HANDLE clone{};
		DWORD processId{};
		bool handleData{};
		std::vector<MemoryRegion> regions;
		std::vector<CapturedThread> threads;
		std::vector<ModuleImage> modules;
	};

	namespace
	{
		std::expected<ReadResult, Error> read_clone(
			const SnapshotState& a_state,
			std::uint64_t a_address,
			std::span<std::byte> a_destination)
		{
			if (a_destination.empty())
				return ReadResult{};

			std::uint64_t requestedEnd{};
			if (!checked_end(a_address, a_destination.size(), requestedEnd))
				return std::unexpected(make_error(
					ErrorCode::kAddressOverflow,
					ERROR_ARITHMETIC_OVERFLOW,
					"address range overflow"));

			std::size_t completed = 0;
			auto provenance = MemoryProvenance::kPrivateFrozen;
			while (completed < a_destination.size())
			{
				const auto currentAddress = a_address + completed;
				Error validationError{};
				const auto* region = find_region_at(
					a_state.regions,
					currentAddress,
					std::addressof(validationError));
				if (!region)
				{
					validationError.code = completed == 0 ?
						validationError.code :
						ErrorCode::kPartialRead;
					validationError.bytesCompleted = completed;
					validationError.message = completed == 0 ?
						validationError.message :
						"captured range has partial committed/readable coverage";
					return std::unexpected(std::move(validationError));
				}

				std::uint64_t regionEnd{};
				if (!checked_end(region->base, region->size, regionEnd))
					return std::unexpected(make_error(
						ErrorCode::kInvalidAddress,
						ERROR_ARITHMETIC_OVERFLOW,
						"captured region range overflow",
						completed));
				const auto chunkSize = static_cast<std::size_t>(
					std::min<std::uint64_t>(
						a_destination.size() - completed,
						regionEnd - currentAddress));
				SIZE_T bytesRead{};
				if (!::ReadProcessMemory(
						a_state.clone,
						reinterpret_cast<const void*>(currentAddress),
						a_destination.data() + completed,
						chunkSize,
						&bytesRead) ||
					bytesRead != chunkSize)
				{
					const auto total = completed + static_cast<std::size_t>(bytesRead);
					return std::unexpected(make_error(
						total == 0 ? ErrorCode::kUnreadable : ErrorCode::kPartialRead,
						::GetLastError(),
						total == 0 ?
							"ReadProcessMemory from VA clone failed" :
							"ReadProcessMemory from VA clone returned partial coverage",
						total));
				}

				completed += chunkSize;
				provenance = combine_provenance(provenance, region->provenance());
			}

			return ReadResult{ .bytesRead = completed, .provenance = provenance };
		}

		std::expected<void, Error> collect_regions(SnapshotState& a_state)
		{
			auto markerResult = WalkMarker::create(a_state.api);
			if (!markerResult)
				return std::unexpected(markerResult.error());
			auto marker = std::move(*markerResult);

			for (;;)
			{
				PSS_VA_SPACE_ENTRY entry{};
				const auto error = a_state.api.walk_snapshot(
					a_state.snapshot,
					PSS_WALK_VA_SPACE,
					marker.get(),
					std::addressof(entry),
					sizeof(entry));
				if (error == ERROR_NO_MORE_ITEMS)
					break;
				if (error != ERROR_SUCCESS)
				{
					return std::unexpected(make_error(
						ErrorCode::kWalkFailed,
						error,
						"PssWalkSnapshot(PSS_WALK_VA_SPACE) failed"));
				}

				a_state.regions.push_back(MemoryRegion{
					.base = reinterpret_cast<std::uint64_t>(entry.BaseAddress),
					.allocationBase = reinterpret_cast<std::uint64_t>(entry.AllocationBase),
					.size = static_cast<std::uint64_t>(entry.RegionSize),
					.allocationProtect = entry.AllocationProtect,
					.state = entry.State,
					.protect = entry.Protect,
					.type = entry.Type,
					.imageSize = entry.SizeOfImage,
					.mappedFileName = copy_walk_string(entry.MappedFileName, entry.MappedFileNameLength)
				});
			}

			std::ranges::sort(a_state.regions, {}, &MemoryRegion::base);
			return {};
		}

		std::expected<void, Error> collect_threads(SnapshotState& a_state)
		{
			auto markerResult = WalkMarker::create(a_state.api);
			if (!markerResult)
				return std::unexpected(markerResult.error());
			auto marker = std::move(*markerResult);

			for (;;)
			{
				PSS_THREAD_ENTRY entry{};
				const auto error = a_state.api.walk_snapshot(
					a_state.snapshot,
					PSS_WALK_THREADS,
					marker.get(),
					std::addressof(entry),
					sizeof(entry));
				if (error == ERROR_NO_MORE_ITEMS)
					break;
				if (error != ERROR_SUCCESS)
				{
					return std::unexpected(make_error(
						ErrorCode::kWalkFailed,
						error,
						"PssWalkSnapshot(PSS_WALK_THREADS) failed"));
				}

				if (!entry.ContextRecord || entry.SizeOfContextRecord < sizeof(CONTEXT))
					continue;

				CapturedThread thread{
					.processId = entry.ProcessId,
					.threadId = entry.ThreadId,
					.tebAddress = reinterpret_cast<std::uint64_t>(entry.TebBaseAddress)
				};
				thread.contextBytes.resize(entry.SizeOfContextRecord);
				std::memcpy(thread.contextBytes.data(), entry.ContextRecord, entry.SizeOfContextRecord);
				std::memcpy(std::addressof(thread.context), thread.contextBytes.data(), sizeof(CONTEXT));

				TebStackPrefix stack{};
				auto stackRead = read_clone(
					a_state,
					thread.tebAddress,
					std::span<std::byte>{ reinterpret_cast<std::byte*>(std::addressof(stack)), sizeof(stack) });
				if (stackRead)
				{
					thread.stackLimit = reinterpret_cast<std::uint64_t>(stack.stackLimit);
					thread.stackBase = reinterpret_cast<std::uint64_t>(stack.stackBase);
					thread.hasStackBounds =
						thread.stackLimit < thread.stackBase &&
						thread.context.Rsp >= thread.stackLimit &&
						thread.context.Rsp < thread.stackBase;
				}

				a_state.threads.push_back(std::move(thread));
			}
			return {};
		}

		std::expected<void, Error> collect_modules(SnapshotState& a_state)
		{
			constexpr std::size_t kRuntimeFunctionBudget =
				32u * 1024u * 1024u;
			std::size_t runtimeFunctionBytes{};
			struct ModuleSeed
			{
				std::uint64_t size{};
				std::wstring path;
			};

			std::map<std::uint64_t, ModuleSeed> seeds;
			for (const auto& region : a_state.regions)
			{
				if (region.type != MEM_IMAGE || region.allocationBase == 0)
					continue;

				auto& seed = seeds[region.allocationBase];
				std::uint64_t regionEnd{};
				if (checked_end(region.base, region.size, regionEnd) && regionEnd >= region.allocationBase)
					seed.size = std::max(seed.size, regionEnd - region.allocationBase);
				seed.size = std::max(seed.size, static_cast<std::uint64_t>(region.imageSize));
				if (seed.path.empty() && !region.mappedFileName.empty())
					seed.path = region.mappedFileName;
			}

			for (auto& [base, seed] : seeds)
			{
				IMAGE_DOS_HEADER dos{};
				auto dosRead = read_clone(
					a_state,
					base,
					std::span<std::byte>{ reinterpret_cast<std::byte*>(std::addressof(dos)), sizeof(dos) });
				if (!dosRead || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0)
					continue;

				const auto ntAddress = base + static_cast<std::uint32_t>(dos.e_lfanew);
				IMAGE_NT_HEADERS64 nt{};
				auto ntRead = read_clone(
					a_state,
					ntAddress,
					std::span<std::byte>{ reinterpret_cast<std::byte*>(std::addressof(nt)), sizeof(nt) });
				if (!ntRead ||
					nt.Signature != IMAGE_NT_SIGNATURE ||
					nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
					nt.FileHeader.SizeOfOptionalHeader <
						offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory) +
							sizeof(IMAGE_DATA_DIRECTORY) * (IMAGE_DIRECTORY_ENTRY_EXCEPTION + 1) ||
					nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION ||
					nt.FileHeader.NumberOfSections == 0 ||
					nt.FileHeader.NumberOfSections > 96)
				{
					continue;
				}

				ModuleImage module{
					.base = base,
					.size = std::max(seed.size, static_cast<std::uint64_t>(nt.OptionalHeader.SizeOfImage)),
					.path = std::move(seed.path)
				};
				module.pathIsNtDevice = module.path.starts_with(LR"(\Device\)");

				const auto sectionAddress =
					ntAddress +
					sizeof(DWORD) +
					sizeof(IMAGE_FILE_HEADER) +
					nt.FileHeader.SizeOfOptionalHeader;
				module.sections.resize(nt.FileHeader.NumberOfSections);
				auto sectionsRead = read_clone(
					a_state,
					sectionAddress,
					std::span<std::byte>{
						reinterpret_cast<std::byte*>(module.sections.data()),
						module.sections.size() * sizeof(IMAGE_SECTION_HEADER) });
				if (!sectionsRead)
					continue;

				const auto& exceptionDirectory =
					nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
				if (exceptionDirectory.VirtualAddress != 0 &&
					exceptionDirectory.Size >= sizeof(RUNTIME_FUNCTION) &&
					exceptionDirectory.Size % sizeof(RUNTIME_FUNCTION) == 0 &&
					exceptionDirectory.VirtualAddress < module.size &&
					exceptionDirectory.Size <= module.size - exceptionDirectory.VirtualAddress)
				{
					const auto functionCount = exceptionDirectory.Size / sizeof(RUNTIME_FUNCTION);
					if (functionCount <= 1'000'000)
					{
						const auto requestedBytes =
							functionCount * sizeof(RUNTIME_FUNCTION);
						if (requestedBytes >
							kRuntimeFunctionBudget -
								std::min(
									runtimeFunctionBytes,
									kRuntimeFunctionBudget))
						{
							module.metadataPartial = true;
							module.metadataStatus =
								"global runtime-function metadata budget was reached";
							a_state.modules.push_back(std::move(module));
							continue;
						}
						std::vector<RUNTIME_FUNCTION> rawFunctions(functionCount);
						auto functionsRead = read_clone(
							a_state,
							module.base + exceptionDirectory.VirtualAddress,
							std::span<std::byte>{
								reinterpret_cast<std::byte*>(rawFunctions.data()),
								rawFunctions.size() * sizeof(RUNTIME_FUNCTION) });
						if (functionsRead)
						{
							runtimeFunctionBytes += requestedBytes;
							module.metadataProvenance = functionsRead->provenance;
							module.runtimeFunctions.reserve(rawFunctions.size());
							std::uint32_t previousBegin{};
							bool first = true;
							for (const auto& function : rawFunctions)
							{
								const auto unwindRva = function.UnwindData & ~std::uint32_t{ 3 };
								const bool valid =
									function.BeginAddress < function.EndAddress &&
									function.EndAddress <= module.size &&
									unwindRva != 0 &&
									unwindRva < module.size &&
									(first || function.BeginAddress >= previousBegin);
								if (valid)
								{
									module.runtimeFunctions.push_back(function);
									previousBegin = function.BeginAddress;
									first = false;
								}
								else
								{
									module.metadataPartial = true;
								}
							}
							if (module.metadataPartial)
								module.metadataStatus = "invalid or unsorted runtime-function entries were omitted";
						}
						else
						{
							module.metadataPartial = true;
							module.metadataStatus =
								"exception directory was not fully readable from the captured image";
						}
					}
					else
					{
						module.metadataPartial = true;
						module.metadataStatus = "exception directory exceeded the bounded entry count";
					}
				}
				else if (exceptionDirectory.VirtualAddress != 0 || exceptionDirectory.Size != 0)
				{
					module.metadataPartial = true;
					module.metadataStatus = "exception directory failed PE bounds validation";
				}

				a_state.modules.push_back(std::move(module));
			}

			std::ranges::sort(a_state.modules, {}, &ModuleImage::base);
			return {};
		}
	}

	std::expected<PssApi, Error> PssApi::load()
	{
		const auto module = ::GetModuleHandleW(L"kernel32.dll");
		if (!module)
		{
			return std::unexpected(make_error(
				ErrorCode::kUnavailable,
				::GetLastError(),
				"kernel32.dll is unavailable"));
		}
		return resolve(module);
	}

	std::expected<PssApi, Error> PssApi::resolve(HMODULE a_module)
	{
		if (!a_module)
		{
			return std::unexpected(make_error(
				ErrorCode::kUnavailable,
				ERROR_MOD_NOT_FOUND,
				"PSS API module is unavailable"));
		}

		PssApi api;
		api.m_capture = reinterpret_cast<CaptureSnapshot>(::GetProcAddress(a_module, "PssCaptureSnapshot"));
		api.m_free = reinterpret_cast<FreeSnapshot>(::GetProcAddress(a_module, "PssFreeSnapshot"));
		api.m_query = reinterpret_cast<QuerySnapshot>(::GetProcAddress(a_module, "PssQuerySnapshot"));
		api.m_walk = reinterpret_cast<WalkSnapshot>(::GetProcAddress(a_module, "PssWalkSnapshot"));
		api.m_markerCreate = reinterpret_cast<WalkMarkerCreate>(::GetProcAddress(a_module, "PssWalkMarkerCreate"));
		api.m_markerFree = reinterpret_cast<WalkMarkerFree>(::GetProcAddress(a_module, "PssWalkMarkerFree"));
		if (!api.m_capture || !api.m_free || !api.m_query || !api.m_walk ||
			!api.m_markerCreate || !api.m_markerFree)
		{
			return std::unexpected(make_error(
				ErrorCode::kUnavailable,
				ERROR_PROC_NOT_FOUND,
				"required Process Snapshotting API exports are unavailable"));
		}
		return api;
	}

	DWORD PssApi::capture_snapshot(
		HANDLE a_process,
		PSS_CAPTURE_FLAGS a_flags,
		DWORD a_contextFlags,
		HPSS* a_snapshot) const
	{
		return m_capture(a_process, a_flags, a_contextFlags, a_snapshot);
	}

	DWORD PssApi::free_snapshot(HANDLE a_process, HPSS a_snapshot) const
	{
		return m_free(a_process, a_snapshot);
	}

	DWORD PssApi::query_snapshot(
		HPSS a_snapshot,
		PSS_QUERY_INFORMATION_CLASS a_informationClass,
		void* a_buffer,
		DWORD a_bufferLength) const
	{
		return m_query(a_snapshot, a_informationClass, a_buffer, a_bufferLength);
	}

	DWORD PssApi::walk_snapshot(
		HPSS a_snapshot,
		PSS_WALK_INFORMATION_CLASS a_informationClass,
		HPSSWALK a_marker,
		void* a_buffer,
		DWORD a_bufferLength) const
	{
		return m_walk(a_snapshot, a_informationClass, a_marker, a_buffer, a_bufferLength);
	}

	DWORD PssApi::create_walk_marker(HPSSWALK* a_marker) const
	{
		return m_markerCreate(nullptr, a_marker);
	}

	DWORD PssApi::free_walk_marker(HPSSWALK a_marker) const
	{
		return m_markerFree(a_marker);
	}

	MemoryProvenance MemoryRegion::provenance() const noexcept
	{
		switch (type)
		{
		case MEM_PRIVATE:
			return MemoryProvenance::kPrivateFrozen;
		case MEM_IMAGE:
			return MemoryProvenance::kSharedImageWeak;
		case MEM_MAPPED:
			return MemoryProvenance::kSharedMappedWeak;
		default:
			return MemoryProvenance::kUnknownWeak;
		}
	}

	bool ModuleImage::contains(std::uint64_t a_address) const noexcept
	{
		std::uint64_t end{};
		return checked_end(base, size, end) && a_address >= base && a_address < end;
	}

	const RUNTIME_FUNCTION* ModuleImage::find_runtime_function(std::uint64_t a_address) const noexcept
	{
		if (!contains(a_address))
			return nullptr;

		const auto rva64 = a_address - base;
		if (rva64 > std::numeric_limits<std::uint32_t>::max())
			return nullptr;
		const auto rva = static_cast<std::uint32_t>(rva64);
		const auto it = std::upper_bound(
			runtimeFunctions.begin(),
			runtimeFunctions.end(),
			rva,
			[](std::uint32_t a_value, const RUNTIME_FUNCTION& a_function) {
				return a_value < a_function.BeginAddress;
			});
		if (it == runtimeFunctions.begin())
			return nullptr;
		const auto& function = *std::prev(it);
		return rva >= function.BeginAddress && rva < function.EndAddress ?
			std::addressof(function) :
			nullptr;
	}

	std::expected<ReadResult, Error> SnapshotReader::read(
		std::uint64_t a_address,
		std::span<std::byte> a_destination) const
	{
		const auto state = m_state.lock();
		if (!state)
		{
			return std::unexpected(make_error(
				ErrorCode::kLifetimeExpired,
				ERROR_INVALID_HANDLE,
				"snapshot owner has been released"));
		}
		return read_clone(*state, a_address, a_destination);
	}

	std::expected<SnapshotOperation, Error> SnapshotReader::begin_operation(
		std::size_t a_ownedByteBudget) const
	{
		const auto state = m_state.lock();
		if (!state)
		{
			return std::unexpected(make_error(
				ErrorCode::kLifetimeExpired,
				ERROR_INVALID_HANDLE,
				"snapshot owner has been released"));
		}
		if (a_ownedByteBudget == 0)
		{
			return std::unexpected(make_error(
				ErrorCode::kBudgetExceeded,
				ERROR_INSUFFICIENT_BUFFER,
				"snapshot operation cache budget is zero"));
		}
		try
		{
			return SnapshotOperation(state, a_ownedByteBudget);
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected(make_error(
				ErrorCode::kAllocationFailed,
				ERROR_NOT_ENOUGH_MEMORY,
				"failed to create snapshot operation cache"));
		}
	}

	struct SnapshotOperation::Cache
	{
		explicit Cache(std::size_t a_budget) :
			budget(a_budget)
		{
			SYSTEM_INFO information{};
			::GetSystemInfo(std::addressof(information));
			pageSize = information.dwPageSize != 0 ? information.dwPageSize : 4096;
		}

		std::size_t budget{};
		std::size_t used{};
		std::size_t pageSize{};
		std::unordered_map<std::uint64_t, CapturedSegment> pages;
	};

	SnapshotOperation::~SnapshotOperation() = default;
	SnapshotOperation::SnapshotOperation(SnapshotOperation&&) noexcept = default;
	SnapshotOperation& SnapshotOperation::operator=(SnapshotOperation&&) noexcept = default;

	SnapshotOperation::SnapshotOperation(
		std::shared_ptr<SnapshotState> a_state,
		std::size_t a_ownedByteBudget) :
		m_state(std::move(a_state)),
		m_cache(std::make_unique<Cache>(a_ownedByteBudget))
	{}

	std::expected<ReadResult, Error> SnapshotOperation::read(
		std::uint64_t a_address,
		std::span<std::byte> a_destination) const
	{
		if (!m_state)
		{
			return std::unexpected(make_error(
				ErrorCode::kLifetimeExpired,
				ERROR_INVALID_HANDLE,
				"snapshot operation has no owner"));
		}
		return read_clone(*m_state, a_address, a_destination);
	}

	std::expected<ReadResult, Error> SnapshotOperation::read_owned_page_cached(
		std::uint64_t a_address,
		std::span<std::byte> a_destination)
	{
		if (!m_state || !m_cache)
		{
			return std::unexpected(make_error(
				ErrorCode::kLifetimeExpired,
				ERROR_INVALID_HANDLE,
				"snapshot operation has no owner"));
		}
		if (a_destination.empty())
			return ReadResult{};
		std::uint64_t requestedEnd{};
		if (!checked_end(a_address, a_destination.size(), requestedEnd))
		{
			return std::unexpected(make_error(
				ErrorCode::kAddressOverflow,
				ERROR_ARITHMETIC_OVERFLOW,
				"owned-cache address range overflow"));
		}

		std::size_t completed = 0;
		auto provenance = MemoryProvenance::kPrivateFrozen;
		try
		{
			while (completed < a_destination.size())
			{
				const auto current = a_address + completed;
				const auto pageBase = current - current % m_cache->pageSize;
				auto page = m_cache->pages.find(pageBase);
				if (page == m_cache->pages.end())
				{
					Error regionError{};
					const auto* region = find_region_at(
						m_state->regions,
						current,
						std::addressof(regionError));
					if (!region)
					{
						regionError.bytesCompleted = completed;
						return std::unexpected(std::move(regionError));
					}
					if (region->type != MEM_IMAGE)
					{
						return std::unexpected(make_error(
							ErrorCode::kInvalidMetadata,
							ERROR_INVALID_DATA,
							"owned metadata cache only accepts captured image pages",
							completed));
					}

					std::uint64_t regionEnd{};
					if (!checked_end(region->base, region->size, regionEnd))
					{
						return std::unexpected(make_error(
							ErrorCode::kInvalidAddress,
							ERROR_ARITHMETIC_OVERFLOW,
							"captured image region range overflow",
							completed));
					}
					const auto pageEnd = std::min<std::uint64_t>(
						pageBase + m_cache->pageSize,
						regionEnd);
					const auto bytesToOwn = static_cast<std::size_t>(pageEnd - pageBase);
					if (bytesToOwn == 0 ||
						bytesToOwn > m_cache->budget - std::min(m_cache->used, m_cache->budget))
					{
						return std::unexpected(make_error(
							ErrorCode::kBudgetExceeded,
							ERROR_INSUFFICIENT_BUFFER,
							"snapshot operation exceeded its owned image-page budget",
							completed));
					}

					CapturedSegment segment{
						.base = pageBase,
						.bytes = std::vector<std::byte>(bytesToOwn)
					};
					auto copied = read_clone(*m_state, pageBase, segment.bytes);
					if (!copied)
					{
						auto error = copied.error();
						error.bytesCompleted += completed;
						return std::unexpected(std::move(error));
					}
					segment.sourceProvenance = copied->provenance;
					m_cache->used += segment.bytes.size();
					page = m_cache->pages.emplace(pageBase, std::move(segment)).first;
				}

				const auto& segment = page->second;
				const auto offset = static_cast<std::size_t>(current - segment.base);
				if (offset >= segment.bytes.size())
				{
					return std::unexpected(make_error(
						ErrorCode::kPartialRead,
						ERROR_PARTIAL_COPY,
						"owned image page does not cover the requested address",
						completed));
				}
				const auto chunk = std::min(
					a_destination.size() - completed,
					segment.bytes.size() - offset);
				std::memcpy(
					a_destination.data() + completed,
					segment.bytes.data() + offset,
					chunk);
				completed += chunk;
				const auto ownedProvenance =
					segment.sourceProvenance == MemoryProvenance::kPrivateFrozen ?
						MemoryProvenance::kPrivateFrozen :
						MemoryProvenance::kOwnedAtAnalysisFromSharedImage;
				provenance = combine_provenance(provenance, ownedProvenance);
			}
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected(make_error(
				ErrorCode::kAllocationFailed,
				ERROR_NOT_ENOUGH_MEMORY,
				"failed to allocate owned snapshot image page",
				completed));
		}

		return ReadResult{ .bytesRead = completed, .provenance = provenance };
	}

	const std::vector<MemoryRegion>& SnapshotOperation::regions() const noexcept
	{
		static const std::vector<MemoryRegion> empty;
		return m_state ? m_state->regions : empty;
	}

	const std::vector<CapturedThread>& SnapshotOperation::threads() const noexcept
	{
		static const std::vector<CapturedThread> empty;
		return m_state ? m_state->threads : empty;
	}

	const std::vector<ModuleImage>& SnapshotOperation::modules() const noexcept
	{
		static const std::vector<ModuleImage> empty;
		return m_state ? m_state->modules : empty;
	}

	std::size_t SnapshotOperation::owned_bytes() const noexcept
	{
		return m_cache ? m_cache->used : 0;
	}

	std::size_t SnapshotOperation::owned_byte_budget() const noexcept
	{
		return m_cache ? m_cache->budget : 0;
	}

	std::expected<ProcessSnapshot, Error> ProcessSnapshot::capture(
		const PssApi& a_api,
		const CaptureOptions& a_options)
	{
		try
		{
			auto state = std::make_shared<SnapshotState>(a_api);
			state->processId = ::GetCurrentProcessId();
			state->handleData = a_options.captureHandleData;

			auto flags = static_cast<PSS_CAPTURE_FLAGS>(
				PSS_CAPTURE_VA_CLONE |
				PSS_CAPTURE_THREADS |
				PSS_CAPTURE_THREAD_CONTEXT |
				PSS_CAPTURE_VA_SPACE |
				PSS_CAPTURE_VA_SPACE_SECTION_INFORMATION |
				PSS_CREATE_USE_VM_ALLOCATIONS);
			if (a_options.captureHandleData)
			{
				flags = static_cast<PSS_CAPTURE_FLAGS>(
					flags |
					PSS_CAPTURE_HANDLES |
					PSS_CAPTURE_HANDLE_NAME_INFORMATION |
					PSS_CAPTURE_HANDLE_BASIC_INFORMATION |
					PSS_CAPTURE_HANDLE_TYPE_SPECIFIC_INFORMATION);
			}

			const auto captureError = state->api.capture_snapshot(
				::GetCurrentProcess(),
				flags,
				CONTEXT_ALL,
				std::addressof(state->snapshot));
			if (captureError != ERROR_SUCCESS)
			{
				return std::unexpected(make_error(
					ErrorCode::kCaptureFailed,
					captureError,
					"PssCaptureSnapshot failed"));
			}
			++s_activeSnapshots;

			PSS_VA_CLONE_INFORMATION cloneInformation{};
			const auto queryError = state->api.query_snapshot(
				state->snapshot,
				PSS_QUERY_VA_CLONE_INFORMATION,
				std::addressof(cloneInformation),
				sizeof(cloneInformation));
			if (queryError != ERROR_SUCCESS || !cloneInformation.VaCloneHandle)
			{
				return std::unexpected(make_error(
					ErrorCode::kQueryFailed,
					queryError != ERROR_SUCCESS ? queryError : ERROR_INVALID_HANDLE,
					"PssQuerySnapshot(PSS_QUERY_VA_CLONE_INFORMATION) failed"));
			}
			state->clone = cloneInformation.VaCloneHandle;

			if (auto result = collect_regions(*state); !result)
				return std::unexpected(result.error());
			if (auto result = collect_threads(*state); !result)
				return std::unexpected(result.error());
			if (auto result = collect_modules(*state); !result)
				return std::unexpected(result.error());

			return ProcessSnapshot(std::move(state));
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected(make_error(
				ErrorCode::kAllocationFailed,
				ERROR_NOT_ENOUGH_MEMORY,
				"snapshot capture metadata allocation failed"));
		}
	}

	ProcessSnapshot::operator bool() const noexcept
	{
		return static_cast<bool>(m_state);
	}

	SnapshotReader ProcessSnapshot::reader() const noexcept
	{
		return SnapshotReader{ m_state };
	}

	const std::vector<MemoryRegion>& ProcessSnapshot::regions() const noexcept
	{
		static const std::vector<MemoryRegion> empty;
		return m_state ? m_state->regions : empty;
	}

	const std::vector<CapturedThread>& ProcessSnapshot::threads() const noexcept
	{
		static const std::vector<CapturedThread> empty;
		return m_state ? m_state->threads : empty;
	}

	const std::vector<ModuleImage>& ProcessSnapshot::modules() const noexcept
	{
		static const std::vector<ModuleImage> empty;
		return m_state ? m_state->modules : empty;
	}

	DWORD ProcessSnapshot::process_id() const noexcept
	{
		return m_state ? m_state->processId : 0;
	}

	bool ProcessSnapshot::captured_handle_data() const noexcept
	{
		return m_state && m_state->handleData;
	}

	std::expected<void, Error> ProcessSnapshot::write_minidump(
		const std::filesystem::path& a_path,
		MINIDUMP_TYPE a_type) const
	{
		if (!m_state)
		{
			return std::unexpected(make_error(
				ErrorCode::kLifetimeExpired,
				ERROR_INVALID_HANDLE,
				"cannot dump a released snapshot"));
		}
		if ((a_type & MiniDumpWithHandleData) != 0 && !m_state->handleData)
		{
			return std::unexpected(make_error(
				ErrorCode::kDumpFailed,
				ERROR_INVALID_PARAMETER,
				"handle-data dump requested from a snapshot captured without handles"));
		}

		const auto file = ::CreateFileW(
			a_path.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			return std::unexpected(make_error(
				ErrorCode::kIoFailed,
				::GetLastError(),
				"failed to create snapshot minidump file"));
		}

		MINIDUMP_CALLBACK_INFORMATION callback{
			.CallbackRoutine = snapshot_dump_callback,
			.CallbackParam = nullptr
		};
		const auto gate = DbgHelpGate::lock();
		const auto succeeded = ::MiniDumpWriteDump(
			reinterpret_cast<HANDLE>(m_state->snapshot),
			m_state->processId,
			file,
			a_type,
			nullptr,
			nullptr,
			std::addressof(callback));
		const auto error = succeeded ? ERROR_SUCCESS : ::GetLastError();
		::CloseHandle(file);
		if (!succeeded)
		{
			return std::unexpected(make_error(
				ErrorCode::kDumpFailed,
				error,
				"MiniDumpWriteDump from process snapshot failed"));
		}
		return {};
	}

	void ProcessSnapshot::reset() noexcept
	{
		m_state.reset();
	}

	CaptureDiagnostics capture_diagnostics() noexcept
	{
		return CaptureDiagnostics{
			.activeSnapshots = s_activeSnapshots.load(),
			.activeWalkMarkers = s_activeWalkMarkers.load(),
			.snapshotReleaseFailures = s_snapshotReleaseFailures.load(),
			.walkMarkerReleaseFailures = s_walkMarkerReleaseFailures.load(),
			.lastSnapshotReleaseError = s_lastSnapshotReleaseError.load(),
			.lastWalkMarkerReleaseError = s_lastWalkMarkerReleaseError.load()
		};
	}
}
