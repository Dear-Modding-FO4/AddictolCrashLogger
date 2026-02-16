#pragma once

#include <RE/Fallout.h>
#include <F4SE/F4SE.h>
#include <REX/REX/INI.h>
#include <xbyak/xbyak.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <execution>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>

#pragma warning(push)
#include <boost/algorithm/searching/knuth_morris_pratt.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/stacktrace.hpp>
#include <fmt/chrono.h>
#include <frozen/map.h>
#include <infoware/cpu.hpp>
#include <infoware/gpu.hpp>
#include <infoware/system.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#pragma warning(pop)

using namespace std::literals;

namespace WinAPI
{
	inline constexpr auto UNDNAME_NO_MS_KEYWORDS = std::uint32_t{ 0x0002 };
	inline constexpr auto UNDNAME_NO_FUNCTION_RETURNS = std::uint32_t{ 0x0004 };
	inline constexpr auto UNDNAME_NO_ALLOCATION_MODEL = std::uint32_t{ 0x0008 };
	inline constexpr auto UNDNAME_NO_ALLOCATION_LANGUAGE = std::uint32_t{ 0x0010 };
	inline constexpr auto UNDNAME_NO_THISTYPE = std::uint32_t{ 0x0060 };
	inline constexpr auto UNDNAME_NO_ACCESS_SPECIFIERS = std::uint32_t{ 0x0080 };
	inline constexpr auto UNDNAME_NO_THROW_SIGNATURES = std::uint32_t{ 0x0100 };
	inline constexpr auto UNDNAME_NO_RETURN_UDT_MODEL = std::uint32_t{ 0x0400 };
	inline constexpr auto UNDNAME_NAME_ONLY = std::uint32_t{ 0x1000 };
	inline constexpr auto UNDNAME_NO_ARGUMENTS = std::uint32_t{ 0x2000 };

	[[nodiscard]] bool IsDebuggerPresent() noexcept;

	[[nodiscard]] std::uint32_t UnDecorateSymbolName(
		const char* a_name,
		char* a_outputString,
		std::uint32_t a_maxStringLength,
		std::uint32_t a_flags) noexcept;
}

inline std::string utf16_to_utf8(std::wstring_view wstr) {
	std::string result;
	result.reserve(wstr.size());

	for (wchar_t wc : wstr) {
		if (wc <= 0x7F) {
			result.push_back(static_cast<char>(wc));
		}
		else if (wc <= 0x7FF) {
			result.push_back(static_cast<char>(0xC0 | ((wc >> 6) & 0x1F)));
			result.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
		}
		else {
			result.push_back(static_cast<char>(0xE0 | ((wc >> 12) & 0x0F)));
			result.push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
			result.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
		}
	}

	return result;
}

namespace util
{
	[[nodiscard]] inline auto module_name() -> std::string
	{
		const auto FileName = std::filesystem::path(REL::Module::GetSingleton()->filename()).filename().wstring();
		return utf16_to_utf8(FileName);
	}
}
