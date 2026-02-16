#include <pch.h>

// Patches
#include <CrashHandler.h>
#include <PDB/PdbHandler.h>

#define MAKE_EXE_VERSION_EX(major, minor, build, sub)	((((major) & 0xFF) << 24) | (((minor) & 0xFF) << 16) | (((build) & 0xFFF) << 4) | ((sub) & 0xF))
#define MAKE_EXE_VERSION(major, minor, build)			MAKE_EXE_VERSION_EX(major, minor, build, 0)

namespace OGSupport
{
    static F4SE::Impl::F4SEInterface RestoreLoadInterface;

    [[nodiscard]] inline static const char* F4SEAPI F4SEGetSaveFolderName() noexcept
    {
        return "Fallout4";
    }

    void Init(const F4SE::LoadInterface* a_f4se)
    {
        memcpy(&RestoreLoadInterface, a_f4se, 48);
        (((F4SE::Impl::F4SEInterface*)(&RestoreLoadInterface))->GetSaveFolderName) = F4SEGetSaveFolderName;
        
        F4SE::Init((const F4SE::LoadInterface*)(&RestoreLoadInterface));
    }
}

namespace Main
{
    // Config Options
    static REX::INI::Bool bCrashLoggerPatch{ "CrashLogger"sv, "bCrashLogger"sv, true };
    
    // Init Bool
    static bool isInit = false;

    bool InitPlugin(const F4SE::LoadInterface* a_f4se)
    {
        if (isInit)
            return true;

        static std::once_flag once;
        std::call_once(once, [&]() {
            // Init F4SE (OG Support)
            OGSupport::Init(a_f4se);

            REX::INFO("Addictol's Crash Logger Initializing...");

            // Load the Config
            const auto config = REX::INI::SettingStore::GetSingleton();
            config->Init("Data/F4SE/Plugins/AddictolCrashLogger.ini", "Data/F4SE/Plugins/AddictolCrashLoggerCustom.ini");
            config->Load();

            // Install Crash Logger
            if (bCrashLoggerPatch.GetValue() == true)
            {
                if (Crash::Install())
                {
                    // PDB
                    Crash::PDB::hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

                    REX::INFO("Addictol's Crash Logger Initialized!");
                }
                else
                {
                    REX::INFO("Addictol's Crash Logger Initialization failed!");
                }
            }
            else
            {
                REX::INFO("Addictol's Crash Logger is disabled.");
            }

            // Finished
            isInit = true;
        });

        return isInit;
    }

    F4SE_EXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
    {
        if (!a_f4se)
            return false;

        if (!a_info)
            return false;

        if (a_f4se->RuntimeVersion() != REL::Version{ 1, 10, 163, 0 })
            return false;

        a_info->infoVersion = F4SE::PluginInfo::kVersion;
        a_info->version = MAKE_EXE_VERSION(PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, PLUGIN_VERSION_PATCH);
        a_info->name = PLUGIN_NAME;

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
