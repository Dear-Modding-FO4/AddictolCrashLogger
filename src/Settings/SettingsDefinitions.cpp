#include "Settings.h"

namespace Settings
{
	REX::TOML::Bool<> bEnableCrashLogger{
		"General"sv, "bEnableCrashLogger"sv, kBuiltInDefaults.enableCrashLogger
	};
	REX::TOML::Bool<> bPrintSettings{
		"General"sv, "bPrintSettings"sv, kBuiltInDefaults.printSettings
	};
	REX::TOML::Bool<> bAutoOpenLogs{
		"General"sv, "bAutoOpenLogs"sv, kBuiltInDefaults.autoOpenLogs
	};
	REX::TOML::I32<> iMaxCrashLogs{
		"General"sv, "iMaxCrashLogs"sv, kBuiltInDefaults.maxCrashLogs
	};
	REX::TOML::I32<> iMaxMiniDumps{
		"General"sv, "iMaxMiniDumps"sv, kBuiltInDefaults.maxMiniDumps
	};

	REX::TOML::Str<> sCrashLogDirectory{
		"Directories"sv, "sCrashLogDirectory"sv, kBuiltInDefaults.crashLogDirectory
	};
	REX::TOML::Str<> sSymcacheDirectory{
		"Directories"sv, "sSymcacheDirectory"sv, kBuiltInDefaults.symcacheDirectory
	};

	REX::TOML::Bool<> bAutoUploadCrashLog{
		"Pastebin"sv, "bAutoUploadCrashLog"sv, kBuiltInDefaults.autoUploadCrashLog
	};
	REX::TOML::Str<> sPastebinAPIKey{
		"Pastebin"sv, "sPastebinAPIKey"sv, std::string{}
	};

	REX::TOML::Bool<> bWaitForDebugger{
		"Debugging"sv, "bWaitForDebugger"sv, kBuiltInDefaults.waitForDebugger
	};
	REX::TOML::Bool<> bFullMemoryMiniDump{
		"Debugging"sv, "bFullMemoryMiniDump"sv, kBuiltInDefaults.fullMemoryMiniDump
	};
	REX::TOML::Bool<> bCrashLogWriteMiniDump{
		"Debugging"sv, "bCrashLogWriteMiniDump"sv, kBuiltInDefaults.crashLogWriteMiniDump
	};
	REX::TOML::Bool<> bThreadDumpWriteMiniDump{
		"Debugging"sv, "bThreadDumpWriteMiniDump"sv, kBuiltInDefaults.threadDumpWriteMiniDump
	};
	REX::TOML::Bool<> bHeapAnalysis{
		"Debugging"sv, "bHeapAnalysis"sv, kBuiltInDefaults.heapAnalysis
	};
	REX::TOML::I32<> iMaxHeapsToCheck{
		"Debugging"sv, "iMaxHeapsToCheck"sv, kBuiltInDefaults.maxHeapsToCheck
	};
	REX::TOML::I32<> iMaxHeapIterationsPerHeap{
		"Debugging"sv, "iMaxHeapIterationsPerHeap"sv, kBuiltInDefaults.maxHeapIterationsPerHeap
	};

	REX::TOML::Bool<> bEnableThreadDumpHotkey{
		"Hotkeys"sv, "bEnableThreadDumpHotkey"sv, kBuiltInDefaults.enableThreadDumpHotkey
	};
	const std::vector<int> hotkeyThreadDump{
		0x11,
		0x10,
		0x7B
	};

	Values Snapshot()
	{
		return {
			.enableCrashLogger = bEnableCrashLogger.GetValue(),
			.printSettings = bPrintSettings.GetValue(),
			.autoOpenLogs = bAutoOpenLogs.GetValue(),
			.maxCrashLogs = iMaxCrashLogs.GetValue(),
			.maxMiniDumps = iMaxMiniDumps.GetValue(),
			.crashLogDirectory = sCrashLogDirectory.GetValue(),
			.symcacheDirectory = sSymcacheDirectory.GetValue(),
			.autoUploadCrashLog = bAutoUploadCrashLog.GetValue(),
			.waitForDebugger = bWaitForDebugger.GetValue(),
			.fullMemoryMiniDump = bFullMemoryMiniDump.GetValue(),
			.crashLogWriteMiniDump = bCrashLogWriteMiniDump.GetValue(),
			.threadDumpWriteMiniDump = bThreadDumpWriteMiniDump.GetValue(),
			.heapAnalysis = bHeapAnalysis.GetValue(),
			.maxHeapsToCheck = iMaxHeapsToCheck.GetValue(),
			.maxHeapIterationsPerHeap = iMaxHeapIterationsPerHeap.GetValue(),
			.enableThreadDumpHotkey = bEnableThreadDumpHotkey.GetValue()
		};
	}
}
