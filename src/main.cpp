#include <pch.h>

#include <CrashHandler.h>
#include <Menu/MenuStartupState.h>
#include <Menu/MenuUI.h>

namespace Main
{
	namespace
	{
		std::once_flag s_loggerOnce;
		std::once_flag s_listenerOnce;
		bool s_loggerInitialized{};

		[[nodiscard]] std::string RuntimeVersion()
		{
			const auto version =
				REX::FModule::GetExecutingModule().GetFileVersion();
			return std::format(
				"{}.{}.{}.{}",
				version[0],
				version[1],
				version[2],
				version[3]);
		}

		void PublishStartup(
			CrashUI::InstallOutcome a_outcome,
			std::string a_detail)
		{
			const auto active = Settings::Snapshot();
			const auto defaultPath = Crash::GetF4SELogDirectory();
			auto resolvedPath = defaultPath;
			std::string pathDetail =
				"Using the F4SE log directory. A path does not prove writability.";
			if (!active.crashLogDirectory.empty())
			{
				std::error_code error;
				const std::filesystem::path configured{
					active.crashLogDirectory
				};
				if (std::filesystem::is_directory(configured, error) && !error)
				{
					resolvedPath =
						std::filesystem::absolute(configured).lexically_normal();
					pathDetail =
						"Using the configured report directory. A path does not prove writability.";
				}
				else
				{
					pathDetail =
						"The configured report directory is unavailable; this "
						"session uses the F4SE log directory fallback.";
				}
			}
			const auto installedPath = Crash::GetCrashLogDirectory();
			if (!installedPath.empty())
				resolvedPath = installedPath;
			CrashUI::PublishStartupSnapshot({
				.activeSettings = active,
				.installOutcome = a_outcome,
				.installDetail = std::move(a_detail),
				.gameVersion = RuntimeVersion(),
				.loggerVersion = std::format(
					"{}.{}.{} ({} {})",
					PLUGIN_VERSION_MAJOR,
					PLUGIN_VERSION_MINOR,
					PLUGIN_VERSION_PATCH,
					__DATE__,
					__TIME__),
				.configuredReportDirectory =
					active.crashLogDirectory.empty() ?
						std::filesystem::path{} :
						std::filesystem::path{ active.crashLogDirectory },
				.resolvedReportDirectory = std::move(resolvedPath),
				.reportDirectoryDetail = std::move(pathDetail),
				.startupLogPath = Crash::GetStartupLogPath()
			});
		}

		void InitializeLogger()
		{
			std::call_once(s_loggerOnce, [] {
				REX::INFO("Addictol's Crash Logger Initializing...");

				REX::INFO("============================================");
				REX::INFO("!!!! ---- This is NOT a Crash Log ---- !!!!");
				REX::INFO("Look for crash-YYYY-MM-DD-HH-MM-SS.log files");
				REX::INFO("============================================");

				const auto config = REX::TOML::SettingStore::GetSingleton();
				config->Init(
					"Data/F4SE/Plugins/AddictolCrashLogger.toml",
					"Data/F4SE/Plugins/AddictolCrashLoggerCustom.toml");
				config->Load();

				if (Settings::bEnableCrashLogger.GetValue())
				{
					if (Crash::Install())
					{
						REX::INFO("Addictol's Crash Logger Initialized!");
						PublishStartup(
							CrashUI::InstallOutcome::kInstalled,
							"Crash handlers were installed.");
					}
					else
					{
						REX::ERROR(
							"Addictol's Crash Logger Initialization failed!");
						PublishStartup(
							CrashUI::InstallOutcome::kFailed,
							"Crash-handler installation returned failure.");
					}
				}
				else
				{
					REX::INFO("Addictol's Crash Logger is disabled.");
					PublishStartup(
						CrashUI::InstallOutcome::kDisabled,
						"Disabled by the active configuration; browsing remains available.");
				}

				s_loggerInitialized = true;
			});
		}

		void OnF4SEMessage(F4SE::MessagingInterface::Message* a_message)
		{
			if (a_message &&
				a_message->type == F4SE::MessagingInterface::kPostPostLoad)
				(void)CrashUI::InstallMenu();
		}

		void RegisterNormalLoadListener()
		{
			std::call_once(s_listenerOnce, [] {
				const auto messaging = F4SE::GetMessagingInterface();
				if (!messaging || !messaging->RegisterListener(&OnF4SEMessage))
				{
					REX::ERROR(
						"Crash Logger UI: F4SE messaging listener registration failed; "
						"headless crash logging remains active."sv);
				}
			});
		}
	}

	F4SE_PLUGIN_QUERY(
		const F4SE::QueryInterface* a_f4se,
		F4SE::PluginInfo* a_info)
	{
		if (const auto data = F4SE::PluginVersionData::GetSingleton())
		{
			a_info->infoVersion = F4SE::PluginInfo::kVersion;
			a_info->name = data->GetPluginName().data();
			a_info->version = data->GetPluginVersion().pack();
		}

		const auto version = a_f4se->RuntimeVersion();
		return version >= REL::Version(F4SE::RUNTIME_1_10_163);
	}

	F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
	{
		F4SE::Init(a_f4se);
		InitializeLogger();
		RegisterNormalLoadListener();
		return s_loggerInitialized;
	}

	F4SE_PLUGIN_PRELOAD(const F4SE::PreLoadInterface* a_f4se)
	{
		F4SE::Init(a_f4se);
		InitializeLogger();
		return s_loggerInitialized;
	}
}
