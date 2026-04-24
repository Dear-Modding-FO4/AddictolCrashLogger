#pragma once

#include <REX/REX/TOML.h>
#include "REX/W32/USER32.h"

using namespace std::literals;

namespace Settings
{
	// General
	static REX::TOML::Bool	bEnableCrashLogger			{ "General"sv,					"bEnableCrashLogger"sv,			true							};
	static REX::TOML::Bool	bPrintSettings				{ "General"sv,					"bPrintSettings"sv,				true							};
	static REX::TOML::Bool	bAutoOpenLogs				{ "General"sv,					"bAutoOpenLogs"sv,				false							};
	static REX::TOML::I32	iMaxCrashLogs				{ "General"sv,					"iMaxCrashLogs"sv,				20								};
	static REX::TOML::I32	iMaxMiniDumps				{ "General"sv,					"iMaxMiniDumps"sv,				1								};

	// Directories
	static REX::TOML::Str	sCrashLogDirectory			{ "Directories"sv,				"sCrashLogDirectory"sv,			std::string{""}					};
	static REX::TOML::Str	sSymcacheDirectory			{ "Directories"sv,				"sSymcacheDirectory"sv,			std::string{"C:\\symcache"}		};

	// Pastebin
	static REX::TOML::Bool	bAutoUploadCrashLog			{ "Pastebin"sv,					"bAutoUploadCrashLog"sv,		false							};
	static REX::TOML::Str	sPastebinAPIKey				{ "Pastebin"sv,					"sPastebinAPIKey"sv,			std::string{""}					};

	// Debugging
	static REX::TOML::Bool	bWaitForDebugger			{ "Debugging"sv,				"bWaitForDebugger"sv,			false							};
	static REX::TOML::Bool	bFullMemoryMiniDump			{ "Debugging"sv,				"bFullMemoryMiniDump"sv,		false							};
	static REX::TOML::Bool	bCrashLogWriteMiniDump		{ "Debugging"sv,				"bCrashLogWriteMiniDump"sv,		false							};
	static REX::TOML::Bool	bThreadDumpWriteMiniDump	{ "Debugging"sv,				"bThreadDumpWriteMiniDump"sv,	false							};
	static REX::TOML::Bool	bHeapAnalysis				{ "Debugging"sv,				"bHeapAnalysis"sv,				false							};
	static REX::TOML::I32	iMaxHeapsToCheck			{ "Debugging"sv,				"iMaxHeapsToCheck"sv,			1								};
	static REX::TOML::I32	iMaxHeapIterationsPerHeap	{ "Debugging"sv,				"iMaxHeapIterationsPerHeap"sv,	1000							};

	// Hotkeys
	static REX::TOML::Bool	bEnableThreadDumpHotkey		{ "Hotkeys"sv,					"bEnableThreadDumpHotkey"sv,	false							};
	static std::vector<int>	hotkeyThreadDump			{ REX::W32::VK::VK_CONTROL,		REX::W32::VK::VK_SHIFT,			REX::W32::VK::VK_F12			};
}
