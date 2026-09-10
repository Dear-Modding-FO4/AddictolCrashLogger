#include "SettingsRepository.h"

#include <toml.hpp>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <limits>
#include <thread>
#include <type_traits>

namespace CrashUI
{
	namespace
	{
		std::atomic_uint64_t s_temporarySequence{};

		[[nodiscard]] size_t Index(SettingKey a_key) noexcept
		{
			return static_cast<size_t>(a_key);
		}

		struct SettingLocation
		{
			SettingKey key;
			const char* section;
			const char* name;
		};

		constexpr std::array kLocations{
			SettingLocation{ SettingKey::kEnableCrashLogger, "General", "bEnableCrashLogger" },
			SettingLocation{ SettingKey::kPrintSettings, "General", "bPrintSettings" },
			SettingLocation{ SettingKey::kAutoOpenLogs, "General", "bAutoOpenLogs" },
			SettingLocation{ SettingKey::kMaxCrashLogs, "General", "iMaxCrashLogs" },
			SettingLocation{ SettingKey::kMaxMiniDumps, "General", "iMaxMiniDumps" },
			SettingLocation{ SettingKey::kCrashLogDirectory, "Directories", "sCrashLogDirectory" },
			SettingLocation{ SettingKey::kSymcacheDirectory, "Directories", "sSymcacheDirectory" },
			SettingLocation{ SettingKey::kWaitForDebugger, "Debugging", "bWaitForDebugger" },
			SettingLocation{ SettingKey::kFullMemoryMiniDump, "Debugging", "bFullMemoryMiniDump" },
			SettingLocation{ SettingKey::kCrashLogWriteMiniDump, "Debugging", "bCrashLogWriteMiniDump" },
			SettingLocation{ SettingKey::kThreadDumpWriteMiniDump, "Debugging", "bThreadDumpWriteMiniDump" },
			SettingLocation{ SettingKey::kHeapAnalysis, "Debugging", "bHeapAnalysis" },
			SettingLocation{ SettingKey::kMaxHeapsToCheck, "Debugging", "iMaxHeapsToCheck" },
			SettingLocation{ SettingKey::kMaxHeapIterationsPerHeap, "Debugging", "iMaxHeapIterationsPerHeap" },
			SettingLocation{ SettingKey::kEnableThreadDumpHotkey, "Hotkeys", "bEnableThreadDumpHotkey" }
		};

		[[nodiscard]] std::optional<std::string> ReadFile(
			const std::filesystem::path& a_path,
			std::string& a_error)
		{
			std::error_code error;
			if (!std::filesystem::exists(a_path, error))
			{
				if (error)
					a_error = "Could not inspect " + a_path.string() + ".";
				return std::string{};
			}
			std::ifstream file{ a_path, std::ios::binary };
			if (!file)
			{
				a_error = "Could not read " + a_path.string() + ".";
				return std::nullopt;
			}
			std::string contents{
				std::istreambuf_iterator<char>{ file },
				std::istreambuf_iterator<char>{}
			};
			if (file.bad())
			{
				a_error = "Could not read the complete file " + a_path.string() + ".";
				return std::nullopt;
			}
			return contents;
		}

		[[nodiscard]] const toml::value* FindValue(
			const toml::value& a_root,
			const char* a_section,
			const char* a_name)
		{
			if (!a_root.is_table())
				return nullptr;
			const auto& root = a_root.as_table();
			const auto section = root.find(a_section);
			if (section == root.end() || !section->second.is_table())
				return nullptr;
			const auto& table = section->second.as_table();
			const auto value = table.find(a_name);
			return value == table.end() ? nullptr : &value->second;
		}

		template <class T>
		[[nodiscard]] bool AssignValue(
			const toml::value& a_root,
			const SettingLocation& a_location,
			T& a_destination,
			bool& a_present,
			std::string& a_error)
		{
			const auto* value = FindValue(
				a_root, a_location.section, a_location.name);
			if (!value)
			{
				a_present = false;
				return true;
			}
			a_present = true;
			try
			{
				if constexpr (std::is_same_v<T, bool>)
				{
					if (!value->is_boolean())
						throw std::runtime_error("expected boolean");
					a_destination = value->as_boolean();
				}
				else if constexpr (std::is_same_v<T, int32_t>)
				{
					if (!value->is_integer())
						throw std::runtime_error("expected integer");
					const auto parsed = value->as_integer();
					if (parsed < (std::numeric_limits<int32_t>::min)() ||
						parsed > (std::numeric_limits<int32_t>::max)())
						throw std::runtime_error("integer is outside 32-bit range");
					a_destination = static_cast<int32_t>(parsed);
				}
				else
				{
					if (!value->is_string())
						throw std::runtime_error("expected string");
					a_destination = value->as_string();
				}
				return true;
			}
			catch (const std::exception& exception)
			{
				a_error = std::string{ a_location.section } + "." +
					a_location.name + ": " + exception.what();
				return false;
			}
		}

		[[nodiscard]] bool ApplyToml(
			const toml::value& a_root,
			Settings::Values& a_values,
			std::array<SettingSource, static_cast<size_t>(SettingKey::kCount)>* a_sources,
			SettingSource a_source,
			std::string& a_error)
		{
			for (const auto& location : kLocations)
			{
				bool present{};
				bool success{};
				switch (location.key)
				{
				case SettingKey::kEnableCrashLogger:
					success = AssignValue(a_root, location, a_values.enableCrashLogger, present, a_error);
					break;
				case SettingKey::kPrintSettings:
					success = AssignValue(a_root, location, a_values.printSettings, present, a_error);
					break;
				case SettingKey::kAutoOpenLogs:
					success = AssignValue(a_root, location, a_values.autoOpenLogs, present, a_error);
					break;
				case SettingKey::kMaxCrashLogs:
					success = AssignValue(a_root, location, a_values.maxCrashLogs, present, a_error);
					break;
				case SettingKey::kMaxMiniDumps:
					success = AssignValue(a_root, location, a_values.maxMiniDumps, present, a_error);
					break;
				case SettingKey::kCrashLogDirectory:
					success = AssignValue(a_root, location, a_values.crashLogDirectory, present, a_error);
					break;
				case SettingKey::kSymcacheDirectory:
					success = AssignValue(a_root, location, a_values.symcacheDirectory, present, a_error);
					break;
				case SettingKey::kWaitForDebugger:
					success = AssignValue(a_root, location, a_values.waitForDebugger, present, a_error);
					break;
				case SettingKey::kFullMemoryMiniDump:
					success = AssignValue(a_root, location, a_values.fullMemoryMiniDump, present, a_error);
					break;
				case SettingKey::kCrashLogWriteMiniDump:
					success = AssignValue(a_root, location, a_values.crashLogWriteMiniDump, present, a_error);
					break;
				case SettingKey::kThreadDumpWriteMiniDump:
					success = AssignValue(a_root, location, a_values.threadDumpWriteMiniDump, present, a_error);
					break;
				case SettingKey::kHeapAnalysis:
					success = AssignValue(a_root, location, a_values.heapAnalysis, present, a_error);
					break;
				case SettingKey::kMaxHeapsToCheck:
					success = AssignValue(a_root, location, a_values.maxHeapsToCheck, present, a_error);
					break;
				case SettingKey::kMaxHeapIterationsPerHeap:
					success = AssignValue(a_root, location, a_values.maxHeapIterationsPerHeap, present, a_error);
					break;
				case SettingKey::kEnableThreadDumpHotkey:
					success = AssignValue(a_root, location, a_values.enableThreadDumpHotkey, present, a_error);
					break;
				default:
					success = false;
					break;
				}
				if (!success)
					return false;
				if (present && a_sources)
					(*a_sources)[Index(location.key)] = a_source;
			}
			return true;
		}

		template <class T>
		void SetToml(
			toml::value& a_root,
			const SettingLocation& a_location,
			const T& a_value)
		{
			auto& root = a_root.as_table();
			auto section = root.find(a_location.section);
			if (section == root.end())
			{
				section = root.emplace(
					a_location.section,
					toml::value{ toml::table{} }).first;
			}
			section->second.as_table().insert_or_assign(
				a_location.name,
				toml::value{ a_value });
		}

		void RemoveToml(
			toml::value& a_root,
			const SettingLocation& a_location)
		{
			auto& root = a_root.as_table();
			const auto section = root.find(a_location.section);
			if (section == root.end() || !section->second.is_table())
				return;
			section->second.as_table().erase(a_location.name);
			if (section->second.as_table().empty())
				root.erase(section);
		}

		template <class T>
		void UpdateOwnedValue(
			toml::value& a_root,
			const SettingLocation& a_location,
			const T& a_base,
			const T& a_draft)
		{
			if (a_base == a_draft)
				RemoveToml(a_root, a_location);
			else
				SetToml(a_root, a_location, a_draft);
		}

		[[nodiscard]] bool IsPathSyntaxValid(
			std::string_view a_path,
			std::string& a_error)
		{
			if (a_path.find('\0') != std::string_view::npos)
			{
				a_error = "Path contains an embedded NUL.";
				return false;
			}
			for (size_t i = 0; i < a_path.size(); ++i)
			{
				const auto character = a_path[i];
				if (character == '<' || character == '>' || character == '"' ||
					character == '|' || character == '?' || character == '*' ||
					(character == ':' && i != 1))
				{
					a_error = "Path contains a character Windows does not allow.";
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] std::filesystem::path TemporaryPath(
			const std::filesystem::path& a_target)
		{
			auto name = a_target.filename().wstring();
			name += L".tmp.";
			name += std::to_wstring(GetCurrentProcessId());
			name += L".";
			name += std::to_wstring(
				s_temporarySequence.fetch_add(1, std::memory_order_relaxed));
			return a_target.parent_path() / name;
		}

		[[nodiscard]] bool WriteReplacement(
			const std::filesystem::path& a_target,
			std::string_view a_contents,
			std::string& a_error)
		{
			std::error_code error;
			if (a_target.parent_path().empty() ||
				!std::filesystem::is_directory(a_target.parent_path(), error) ||
				error)
			{
				a_error = "The logical custom-settings directory is unavailable; saving is disabled.";
				return false;
			}
			const auto temporary = TemporaryPath(a_target);
			{
				std::ofstream file{
					temporary,
					std::ios::binary | std::ios::trunc
				};
				if (!file)
				{
					a_error = "Could not create a same-directory temporary settings file.";
					return false;
				}
				file.write(
					a_contents.data(),
					static_cast<std::streamsize>(a_contents.size()));
				file.flush();
				if (!file)
				{
					file.close();
					std::filesystem::remove(temporary, error);
					a_error = "Could not complete the temporary settings file.";
					return false;
				}
			}
			if (MoveFileExW(
					temporary.c_str(),
					a_target.c_str(),
					MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				std::string readError;
				const auto readback = ReadFile(a_target, readError);
				if (readback && *readback == a_contents)
					return true;
				a_error =
					"Custom settings replacement returned success but read-back "
					"did not confirm the intended file.";
				return false;
			}

			const auto nativeError = GetLastError();
			std::filesystem::remove(temporary, error);
			std::string readError;
			const auto readback = ReadFile(a_target, readError);
			if (readback && *readback == a_contents)
				return true;
			a_error = "Custom settings replacement was not confirmed (Windows error " +
				std::to_string(nativeError) + "). " +
				(readError.empty() ? "The prior file was preserved or the resulting state is unchanged." :
					readError);
			return false;
		}

		template <class T>
		void MarkIfDifferent(
			DirtySettings& a_dirty,
			SettingKey a_key,
			const T& a_left,
			const T& a_right)
		{
			a_dirty.set(Index(a_key), a_left != a_right);
		}
	}

	SourceFingerprint FingerprintFile(
		const std::filesystem::path& a_path,
		std::string* a_error)
	{
		SourceFingerprint result;
		if (a_error)
			a_error->clear();
		std::error_code error;
		result.exists = std::filesystem::exists(a_path, error);
		if (error)
		{
			if (a_error)
				*a_error = "Could not inspect " + a_path.string() + ".";
			return result;
		}
		if (!result.exists)
			return result;
		result.size = std::filesystem::file_size(a_path, error);
		if (error)
		{
			if (a_error)
				*a_error = "Could not inspect the size of " + a_path.string() + ".";
			return result;
		}
		const auto writeTime = std::filesystem::last_write_time(a_path, error);
		if (error)
		{
			if (a_error)
				*a_error = "Could not inspect the write time of " + a_path.string() + ".";
			return result;
		}
		result.writeTime = static_cast<uint64_t>(
			writeTime.time_since_epoch().count());
		std::string readError;
		const auto contents = ReadFile(a_path, readError);
		if (!contents)
		{
			if (a_error)
				*a_error = std::move(readError);
			return result;
		}
		uint64_t hash = 1469598103934665603ull;
		for (const auto character : *contents)
		{
			hash ^= static_cast<unsigned char>(character);
			hash *= 1099511628211ull;
		}
		result.contentHash = hash;
		return result;
	}

	SettingsSnapshot LoadSettingsFiles(
		const std::filesystem::path& a_basePath,
		const std::filesystem::path& a_customPath,
		uint64_t a_generation,
		const std::filesystem::path& a_defaultReportDirectory)
	{
		SettingsSnapshot result;
		result.generation = a_generation;
		result.basePath = a_basePath;
		result.customPath = a_customPath;
		result.sources.fill(SettingSource::kBuiltIn);
		std::string error;
		result.baseFingerprint = FingerprintFile(a_basePath, &error);
		if (!error.empty())
		{
			result.configurationValid = false;
			result.error = std::move(error);
			return result;
		}
		result.customFingerprint = FingerprintFile(a_customPath, &error);
		if (!error.empty())
		{
			result.configurationValid = false;
			result.error = std::move(error);
			return result;
		}
		const auto load = [&](const std::filesystem::path& a_path)
			-> std::optional<toml::value> {
			auto contents = ReadFile(a_path, error);
			if (!contents)
				return std::nullopt;
			if (contents->empty())
				return toml::value{ toml::table{} };
			auto parsed = toml::try_parse_str(*contents);
			if (!parsed.is_ok())
			{
				error = a_path.string() + " is malformed TOML.";
				return std::nullopt;
			}
			auto value = std::move(parsed).unwrap();
			if (!value.is_table())
			{
				error = a_path.string() + " does not contain a TOML table.";
				return std::nullopt;
			}
			return value;
		};
		const auto base = load(a_basePath);
		if (!base)
		{
			result.configurationValid = false;
			result.error = std::move(error);
			return result;
		}
		result.baseValues = Settings::kBuiltInDefaults;
		if (!ApplyToml(
				*base,
				result.baseValues,
				&result.sources,
				SettingSource::kBase,
				error))
		{
			result.configurationValid = false;
			result.error = a_basePath.string() + ": " + error;
			return result;
		}
		{
			bool present{};
			const SettingLocation upload{
				SettingKey::kCount,
				"Pastebin",
				"bAutoUploadCrashLog"
			};
			if (!AssignValue(
					*base,
					upload,
					result.baseValues.autoUploadCrashLog,
					present,
					error))
			{
				result.configurationValid = false;
				result.error = a_basePath.string() + ": " + error;
				return result;
			}
		}
		const auto custom = load(a_customPath);
		if (!custom)
		{
			result.configurationValid = false;
			result.error = std::move(error);
			return result;
		}
		result.savedValues = result.baseValues;
		if (!ApplyToml(
				*custom,
				result.savedValues,
				&result.sources,
				SettingSource::kCustom,
				error))
		{
			result.configurationValid = false;
			result.error = a_customPath.string() + ": " + error;
			return result;
		}
		{
			bool present{};
			const SettingLocation upload{
				SettingKey::kCount,
				"Pastebin",
				"bAutoUploadCrashLog"
			};
			if (!AssignValue(
					*custom,
					upload,
					result.savedValues.autoUploadCrashLog,
					present,
					error))
			{
				result.configurationValid = false;
				result.error = a_customPath.string() + ": " + error;
				return result;
			}
		}
		if (result.savedValues.maxCrashLogs <= 0)
			result.savedValues.maxCrashLogs = 0;
		if (result.savedValues.maxMiniDumps <= 0)
			result.savedValues.maxMiniDumps = 0;

		const auto defaultDirectory = a_defaultReportDirectory;
		result.effectiveReportDirectory = defaultDirectory;
		if (result.savedValues.crashLogDirectory.empty())
		{
			result.reportDirectoryDetail =
				"Empty setting uses the F4SE log directory.";
		}
		else
		{
			std::error_code pathError;
			const std::filesystem::path configured{
				result.savedValues.crashLogDirectory
			};
			if (std::filesystem::is_directory(configured, pathError) && !pathError)
			{
				result.effectiveReportDirectory =
					std::filesystem::absolute(configured).lexically_normal();
				result.reportDirectoryDetail = "Configured directory is currently valid.";
			}
			else
			{
				result.reportDirectoryDetail =
					"Configured directory is unavailable; next launch would fall back to " +
					defaultDirectory.string() + ".";
			}
		}
		return result;
	}

	bool ValidateSettingsDraft(
		const Settings::Values& a_values,
		const DirtySettings& a_dirty,
		std::string& a_error)
	{
		a_error.clear();
		if (a_values.maxHeapsToCheck < 1)
		{
			a_error = "Maximum heaps to check must be at least 1.";
			return false;
		}
		if (a_values.maxHeapIterationsPerHeap < 1)
		{
			a_error = "Maximum heap iterations must be at least 1.";
			return false;
		}
		const auto checkDirectory = [&](SettingKey a_key, const std::string& a_value,
			bool a_allowEmpty, const char* a_label) {
			if (!a_dirty.test(Index(a_key)))
				return true;
			if (a_value.empty() && a_allowEmpty)
				return true;
			if (a_value.empty())
			{
				a_error = std::string{ a_label } + " cannot be empty.";
				return false;
			}
			if (!IsPathSyntaxValid(a_value, a_error))
			{
				a_error = std::string{ a_label } + ": " + a_error;
				return false;
			}
			std::error_code error;
			if (!std::filesystem::is_directory(
					std::filesystem::path{ a_value }, error) || error)
			{
				a_error = std::string{ a_label } +
					" must name an existing directory.";
				return false;
			}
			return true;
		};
		return checkDirectory(
				SettingKey::kCrashLogDirectory,
				a_values.crashLogDirectory,
				true,
				"Report directory") &&
			checkDirectory(
				SettingKey::kSymcacheDirectory,
				a_values.symcacheDirectory,
				false,
				"Symbol-cache directory");
	}

	bool BuildCustomSettingsToml(
		std::string_view a_existing,
		const Settings::Values& a_baseValues,
		const Settings::Values& a_draft,
		const DirtySettings& a_dirty,
		std::string& a_output,
		std::string& a_error)
	{
		try
		{
			toml::value root{ toml::table{} };
			if (!a_existing.empty())
			{
				auto parsed = toml::try_parse_str(std::string{ a_existing });
				if (!parsed.is_ok())
				{
					a_error = "Existing custom settings TOML is malformed.";
					return false;
				}
				root = std::move(parsed).unwrap();
				if (!root.is_table())
				{
					a_error = "Existing custom settings root is not a TOML table.";
					return false;
				}
			}
			for (const auto& location : kLocations)
			{
				if (!a_dirty.test(Index(location.key)))
					continue;
				switch (location.key)
				{
				case SettingKey::kEnableCrashLogger:
					UpdateOwnedValue(root, location, a_baseValues.enableCrashLogger, a_draft.enableCrashLogger);
					break;
				case SettingKey::kPrintSettings:
					UpdateOwnedValue(root, location, a_baseValues.printSettings, a_draft.printSettings);
					break;
				case SettingKey::kAutoOpenLogs:
					UpdateOwnedValue(root, location, a_baseValues.autoOpenLogs, a_draft.autoOpenLogs);
					break;
				case SettingKey::kMaxCrashLogs:
					UpdateOwnedValue(root, location, a_baseValues.maxCrashLogs, a_draft.maxCrashLogs);
					break;
				case SettingKey::kMaxMiniDumps:
					UpdateOwnedValue(root, location, a_baseValues.maxMiniDumps, a_draft.maxMiniDumps);
					break;
				case SettingKey::kCrashLogDirectory:
					UpdateOwnedValue(root, location, a_baseValues.crashLogDirectory, a_draft.crashLogDirectory);
					break;
				case SettingKey::kSymcacheDirectory:
					UpdateOwnedValue(root, location, a_baseValues.symcacheDirectory, a_draft.symcacheDirectory);
					break;
				case SettingKey::kWaitForDebugger:
					UpdateOwnedValue(root, location, a_baseValues.waitForDebugger, a_draft.waitForDebugger);
					break;
				case SettingKey::kFullMemoryMiniDump:
					UpdateOwnedValue(root, location, a_baseValues.fullMemoryMiniDump, a_draft.fullMemoryMiniDump);
					break;
				case SettingKey::kCrashLogWriteMiniDump:
					UpdateOwnedValue(root, location, a_baseValues.crashLogWriteMiniDump, a_draft.crashLogWriteMiniDump);
					break;
				case SettingKey::kThreadDumpWriteMiniDump:
					UpdateOwnedValue(root, location, a_baseValues.threadDumpWriteMiniDump, a_draft.threadDumpWriteMiniDump);
					break;
				case SettingKey::kHeapAnalysis:
					UpdateOwnedValue(root, location, a_baseValues.heapAnalysis, a_draft.heapAnalysis);
					break;
				case SettingKey::kMaxHeapsToCheck:
					UpdateOwnedValue(root, location, a_baseValues.maxHeapsToCheck, a_draft.maxHeapsToCheck);
					break;
				case SettingKey::kMaxHeapIterationsPerHeap:
					UpdateOwnedValue(root, location, a_baseValues.maxHeapIterationsPerHeap, a_draft.maxHeapIterationsPerHeap);
					break;
				case SettingKey::kEnableThreadDumpHotkey:
					UpdateOwnedValue(root, location, a_baseValues.enableThreadDumpHotkey, a_draft.enableThreadDumpHotkey);
					break;
				default:
					break;
				}
			}
			a_output = toml::format(root);
			if (!a_output.empty() && a_output.back() != '\n')
				a_output.push_back('\n');
			a_error.clear();
			return true;
		}
		catch (const std::exception& exception)
		{
			a_error = exception.what();
			return false;
		}
	}

	SettingsSaveResult SaveSettingsFiles(
		const SettingsSnapshot& a_snapshot,
		const SettingsSaveRequest& a_request)
	{
		SettingsSaveResult result;
		result.operation = a_request.operation;
		std::string error;
		if (!a_snapshot.configurationValid)
		{
			result.message = "Configuration is malformed; save was refused.";
			return result;
		}
		if (!ValidateSettingsDraft(a_request.draft, a_request.dirty, error))
		{
			result.message = std::move(error);
			return result;
		}
		const auto baseFingerprint = FingerprintFile(a_snapshot.basePath, &error);
		if (!error.empty() || !(baseFingerprint == a_request.baseFingerprint))
		{
			result.conflict = true;
			result.message = "Base settings changed externally; reload and review.";
			return result;
		}
		const auto customFingerprint = FingerprintFile(a_snapshot.customPath, &error);
		if (!error.empty() || !(customFingerprint == a_request.customFingerprint))
		{
			result.conflict = true;
			result.message = "Custom settings changed externally; reload and review.";
			return result;
		}
		const auto existing = ReadFile(a_snapshot.customPath, error);
		if (!existing)
		{
			result.message = std::move(error);
			return result;
		}
		std::string output;
		if (!BuildCustomSettingsToml(
				*existing,
				a_snapshot.baseValues,
				a_request.draft,
				a_request.dirty,
				output,
				error))
		{
			result.message = std::move(error);
			return result;
		}
		const auto finalBaseFingerprint = FingerprintFile(a_snapshot.basePath, &error);
		const auto finalCustomFingerprint = FingerprintFile(a_snapshot.customPath, &error);
		if (!error.empty() ||
			!(finalBaseFingerprint == a_request.baseFingerprint) ||
			!(finalCustomFingerprint == a_request.customFingerprint))
		{
			result.conflict = true;
			result.message = "Settings changed before replacement; reload and review.";
			return result;
		}
		if (!WriteReplacement(a_snapshot.customPath, output, error))
		{
			result.message = std::move(error);
			return result;
		}
		result.success = true;
		result.changed = a_request.dirty.count();
		result.message = "Settings saved for the next launch.";
		return result;
	}

	void SettingsPageState::Activate(const SettingsSnapshot& a_snapshot)
	{
		if (m_savePending)
		{
			m_active = true;
			return;
		}
		m_saved = a_snapshot.savedValues;
		m_draft = m_saved;
		m_dirty.reset();
		m_baseFingerprint = a_snapshot.baseFingerprint;
		m_customFingerprint = a_snapshot.customFingerprint;
		m_active = true;
	}

	void SettingsPageState::Deactivate()
	{
		m_active = false;
		if (!m_savePending)
		{
			m_draft = m_saved;
			m_dirty.reset();
		}
	}

	void SettingsPageState::Edit(SettingKey a_key)
	{
		bool different{};
		switch (a_key)
		{
		case SettingKey::kEnableCrashLogger:
			different = m_draft.enableCrashLogger != m_saved.enableCrashLogger;
			break;
		case SettingKey::kPrintSettings:
			different = m_draft.printSettings != m_saved.printSettings;
			break;
		case SettingKey::kAutoOpenLogs:
			different = m_draft.autoOpenLogs != m_saved.autoOpenLogs;
			break;
		case SettingKey::kMaxCrashLogs:
			different = m_draft.maxCrashLogs != m_saved.maxCrashLogs;
			break;
		case SettingKey::kMaxMiniDumps:
			different = m_draft.maxMiniDumps != m_saved.maxMiniDumps;
			break;
		case SettingKey::kCrashLogDirectory:
			different = m_draft.crashLogDirectory != m_saved.crashLogDirectory;
			break;
		case SettingKey::kSymcacheDirectory:
			different = m_draft.symcacheDirectory != m_saved.symcacheDirectory;
			break;
		case SettingKey::kWaitForDebugger:
			different = m_draft.waitForDebugger != m_saved.waitForDebugger;
			break;
		case SettingKey::kFullMemoryMiniDump:
			different = m_draft.fullMemoryMiniDump != m_saved.fullMemoryMiniDump;
			break;
		case SettingKey::kCrashLogWriteMiniDump:
			different = m_draft.crashLogWriteMiniDump != m_saved.crashLogWriteMiniDump;
			break;
		case SettingKey::kThreadDumpWriteMiniDump:
			different = m_draft.threadDumpWriteMiniDump != m_saved.threadDumpWriteMiniDump;
			break;
		case SettingKey::kHeapAnalysis:
			different = m_draft.heapAnalysis != m_saved.heapAnalysis;
			break;
		case SettingKey::kMaxHeapsToCheck:
			different = m_draft.maxHeapsToCheck != m_saved.maxHeapsToCheck;
			break;
		case SettingKey::kMaxHeapIterationsPerHeap:
			different =
				m_draft.maxHeapIterationsPerHeap !=
				m_saved.maxHeapIterationsPerHeap;
			break;
		case SettingKey::kEnableThreadDumpHotkey:
			different =
				m_draft.enableThreadDumpHotkey !=
				m_saved.enableThreadDumpHotkey;
			break;
		default:
			break;
		}
		m_dirty.set(Index(a_key), different);
	}

	void SettingsPageState::Reset()
	{
		if (m_savePending)
			return;
		m_draft = Settings::kBuiltInDefaults;
		m_draft.autoUploadCrashLog = m_saved.autoUploadCrashLog;
		m_dirty.reset();
		for (size_t index = 0;
			 index < static_cast<size_t>(SettingKey::kCount);
			 ++index)
			Edit(static_cast<SettingKey>(index));
	}

	void SettingsPageState::Revert()
	{
		if (m_savePending)
			return;
		m_draft = m_saved;
		m_dirty.reset();
	}

	std::optional<SettingsSaveRequest> SettingsPageState::AcceptApply(
		uint64_t a_operation,
		std::string& a_error)
	{
		if (m_savePending || m_dirty.none())
			return std::nullopt;
		a_error.clear();
		m_savePending = true;
		m_pendingOperation = a_operation;
		return SettingsSaveRequest{
			a_operation,
			m_draft,
			m_dirty,
			m_baseFingerprint,
			m_customFingerprint
		};
	}

	void SettingsPageState::CancelApply(uint64_t a_operation)
	{
		if (m_savePending && m_pendingOperation == a_operation)
		{
			m_savePending = false;
			m_pendingOperation = 0;
		}
	}

	void SettingsPageState::Reconcile(
		const SettingsSnapshot& a_snapshot,
		const SettingsSaveResult* a_result)
	{
		if (!m_savePending || !a_result ||
			a_result->operation != m_pendingOperation)
			return;
		m_savePending = false;
		m_pendingOperation = 0;
		m_active = true;
		if (a_result->success)
		{
			m_saved = a_snapshot.savedValues;
			m_draft = m_saved;
			m_dirty.reset();
			m_baseFingerprint = a_snapshot.baseFingerprint;
			m_customFingerprint = a_snapshot.customFingerprint;
		}
	}

	bool SettingsPageState::Active() const noexcept
	{
		return m_active;
	}

	bool SettingsPageState::SavePending() const noexcept
	{
		return m_savePending;
	}

	Settings::Values& SettingsPageState::Draft() noexcept
	{
		return m_draft;
	}

	const Settings::Values& SettingsPageState::Draft() const noexcept
	{
		return m_draft;
	}

	const DirtySettings& SettingsPageState::Dirty() const noexcept
	{
		return m_dirty;
	}

	SettingsRepository& SettingsRepository::GetSingleton()
	{
		static auto* singleton = new SettingsRepository;
		return *singleton;
	}

	SettingsRepository::SettingsRepository() :
		m_snapshot(std::make_shared<const SettingsSnapshot>())
	{
		std::thread([this] { Worker(); }).detach();
	}

	void SettingsRepository::SetDefaultReportDirectory(
		const std::filesystem::path& a_directory)
	{
		const std::scoped_lock lock{ m_mutex };
		m_defaultReportDirectory = a_directory;
	}

	void SettingsRepository::RequestLoad()
	{
		{
			const std::scoped_lock lock{ m_mutex };
			m_loadPending = true;
			auto loading = std::make_shared<SettingsSnapshot>(*m_snapshot);
			loading->loading = true;
			loading->error.clear();
			m_snapshot = std::move(loading);
		}
		m_condition.notify_one();
	}

	bool SettingsRepository::RequestSave(
		const SettingsSaveRequest& a_request)
	{
		{
			const std::scoped_lock lock{ m_mutex };
			if (m_saveInFlight || m_pendingSave)
				return false;
			m_pendingSave = a_request;
			auto pending = std::make_shared<SettingsSnapshot>(*m_snapshot);
			pending->savePending = true;
			pending->saveOperation = a_request.operation;
			pending->saveMessage = "Saving settings for the next launch...";
			m_snapshot = std::move(pending);
		}
		m_condition.notify_one();
		return true;
	}

	SettingsRepositoryState SettingsRepository::Snapshot() const
	{
		const std::scoped_lock lock{ m_mutex };
		return { m_snapshot, m_lastSaveResult };
	}

	void SettingsRepository::Worker()
	{
		for (;;)
		{
			std::optional<SettingsSaveRequest> save;
			bool load{};
			std::shared_ptr<const SettingsSnapshot> snapshot;
			std::filesystem::path defaultReportDirectory;
			{
				std::unique_lock lock{ m_mutex };
				m_condition.wait(lock, [this] {
					return m_loadPending || m_pendingSave.has_value();
				});
				if (m_pendingSave)
				{
					save = std::move(m_pendingSave);
					m_pendingSave.reset();
					m_saveInFlight = true;
					snapshot = m_snapshot;
				}
				else
				{
					load = true;
					m_loadPending = false;
				}
				defaultReportDirectory = m_defaultReportDirectory;
			}
			if (save)
			{
				const auto result = SaveSettingsFiles(*snapshot, *save);
				auto reloaded = LoadSettingsFiles(
					std::filesystem::path{ kBaseSettingsPath },
					std::filesystem::path{ kCustomSettingsPath },
					++m_generation,
					defaultReportDirectory);
				reloaded.savePending = false;
				reloaded.saveOperation = result.operation;
				reloaded.lastSaveSucceeded = result.success;
				reloaded.saveMessage = result.message;
				{
					const std::scoped_lock lock{ m_mutex };
					m_snapshot =
						std::make_shared<const SettingsSnapshot>(std::move(reloaded));
					m_lastSaveResult = result;
					m_saveInFlight = false;
				}
				continue;
			}
			if (load)
			{
				auto loaded = LoadSettingsFiles(
					std::filesystem::path{ kBaseSettingsPath },
					std::filesystem::path{ kCustomSettingsPath },
					++m_generation,
					defaultReportDirectory);
				const std::scoped_lock lock{ m_mutex };
				m_snapshot =
					std::make_shared<const SettingsSnapshot>(std::move(loaded));
			}
		}
	}
}
