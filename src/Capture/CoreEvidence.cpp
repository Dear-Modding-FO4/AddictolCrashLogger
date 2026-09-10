#include "Capture/CoreEvidence.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>

namespace Capture
{
	namespace
	{
		constexpr std::size_t kMaximumPreparedDirectory = 30'000;
		std::array<wchar_t, kMaximumPreparedDirectory> s_directory{};
		std::atomic<std::uint32_t> s_directoryLength{};

		struct FixedText
		{
			std::array<char, 8192> data{};
			std::size_t size{};
			bool overflow{};

			void text(std::string_view a_value) noexcept
			{
				const auto count = std::min(a_value.size(), data.size() - size);
				if (count != 0)
				std::memcpy(data.data() + size, a_value.data(), count);
				size += count;
				overflow = overflow || count != a_value.size();
			}

			void hex(std::uint64_t a_value, std::size_t a_digits) noexcept
			{
				static constexpr char digits[]{ "0123456789ABCDEF" };
				text("0x");
				for (std::size_t index = 0; index < a_digits; ++index)
				{
					const auto shift = (a_digits - index - 1) * 4;
					const char digit = digits[(a_value >> shift) & 0xF];
					text(std::string_view(std::addressof(digit), 1));
				}
			}

			void decimal(std::uint64_t a_value) noexcept
			{
				char reversed[32]{};
				std::size_t count{};
				do
				{
					reversed[count++] = static_cast<char>('0' + a_value % 10);
					a_value /= 10;
				} while (a_value != 0 && count < std::size(reversed));
				while (count != 0)
				text(std::string_view(std::addressof(reversed[--count]), 1));
			}

			void line(std::string_view a_key, std::uint64_t a_value, std::size_t a_digits = 16) noexcept
			{
				text(a_key);
				hex(a_value, a_digits);
				text("\r\n");
			}
		};

		void append_two_digits(wchar_t*& a_cursor, WORD a_value) noexcept
		{
			*a_cursor++ = static_cast<wchar_t>(L'0' + (a_value / 10) % 10);
			*a_cursor++ = static_cast<wchar_t>(L'0' + a_value % 10);
		}

		void append_four_digits(wchar_t*& a_cursor, WORD a_value) noexcept
		{
			*a_cursor++ = static_cast<wchar_t>(L'0' + (a_value / 1000) % 10);
			*a_cursor++ = static_cast<wchar_t>(L'0' + (a_value / 100) % 10);
			*a_cursor++ = static_cast<wchar_t>(L'0' + (a_value / 10) % 10);
			*a_cursor++ = static_cast<wchar_t>(L'0' + a_value % 10);
		}

		[[nodiscard]] bool write_all(HANDLE a_file, const void* a_data, std::size_t a_size) noexcept
		{
			const auto* cursor = static_cast<const std::byte*>(a_data);
			std::size_t remaining = a_size;
			while (remaining != 0)
			{
				const auto chunk = static_cast<DWORD>(
					std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));
				DWORD written{};
				if (!::WriteFile(a_file, cursor, chunk, std::addressof(written), nullptr) ||
					written == 0)
					return false;
				cursor += written;
				remaining -= written;
			}
			return true;
		}

		void append_context(FixedText& a_text, const CONTEXT& a_context) noexcept
		{
#define CORE_REGISTER(a_name) a_text.line(#a_name ": ", a_context.a_name)
			CORE_REGISTER(Rax);
			CORE_REGISTER(Rbx);
			CORE_REGISTER(Rcx);
			CORE_REGISTER(Rdx);
			CORE_REGISTER(Rsi);
			CORE_REGISTER(Rdi);
			CORE_REGISTER(Rbp);
			CORE_REGISTER(Rsp);
			CORE_REGISTER(R8);
			CORE_REGISTER(R9);
			CORE_REGISTER(R10);
			CORE_REGISTER(R11);
			CORE_REGISTER(R12);
			CORE_REGISTER(R13);
			CORE_REGISTER(R14);
			CORE_REGISTER(R15);
			CORE_REGISTER(Rip);
			a_text.line("EFlags: ", a_context.EFlags, 8);
#undef CORE_REGISTER
		}
	}

	CoreEvidence capture_core_evidence(
		const EXCEPTION_RECORD& a_exception,
		const CONTEXT& a_context,
		std::span<const std::uint16_t> a_runtimeVersion) noexcept
	{
		CoreEvidence evidence{
			.primary = a_exception,
			.context = a_context,
			.processId = ::GetCurrentProcessId(),
			.threadId = ::GetCurrentThreadId()
		};
		::GetSystemTimeAsFileTime(std::addressof(evidence.utcTime));
		if (a_runtimeVersion.size() == evidence.runtimeVersion.size())
			std::ranges::copy(a_runtimeVersion, evidence.runtimeVersion.begin());

		auto* nested = a_exception.ExceptionRecord;
		while (nested && evidence.nestedCount < evidence.nested.size())
		{
			__try
			{
				evidence.nested[evidence.nestedCount++] = *nested;
				nested = nested->ExceptionRecord;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				break;
			}
		}
		evidence.primary.ExceptionRecord = nullptr;
		for (auto& record : evidence.nested)
			record.ExceptionRecord = nullptr;
		return evidence;
	}

	DWORD prepare_fatal_report_directory(const std::filesystem::path& a_directory) noexcept
	{
		s_directoryLength.store(0, std::memory_order_release);
		const auto& path = a_directory.native();
		if (path.empty() || path.find(L'\0') != std::wstring::npos)
			return ERROR_INVALID_NAME;
		if (path.size() + 1 >= s_directory.size())
			return ERROR_FILENAME_EXCED_RANGE;
		std::ranges::copy(path, s_directory.begin());
		std::uint32_t size = static_cast<std::uint32_t>(path.size());
		if (s_directory[size - 1] != L'\\' && s_directory[size - 1] != L'/')
			s_directory[size++] = L'\\';
		s_directory[size] = L'\0';
		s_directoryLength.store(size, std::memory_order_release);
		return ERROR_SUCCESS;
	}

	CoreWriteResult write_core_evidence(const CoreEvidence& a_evidence) noexcept
	{
		CoreWriteResult result;
		const auto directoryLength = s_directoryLength.load(std::memory_order_acquire);
		if (directoryLength == 0)
		{
			result.systemError = ERROR_PATH_NOT_FOUND;
			return result;
		}

		SYSTEMTIME time{};
		::FileTimeToSystemTime(std::addressof(a_evidence.utcTime), std::addressof(time));
		thread_local std::array<wchar_t, kMaximumPreparedDirectory + 64> path{};
		path.fill(L'\0');
		std::memcpy(path.data(), s_directory.data(), directoryLength * sizeof(wchar_t));
		auto* cursor = path.data() + directoryLength;
		constexpr std::wstring_view prefix{ L"crash-" };
		std::memcpy(cursor, prefix.data(), prefix.size() * sizeof(wchar_t));
		cursor += prefix.size();
		append_four_digits(cursor, time.wYear);
		*cursor++ = L'-';
		append_two_digits(cursor, time.wMonth);
		*cursor++ = L'-';
		append_two_digits(cursor, time.wDay);
		*cursor++ = L'-';
		append_two_digits(cursor, time.wHour);
		*cursor++ = L'-';
		append_two_digits(cursor, time.wMinute);
		*cursor++ = L'-';
		append_two_digits(cursor, time.wSecond);
		auto* suffix = cursor;

		HANDLE file = INVALID_HANDLE_VALUE;
		for (unsigned collision = 0; collision < 100; ++collision)
		{
			cursor = suffix;
			if (collision != 0)
			{
				*cursor++ = L'-';
				append_two_digits(cursor, static_cast<WORD>(collision));
			}
			constexpr std::wstring_view extension{ L".log" };
			std::memcpy(cursor, extension.data(), extension.size() * sizeof(wchar_t));
			cursor += extension.size();
			*cursor = L'\0';
			file = ::CreateFileW(
				path.data(),
				FILE_APPEND_DATA | SYNCHRONIZE,
				FILE_SHARE_READ,
				nullptr,
				CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
				nullptr);
			if (file != INVALID_HANDLE_VALUE || ::GetLastError() != ERROR_FILE_EXISTS)
				break;
		}
		if (file == INVALID_HANDLE_VALUE)
		{
			result.systemError = ::GetLastError();
			return result;
		}
		result.created = true;

		FixedText text;
		text.text("===== ADDICTOL CRASH CORE EVIDENCE =====\r\n");
		text.text("CORE STATUS: WRITING\r\n");
		text.line("Exception Code: ", a_evidence.primary.ExceptionCode, 8);
		text.line("Exception Flags: ", a_evidence.primary.ExceptionFlags, 8);
		text.line(
			"Exception Address: ",
			reinterpret_cast<std::uintptr_t>(a_evidence.primary.ExceptionAddress));
		text.line("Context RIP: ", a_evidence.context.Rip);
		text.line("Context RSP: ", a_evidence.context.Rsp);
		text.text("Process ID: ");
		text.decimal(a_evidence.processId);
		text.text("\r\nThread ID: ");
		text.decimal(a_evidence.threadId);
		text.text("\r\nNumber of Parameters: ");
		text.decimal(std::min<DWORD>(
			a_evidence.primary.NumberParameters,
			EXCEPTION_MAXIMUM_PARAMETERS));
		text.text("\r\n");
		for (DWORD index = 0;
			index < std::min<DWORD>(
				a_evidence.primary.NumberParameters,
				EXCEPTION_MAXIMUM_PARAMETERS);
			++index)
		{
			text.text("Parameter[");
			text.decimal(index);
			text.text("]: ");
			text.hex(a_evidence.primary.ExceptionInformation[index], 16);
			text.text("\r\n");
		}
		append_context(text, a_evidence.context);
		text.text("Nested Records Captured: ");
		text.decimal(a_evidence.nestedCount);
		text.text("\r\nCORE STATUS: COMPLETE\r\n");
		if (text.overflow)
			text.text("CORE WARNING: fixed buffer truncated\r\n");

		result.completed =
			write_all(file, text.data.data(), text.size) &&
			::FlushFileBuffers(file);
		result.systemError = result.completed ? ERROR_SUCCESS : ::GetLastError();
		::CloseHandle(file);
		try
		{
			result.path = std::filesystem::path(path.data());
		}
		catch (...)
		{
			result.systemError = ERROR_NOT_ENOUGH_MEMORY;
		}
		return result;
	}

	bool append_core_marker(
		const std::filesystem::path& a_path,
		std::string_view a_text) noexcept
	{
		const auto file = ::CreateFileW(
			a_path.c_str(),
			FILE_APPEND_DATA | SYNCHRONIZE,
			FILE_SHARE_READ,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return false;
		const bool written =
			write_all(file, a_text.data(), a_text.size()) &&
			::FlushFileBuffers(file);
		::CloseHandle(file);
		return written;
	}
}
