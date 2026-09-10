#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace Capture
{
	inline constexpr std::size_t kMaximumNestedExceptionRecords = 8;

	struct CoreEvidence
	{
		EXCEPTION_RECORD primary{};
		CONTEXT context{};
		std::array<EXCEPTION_RECORD, kMaximumNestedExceptionRecords> nested{};
		std::uint32_t nestedCount{};
		DWORD processId{};
		DWORD threadId{};
		FILETIME utcTime{};
		std::array<std::uint16_t, 4> runtimeVersion{};
	};

	struct CoreWriteResult
	{
		bool created{};
		bool completed{};
		DWORD systemError{};
		std::filesystem::path path;
	};

	[[nodiscard]] CoreEvidence capture_core_evidence(
		const EXCEPTION_RECORD& a_exception,
		const CONTEXT& a_context,
		std::span<const std::uint16_t> a_runtimeVersion = {}) noexcept;

	[[nodiscard]] DWORD prepare_fatal_report_directory(
		const std::filesystem::path& a_directory) noexcept;

	[[nodiscard]] CoreWriteResult write_core_evidence(
		const CoreEvidence& a_evidence) noexcept;

	[[nodiscard]] bool append_core_marker(
		const std::filesystem::path& a_path,
		std::string_view a_text) noexcept;
}
