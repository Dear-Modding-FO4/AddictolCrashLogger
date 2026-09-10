#pragma once

#include "Settings/SettingsValues.h"

#include <REX/REX/TOML.h>
#include "REX/W32/USER32.h"

using namespace std::literals;

namespace Settings
{
	// General
	extern REX::TOML::Bool<>	bEnableCrashLogger;
	extern REX::TOML::Bool<>	bPrintSettings;
	extern REX::TOML::Bool<>	bAutoOpenLogs;
	extern REX::TOML::I32<>	iMaxCrashLogs;
	extern REX::TOML::I32<>	iMaxMiniDumps;

	// Directories
	extern REX::TOML::Str<>	sCrashLogDirectory;
	extern REX::TOML::Str<>	sSymcacheDirectory;

	// Pastebin
	extern REX::TOML::Bool<>	bAutoUploadCrashLog;
	extern REX::TOML::Str<>	sPastebinAPIKey;

	// Debugging
	extern REX::TOML::Bool<>	bWaitForDebugger;
	extern REX::TOML::Bool<>	bFullMemoryMiniDump;
	extern REX::TOML::Bool<>	bCrashLogWriteMiniDump;
	extern REX::TOML::Bool<>	bThreadDumpWriteMiniDump;
	extern REX::TOML::Bool<>	bHeapAnalysis;
	extern REX::TOML::I32<>	iMaxHeapsToCheck;
	extern REX::TOML::I32<>	iMaxHeapIterationsPerHeap;

	// Hotkeys
	extern REX::TOML::Bool<>	bEnableThreadDumpHotkey;
	extern const std::vector<int> hotkeyThreadDump;

	[[nodiscard]] Values Snapshot();
}
