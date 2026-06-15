#pragma once

#include "Settings.h"

#include <RE/Fallout.h>
#include <F4SE/F4SE.h>
#include <REX/REX/TOML.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI

#include <Windows.h>
#include <DbgHelp.h>
#include <Psapi.h>
#include <ShlObj_core.h>
#include <boost/stacktrace.hpp>
#include <dia2.h>
#include <diacreate.h>
#include <fmt/format.h>
#include <frozen/map.h>
#include <infoware/cpu.hpp>
#include <infoware/gpu.hpp>
#include <infoware/system.hpp>

#undef cdecl  // Workaround for Clang 14 CMake configure error.

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>

#undef GetObject  // Have to do this because PCH pulls in spdlog->winbase.h->windows.h->wingdi.h, which redfines GetObject

using namespace std::literals;

// Logging
#define AD_NOMESSAGE_CRASHLOGGER 1

namespace LOG
{
    template <class... T>
    inline void INFO(const std::format_string<T...> fmt, T&&... args)
    {
#if !AD_NOMESSAGE_CRASHLOGGER
        REX::INFO<T...>{ fmt, std::forward<T>(args)... };
#endif
    }

    inline void INFO(std::string_view fmt)
    {
#if !AD_NOMESSAGE_CRASHLOGGER
        REX::INFO<void>{ fmt };
#endif
    }

    template <class... T>
    inline void WARN(const std::format_string<T...> fmt, T&&... args)
    {
#if !AD_NOMESSAGE_CRASHLOGGER
        REX::WARN<T...>{ fmt, std::forward<T>(args)... };
#endif
    }

    inline void WARN(std::string_view fmt)
    {
#if !AD_NOMESSAGE_CRASHLOGGER
        REX::WARN<void>{ fmt };
#endif
    }

	template <class... T>
    inline void ERROR(const std::format_string<T...> fmt, T&&... args)
    {
#if !AD_NOMESSAGE_CRASHLOGGER
        REX::ERROR<T...>{ fmt, std::forward<T>(args)... };
#endif
    }

    inline void ERROR(std::string_view fmt)
    {
#if !AD_NOMESSAGE_CRASHLOGGER
        REX::ERROR<void>{ fmt };
#endif
    }
}

namespace util
{
	[[nodiscard]] inline auto utf16_to_utf8(std::wstring_view a_in) noexcept -> std::optional<std::string>
	{
		const auto cvt = [&](char* a_dst, std::size_t a_length)
		{
			return REX::W32::WideCharToMultiByte(
				65001u,
				0,
				a_in.data(),
				static_cast<int>(a_in.length()),
				a_dst,
				static_cast<int>(a_length),
				nullptr,
				nullptr);
		};

		const auto len = cvt(nullptr, 0);
		if (len == 0)
			return std::nullopt;

		std::string out(len, '\0');
		if (cvt(out.data(), out.length()) == 0)
			return std::nullopt;

		return out;
	}

	[[nodiscard]] inline auto module_name() -> std::string
	{
		const auto FileName = std::filesystem::path(REX::FModule::GetExecutingModule().GetFileName());
		return utf16_to_utf8(FileName.filename().wstring()).value_or("<Unknown Module Name>"s);
	}
}
