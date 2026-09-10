#pragma once

#include "Settings.h"

#include <filesystem>
#include <memory>
#include <string>

namespace CrashUI
{
	enum class InstallOutcome
	{
		kNotAttempted,
		kDisabled,
		kInstalled,
		kFailed
	};

	struct StartupSnapshot
	{
		Settings::Values activeSettings;
		InstallOutcome installOutcome{ InstallOutcome::kNotAttempted };
		std::string installDetail;
		std::string gameVersion;
		std::string loggerVersion;
		std::filesystem::path configuredReportDirectory;
		std::filesystem::path resolvedReportDirectory;
		std::string reportDirectoryDetail;
		std::filesystem::path startupLogPath;
	};

	void PublishStartupSnapshot(StartupSnapshot a_snapshot);
	[[nodiscard]] std::shared_ptr<const StartupSnapshot> GetStartupSnapshot();
}
