#pragma once

#include <REX/REX/INI.h>
#include "REX/W32/USER32.h"

using namespace std::literals;

namespace Settings
{
	// General
	static REX::INI::Bool bEnableCrashLogger			{ "General"sv,					"bEnableCrashLogger"sv,			true							};
	static REX::INI::Bool bPrintSettings				{ "General"sv,					"bPrintSettings"sv,				true							};
	static REX::INI::Bool bAutoOpenLogs					{ "General"sv,					"bAutoOpenLogs"sv,				false							};
	static REX::INI::I32  iMaxCrashLogs					{ "General"sv,					"iMaxCrashLogs"sv,				20								};
	static REX::INI::I32  iMaxMiniDumps					{ "General"sv,					"iMaxMiniDumps"sv,				1								};

	// Directories
	static REX::INI::Str  sCrashLogDirectory			{ "Directories"sv,				"sCrashLogDirectory"sv,			std::string{""}			};
	static REX::INI::Str  sSymcacheDirectory			{ "Directories"sv,				"sSymcacheDirectory"sv,			std::string{"C:\\symcache"}		};

	// Pastebin
	static REX::INI::Bool bAutoUploadCrashLog			{ "Pastebin"sv,					"bAutoUploadCrashLog"sv,		false							};
	static REX::INI::Str  sPastebinAPIKey				{ "Pastebin"sv,					"sPastebinAPIKey"sv,			std::string{""}					};

	// Debugging
	static REX::INI::Bool bWaitForDebugger				{ "Debugging"sv,				"bWaitForDebugger"sv,			false							};
	static REX::INI::Bool bFullMemoryMiniDump			{ "Debugging"sv,				"bFullMemoryMiniDump"sv,		false							};
	static REX::INI::Bool bCrashLogWriteMiniDump		{ "Debugging"sv,				"bCrashLogWriteMiniDump"sv,		false							};
	static REX::INI::Bool bThreadDumpWriteMiniDump		{ "Debugging"sv,				"bThreadDumpWriteMiniDump"sv,	false							};
	static REX::INI::Bool bHeapAnalysis					{ "Debugging"sv,				"bHeapAnalysis"sv,				false							};
	static REX::INI::I32  iMaxHeapsToCheck				{ "Debugging"sv,				"iMaxHeapsToCheck"sv,			1								};
	static REX::INI::I32  iMaxHeapIterationsPerHeap		{ "Debugging"sv,				"iMaxHeapIterationsPerHeap"sv,	1000							};

	// Hotkeys
	static REX::INI::Bool bEnableThreadDumpHotkey		{ "Hotkeys"sv,					"bEnableThreadDumpHotkey"sv,	false							};
	static std::vector<int> hotkeyThreadDump{ REX::W32::VK::VK_CONTROL, REX::W32::VK::VK_SHIFT, REX::W32::VK::VK_F12 };
}
