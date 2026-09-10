#pragma once

#include "SettingsValues.h"

#include <array>
#include <bitset>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace CrashUI
{
	inline constexpr std::string_view kBaseSettingsPath{
		"Data/F4SE/Plugins/AddictolCrashLogger.toml"
	};
	inline constexpr std::string_view kCustomSettingsPath{
		"Data/F4SE/Plugins/AddictolCrashLoggerCustom.toml"
	};

	enum class SettingKey : size_t
	{
		kEnableCrashLogger,
		kPrintSettings,
		kAutoOpenLogs,
		kMaxCrashLogs,
		kMaxMiniDumps,
		kCrashLogDirectory,
		kSymcacheDirectory,
		kWaitForDebugger,
		kFullMemoryMiniDump,
		kCrashLogWriteMiniDump,
		kThreadDumpWriteMiniDump,
		kEnableThreadDumpHotkey,
		kCount
	};

	using DirtySettings = std::bitset<static_cast<size_t>(SettingKey::kCount)>;

	enum class SettingSource
	{
		kBuiltIn,
		kBase,
		kCustom
	};

	struct SourceFingerprint
	{
		bool exists{};
		uint64_t size{};
		uint64_t writeTime{};
		uint64_t contentHash{};

		bool operator==(const SourceFingerprint&) const noexcept = default;
	};

	struct SettingsSnapshot
	{
		uint64_t generation{};
		Settings::Values baseValues{ Settings::kBuiltInDefaults };
		Settings::Values savedValues{ Settings::kBuiltInDefaults };
		std::array<SettingSource, static_cast<size_t>(SettingKey::kCount)> sources{};
		std::filesystem::path basePath;
		std::filesystem::path customPath;
		SourceFingerprint baseFingerprint;
		SourceFingerprint customFingerprint;
		std::filesystem::path effectiveReportDirectory;
		std::string reportDirectoryDetail;
		bool loading{};
		bool configurationValid{ true };
		bool savePending{};
		bool lastSaveSucceeded{};
		uint64_t saveOperation{};
		std::string error;
		std::string saveMessage;
	};

	struct SettingsSaveRequest
	{
		uint64_t operation{};
		Settings::Values draft;
		DirtySettings dirty;
		SourceFingerprint baseFingerprint;
		SourceFingerprint customFingerprint;
	};

	struct SettingsSaveResult
	{
		uint64_t operation{};
		bool success{};
		bool conflict{};
		size_t changed{};
		std::string message;
	};

	struct SettingsRepositoryState
	{
		std::shared_ptr<const SettingsSnapshot> settings;
		std::optional<SettingsSaveResult> saveResult;
	};

	[[nodiscard]] SourceFingerprint FingerprintFile(
		const std::filesystem::path& a_path,
		std::string* a_error = nullptr);
	[[nodiscard]] SettingsSnapshot LoadSettingsFiles(
		const std::filesystem::path& a_basePath,
		const std::filesystem::path& a_customPath,
		uint64_t a_generation,
		const std::filesystem::path& a_defaultReportDirectory = {});
	[[nodiscard]] bool ValidateSettingsDraft(
		const Settings::Values& a_values,
		const DirtySettings& a_dirty,
		std::string& a_error);
	[[nodiscard]] bool BuildCustomSettingsToml(
		std::string_view a_existing,
		const Settings::Values& a_baseValues,
		const Settings::Values& a_draft,
		const DirtySettings& a_dirty,
		std::string& a_output,
		std::string& a_error);
	[[nodiscard]] SettingsSaveResult SaveSettingsFiles(
		const SettingsSnapshot& a_snapshot,
		const SettingsSaveRequest& a_request);

	class SettingsPageState
	{
	public:
		void Activate(const SettingsSnapshot& a_snapshot);
		void Deactivate();
		void Edit(SettingKey a_key);
		void Reset();
		void Revert();
		[[nodiscard]] std::optional<SettingsSaveRequest> AcceptApply(
			uint64_t a_operation,
			std::string& a_error);
		void CancelApply(uint64_t a_operation);
		void Reconcile(
			const SettingsSnapshot& a_snapshot,
			const SettingsSaveResult* a_result);

		[[nodiscard]] bool Active() const noexcept;
		[[nodiscard]] bool SavePending() const noexcept;
		[[nodiscard]] Settings::Values& Draft() noexcept;
		[[nodiscard]] const Settings::Values& Draft() const noexcept;
		[[nodiscard]] const DirtySettings& Dirty() const noexcept;

	private:
		Settings::Values m_saved{ Settings::kBuiltInDefaults };
		Settings::Values m_draft{ Settings::kBuiltInDefaults };
		DirtySettings m_dirty;
		SourceFingerprint m_baseFingerprint;
		SourceFingerprint m_customFingerprint;
		uint64_t m_pendingOperation{};
		bool m_active{};
		bool m_savePending{};
	};

	class SettingsRepository
	{
	public:
		static SettingsRepository& GetSingleton();

		void SetDefaultReportDirectory(
			const std::filesystem::path& a_directory);
		void RequestLoad();
		[[nodiscard]] bool RequestSave(const SettingsSaveRequest& a_request);
		[[nodiscard]] SettingsRepositoryState Snapshot() const;

	private:
		SettingsRepository();
		void Worker();

		mutable std::mutex m_mutex;
		std::condition_variable m_condition;
		std::shared_ptr<const SettingsSnapshot> m_snapshot;
		std::optional<SettingsSaveRequest> m_pendingSave;
		std::optional<SettingsSaveResult> m_lastSaveResult;
		std::filesystem::path m_defaultReportDirectory;
		uint64_t m_generation{};
		bool m_loadPending{};
		bool m_saveInFlight{};
	};
}
