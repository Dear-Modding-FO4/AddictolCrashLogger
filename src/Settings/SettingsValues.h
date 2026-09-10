#pragma once

#include <cstdint>
#include <string>

namespace Settings
{
	struct Values
	{
		bool enableCrashLogger{ true };
		bool printSettings{ true };
		bool autoOpenLogs{ false };
		int32_t maxCrashLogs{ 20 };
		int32_t maxMiniDumps{ 1 };
		std::string crashLogDirectory;
		std::string symcacheDirectory{ "C:\\symcache" };
		bool autoUploadCrashLog{ false };
		bool waitForDebugger{ false };
		bool fullMemoryMiniDump{ false };
		bool crashLogWriteMiniDump{ false };
		bool threadDumpWriteMiniDump{ false };
		bool enableThreadDumpHotkey{ false };

		constexpr bool operator==(const Values&) const noexcept = default;
	};

	inline constexpr Values kBuiltInDefaults{};
}
