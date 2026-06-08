#include <pch.h>

// Patches
#include <CrashHandler.h>

namespace Main
{
    // Init Bool
    static bool isInit = false;

    bool InitPlugin(const F4SE::LoadInterface* a_f4se)
    {
        if (isInit)
            return true;

        static std::once_flag once;
        std::call_once(once, [&]() {
            // Init F4SE
            F4SE::Init(a_f4se);

            // Init Mod
            REX::INFO("Addictol's Crash Logger Initializing...");

            // AddictolCrashLogger.log is not a Crash Log
            REX::INFO("============================================");
            REX::INFO("!!!! ---- This is NOT a Crash Log ---- !!!!");
            REX::INFO("Look for crash-YYYY-MM-DD-HH-MM-SS.log files");
            REX::INFO("============================================");

            // Load the Config
            const auto config = REX::TOML::SettingStore::GetSingleton();
            config->Init("Data/F4SE/Plugins/AddictolCrashLogger.toml", "Data/F4SE/Plugins/AddictolCrashLoggerCustom.toml");
            config->Load();

            // Install Crash Logger
            if (Settings::bEnableCrashLogger.GetValue() == true)
            {
                if (Crash::Install())
                    REX::INFO("Addictol's Crash Logger Initialized!");
                else
                    REX::INFO("Addictol's Crash Logger Initialization failed!");
            }
            else
                REX::INFO("Addictol's Crash Logger is disabled.");

            // Finished
            isInit = true;
        });

        return isInit;
    }

    F4SE_PLUGIN_QUERY(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
    {
        if (const auto data = F4SE::PluginVersionData::GetSingleton())
        {
            a_info->infoVersion = F4SE::PluginInfo::kVersion;
            a_info->name = data->GetPluginName().data();
            a_info->version = data->GetPluginVersion().pack();
        }

        const auto ver = a_f4se->RuntimeVersion();
        if (ver < REL::Version(F4SE::RUNTIME_1_10_163))
            return false;

        return true;
    }

    F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
	{
        // OG does not support PreLoading
		return InitPlugin(a_f4se);
	}

    F4SE_PLUGIN_PRELOAD(const F4SE::LoadInterface* a_f4se)
    {
        return InitPlugin(a_f4se);
    }
}
