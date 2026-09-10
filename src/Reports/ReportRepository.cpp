#include "ReportRepository.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <span>
#include <sstream>
#include <thread>

namespace CrashUI
{
	using namespace std::literals;

	namespace
	{
		struct FileIdentity
		{
			DWORD volume{};
			DWORD indexHigh{};
			DWORD indexLow{};
			DWORD sizeHigh{};
			DWORD sizeLow{};
			FILETIME writeTime{};

			bool operator==(const FileIdentity& a_other) const noexcept
			{
				return volume == a_other.volume &&
					indexHigh == a_other.indexHigh &&
					indexLow == a_other.indexLow &&
					sizeHigh == a_other.sizeHigh &&
					sizeLow == a_other.sizeLow &&
					writeTime.dwLowDateTime == a_other.writeTime.dwLowDateTime &&
					writeTime.dwHighDateTime == a_other.writeTime.dwHighDateTime;
			}
		};

		[[nodiscard]] bool ReadIdentity(HANDLE a_file, FileIdentity& a_identity)
		{
			BY_HANDLE_FILE_INFORMATION info{};
			if (!GetFileInformationByHandle(a_file, &info))
				return false;
			a_identity = {
				info.dwVolumeSerialNumber,
				info.nFileIndexHigh,
				info.nFileIndexLow,
				info.nFileSizeHigh,
				info.nFileSizeLow,
				info.ftLastWriteTime
			};
			return true;
		}

		[[nodiscard]] bool SameFileObject(
			const FileIdentity& a_left,
			const FileIdentity& a_right) noexcept
		{
			return a_left.volume == a_right.volume &&
				a_left.indexHigh == a_right.indexHigh &&
				a_left.indexLow == a_right.indexLow;
		}

		[[nodiscard]] std::string Utf16ToUtf8(
			const wchar_t* a_text,
			size_t a_length)
		{
			if (a_length == 0)
				return {};
			const auto needed = WideCharToMultiByte(
				CP_UTF8, WC_ERR_INVALID_CHARS, a_text,
				static_cast<int>(a_length), nullptr, 0, nullptr, nullptr);
			if (needed <= 0)
				return {};
			std::string result(static_cast<size_t>(needed), '\0');
			if (WideCharToMultiByte(
					CP_UTF8, WC_ERR_INVALID_CHARS, a_text,
					static_cast<int>(a_length), result.data(), needed,
					nullptr, nullptr) <= 0)
				return {};
			return result;
		}

		void AppendReplacement(std::string& a_output)
		{
			a_output.append("\xEF\xBF\xBD");
		}

		[[nodiscard]] bool IsContinuation(unsigned char a_value) noexcept
		{
			return (a_value & 0xC0u) == 0x80u;
		}

		[[nodiscard]] std::string NormalizeUtf8(
			std::span<const std::byte> a_bytes,
			bool& a_changed)
		{
			std::string result;
			result.reserve(a_bytes.size());
			for (size_t i = 0; i < a_bytes.size();)
			{
				const auto c = std::to_integer<unsigned char>(a_bytes[i]);
				size_t length{};
				if (c <= 0x7Fu)
					length = 1;
				else if (c >= 0xC2u && c <= 0xDFu)
					length = 2;
				else if (c >= 0xE0u && c <= 0xEFu)
					length = 3;
				else if (c >= 0xF0u && c <= 0xF4u)
					length = 4;
				if (length == 0 || i + length > a_bytes.size())
				{
					AppendReplacement(result);
					a_changed = true;
					++i;
					continue;
				}
				auto valid = true;
				for (size_t j = 1; j < length; ++j)
					valid = valid &&
						IsContinuation(std::to_integer<unsigned char>(a_bytes[i + j]));
				if (length == 3)
				{
					const auto c1 = std::to_integer<unsigned char>(a_bytes[i + 1]);
					valid = valid &&
						!(c == 0xE0u && c1 < 0xA0u) &&
						!(c == 0xEDu && c1 >= 0xA0u);
				}
				if (length == 4)
				{
					const auto c1 = std::to_integer<unsigned char>(a_bytes[i + 1]);
					valid = valid &&
						!(c == 0xF0u && c1 < 0x90u) &&
						!(c == 0xF4u && c1 >= 0x90u);
				}
				if (!valid)
				{
					AppendReplacement(result);
					a_changed = true;
					++i;
					continue;
				}
				for (size_t j = 0; j < length; ++j)
					result.push_back(static_cast<char>(
						std::to_integer<unsigned char>(a_bytes[i + j])));
				i += length;
			}
			return result;
		}

		[[nodiscard]] std::string VersionAfter(
			std::string_view a_text,
			std::string_view a_prefix)
		{
			const auto position = a_text.find(a_prefix);
			if (position == std::string_view::npos)
				return {};
			const auto start = position + a_prefix.size();
			auto end = start;
			while (end < a_text.size() &&
				((a_text[end] >= '0' && a_text[end] <= '9') ||
					a_text[end] == '.'))
				++end;
			return end == start ?
				std::string{} :
				std::string{ a_text.substr(start, end - start) };
		}

		[[nodiscard]] bool IsHex(char a_character) noexcept
		{
			return (a_character >= '0' && a_character <= '9') ||
				(a_character >= 'a' && a_character <= 'f') ||
				(a_character >= 'A' && a_character <= 'F');
		}

		[[nodiscard]] std::string ExceptionCodeAfter(
			std::string_view a_text,
			std::string_view a_prefix)
		{
			const auto position = a_text.find(a_prefix);
			if (position == std::string_view::npos)
				return {};
			auto start = position + a_prefix.size();
			while (start < a_text.size() &&
				(a_text[start] == ' ' || a_text[start] == '\t'))
				++start;
			if (start + 10 > a_text.size() ||
				a_text.substr(start, 2) != "0x")
				return {};
			for (size_t index = start + 2; index < start + 10; ++index)
				if (!IsHex(a_text[index]))
					return {};
			return std::string{ a_text.substr(start, 10) };
		}

		[[nodiscard]] std::string FaultLocation(
			std::string_view a_text)
		{
			const auto start = a_text.find("Unhandled exception");
			if (start == std::string_view::npos)
				return {};
			const auto end = a_text.find('\n', start);
			const auto line = a_text.substr(
				start,
				end == std::string_view::npos ? a_text.size() - start : end - start);
			const auto address = line.find(" at 0x");
			if (address == std::string_view::npos)
				return {};
			auto moduleStart = line.find(' ', address + 6);
			if (moduleStart == std::string_view::npos)
				return {};
			while (moduleStart < line.size() && line[moduleStart] == ' ')
				++moduleStart;
			const auto plus = line.find('+', moduleStart);
			if (plus == std::string_view::npos || plus == moduleStart)
				return {};
			const auto module = line.substr(moduleStart, plus - moduleStart);
			if (module.find_first_of("\\/:") != std::string_view::npos)
				return {};
			auto offsetStart = plus + 1;
			if (line.substr(offsetStart, 2) == "0x")
				offsetStart += 2;
			auto offsetEnd = offsetStart;
			while (offsetEnd < line.size() && IsHex(line[offsetEnd]))
				++offsetEnd;
			if (offsetEnd == offsetStart)
				return {};
			return std::string{ module } + "+0x" +
				std::string{ line.substr(offsetStart, offsetEnd - offsetStart) };
		}

		[[nodiscard]] std::string ExceptionName(std::string_view a_text)
		{
			constexpr std::string_view prefix{ "Unhandled exception \"" };
			const auto position = a_text.find(prefix);
			if (position == std::string_view::npos)
				return {};
			const auto start = position + prefix.size();
			const auto end = a_text.find('"', start);
			if (end == std::string_view::npos ||
				a_text.substr(end, 7) != "\" at 0x")
				return {};
			const auto name = a_text.substr(start, end - start);
			static constexpr std::array names{
				"EXCEPTION_ACCESS_VIOLATION"sv,
				"EXCEPTION_ARRAY_BOUNDS_EXCEEDED"sv,
				"EXCEPTION_BREAKPOINT"sv,
				"EXCEPTION_DATATYPE_MISALIGNMENT"sv,
				"EXCEPTION_FLT_DENORMAL_OPERAND"sv,
				"EXCEPTION_FLT_DIVIDE_BY_ZERO"sv,
				"EXCEPTION_FLT_INEXACT_RESULT"sv,
				"EXCEPTION_FLT_INVALID_OPERATION"sv,
				"EXCEPTION_FLT_OVERFLOW"sv,
				"EXCEPTION_FLT_STACK_CHECK"sv,
				"EXCEPTION_FLT_UNDERFLOW"sv,
				"EXCEPTION_ILLEGAL_INSTRUCTION"sv,
				"EXCEPTION_IN_PAGE_ERROR"sv,
				"EXCEPTION_INT_DIVIDE_BY_ZERO"sv,
				"EXCEPTION_INT_OVERFLOW"sv,
				"EXCEPTION_INVALID_DISPOSITION"sv,
				"EXCEPTION_NONCONTINUABLE_EXCEPTION"sv,
				"EXCEPTION_PRIV_INSTRUCTION"sv,
				"EXCEPTION_SINGLE_STEP"sv,
				"EXCEPTION_STACK_OVERFLOW"sv,
				"C++ Exception"sv
			};
			return std::ranges::find(names, name) == names.end() ?
				std::string{} : std::string{ name };
		}

		[[nodiscard]] bool IsKnownSection(std::string_view a_line) noexcept
		{
			static constexpr std::array sections{
				"EXCEPTION RECORD:"sv,
				"PROBABLE CALL STACK:"sv,
				"CALL STACK ([P]robable / [S]tack scan):"sv,
				"CALL STACK (HYBRID):"sv,
				"PROCESS INFO:"sv,
				"THREAD CONTEXT (HEURISTIC):"sv,
				"REGISTERS:"sv,
				"STACK:"sv,
				"MODULES:"sv,
				"F4SE PLUGINS:"sv,
				"PLUGINS:"sv,
				"SYSTEM SPECS:"sv,
				"MEMORY:"sv,
				"THREADS:"sv,
				"RAW CALL STACK:"sv
			};
			return std::ranges::find(sections, a_line) != sections.end();
		}

		void BuildLineOffsets(ReportReadSnapshot& a_snapshot)
		{
			a_snapshot.lineOffsets.clear();
			a_snapshot.lineOffsets.push_back(0);
			for (size_t i = 0; i < a_snapshot.text.size(); ++i)
			{
				if (a_snapshot.text[i] == '\n' && i + 1 < a_snapshot.text.size())
					a_snapshot.lineOffsets.push_back(i + 1);
			}
		}
	}

	bool TryParseReportName(
		std::string_view a_name,
		ReportKind& a_kind,
		std::string& a_timestamp) noexcept
	{
		constexpr std::string_view crashPrefix{ "crash-" };
		constexpr std::string_view threadPrefix{ "threaddump-" };
		std::string_view stamp;
		if (a_name.starts_with(crashPrefix))
		{
			a_kind = ReportKind::kCrash;
			stamp = a_name.substr(crashPrefix.size());
		}
		else if (a_name.starts_with(threadPrefix))
		{
			a_kind = ReportKind::kThreadDump;
			stamp = a_name.substr(threadPrefix.size());
		}
		else
			return false;
		if (!stamp.ends_with(".log"))
			return false;
		stamp.remove_suffix(4);
		if (stamp.size() != 19)
			return false;
		for (size_t i = 0; i < stamp.size(); ++i)
		{
			const auto dash = i == 4 || i == 7 || i == 10 ||
				i == 13 || i == 16;
			if (dash ? stamp[i] != '-' :
				(stamp[i] < '0' || stamp[i] > '9'))
				return false;
		}
		const auto number = [&](size_t a_offset, size_t a_length) {
			unsigned value{};
			const auto begin = stamp.data() + a_offset;
			const auto end = begin + a_length;
			const auto parsed = std::from_chars(begin, end, value);
			return parsed.ec == std::errc{} && parsed.ptr == end ?
				std::optional<unsigned>{ value } :
				std::nullopt;
		};
		const auto month = number(5, 2);
		const auto day = number(8, 2);
		const auto hour = number(11, 2);
		const auto minute = number(14, 2);
		const auto second = number(17, 2);
		if (!month || *month < 1 || *month > 12 ||
			!day || *day < 1 || *day > 31 ||
			!hour || *hour > 23 ||
			!minute || *minute > 59 ||
			!second || *second > 59)
			return false;
		a_timestamp.assign(stamp);
		return true;
	}

	std::string NormalizeReportText(
		std::span<const std::byte> a_bytes,
		bool& a_normalized,
		std::string& a_notice)
	{
		a_normalized = false;
		a_notice.clear();
		if (a_bytes.size() >= 2 &&
			std::to_integer<unsigned char>(a_bytes[0]) == 0xFFu &&
			std::to_integer<unsigned char>(a_bytes[1]) == 0xFEu)
		{
			std::vector<wchar_t> text;
			text.reserve((a_bytes.size() - 2) / 2);
			for (size_t i = 2; i + 1 < a_bytes.size(); i += 2)
			{
				const auto low = std::to_integer<unsigned char>(a_bytes[i]);
				const auto high = std::to_integer<unsigned char>(a_bytes[i + 1]);
				text.push_back(static_cast<wchar_t>(low | (high << 8u)));
			}
			a_normalized = true;
			a_notice = "UTF-16LE text was normalized to UTF-8 for display.";
			return Utf16ToUtf8(text.data(), text.size());
		}
		if (a_bytes.size() >= 2 &&
			std::to_integer<unsigned char>(a_bytes[0]) == 0xFEu &&
			std::to_integer<unsigned char>(a_bytes[1]) == 0xFFu)
		{
			std::vector<wchar_t> text;
			text.reserve((a_bytes.size() - 2) / 2);
			for (size_t i = 2; i + 1 < a_bytes.size(); i += 2)
			{
				const auto high = std::to_integer<unsigned char>(a_bytes[i]);
				const auto low = std::to_integer<unsigned char>(a_bytes[i + 1]);
				text.push_back(static_cast<wchar_t>((high << 8u) | low));
			}
			a_normalized = true;
			a_notice = "UTF-16BE text was normalized to UTF-8 for display.";
			return Utf16ToUtf8(text.data(), text.size());
		}
		auto content = a_bytes;
		if (content.size() >= 3 &&
			std::to_integer<unsigned char>(content[0]) == 0xEFu &&
			std::to_integer<unsigned char>(content[1]) == 0xBBu &&
			std::to_integer<unsigned char>(content[2]) == 0xBFu)
		{
			content = content.subspan(3);
			a_normalized = true;
			a_notice = "The UTF-8 byte-order mark was removed for display.";
		}
		auto invalidReplaced = false;
		auto result = NormalizeUtf8(content, invalidReplaced);
		if (invalidReplaced)
		{
			a_normalized = true;
			a_notice = "Invalid text bytes were replaced with Unicode replacement characters.";
		}
		return result;
	}

	ReportMetadata ParseReportMetadata(std::string_view a_text)
	{
		ReportMetadata result;
		const auto header = a_text.substr(
			0,
			(std::min)(a_text.size(), size_t{ 32 * 1024 }));
		result.gameVersion = VersionAfter(header, "Fallout 4 v");
		result.loggerVersion =
			VersionAfter(header, "Addictol Crash Logger v");
		for (const auto prefix : {
			"Exception Code:"sv,
			"EXCEPTION_CODE:"sv,
			"Code:"sv })
		{
			result.exceptionCode = ExceptionCodeAfter(header, prefix);
			if (!result.exceptionCode.empty())
				break;
		}
		result.exceptionName = ExceptionName(header);
		result.faultLocation = FaultLocation(header);

		size_t lineIndex{};
		size_t start{};
		while (start <= a_text.size())
		{
			const auto end = a_text.find('\n', start);
			auto line = a_text.substr(
				start,
				end == std::string_view::npos ? a_text.size() - start : end - start);
			if (!line.empty() && line.back() == '\r')
				line.remove_suffix(1);
			if (IsKnownSection(line))
				result.sections.emplace_back(std::string{ line }, lineIndex);
			++lineIndex;
			if (end == std::string_view::npos)
				break;
			start = end + 1;
		}
		return result;
	}

	std::string BuildReportSummary(
		const ReportRecord& a_record,
		const ReportMetadata& a_metadata)
	{
		std::ostringstream output;
		output << "Report kind: " <<
			(a_record.kind == ReportKind::kCrash ? "Crash" : "Thread dump") <<
			'\n';
		output << "Report file: " << a_record.basename << '\n';
		output << "Recorded timestamp: " << a_record.recordedTimestamp << '\n';
		if (!a_metadata.gameVersion.empty())
			output << "Game version: " << a_metadata.gameVersion << '\n';
		if (!a_metadata.loggerVersion.empty())
			output << "Logger version: " << a_metadata.loggerVersion << '\n';
		if (!a_metadata.exceptionCode.empty())
			output << "Exception code: " << a_metadata.exceptionCode << '\n';
		else if (!a_metadata.exceptionName.empty())
			output << "Exception: " << a_metadata.exceptionName << '\n';
		if (!a_metadata.faultLocation.empty())
			output << "Fault location: " << a_metadata.faultLocation << '\n';
		return output.str();
	}

	ReportIndexSnapshot IndexReports(
		const std::filesystem::path& a_directory,
		uint64_t a_generation)
	{
		ReportIndexSnapshot result;
		result.generation = a_generation;
		result.directory = a_directory;
		std::error_code error;
		if (a_directory.empty() ||
			!std::filesystem::exists(a_directory, error) ||
			error)
		{
			result.error = error ?
				"Reports directory could not be inspected." :
				"Reports directory does not exist.";
			return result;
		}
		if (!std::filesystem::is_directory(a_directory, error) || error)
		{
			result.error = "Configured reports location is not a directory.";
			return result;
		}
		size_t enumerated{};
		try
		{
			for (const auto& entry : std::filesystem::directory_iterator(a_directory))
			{
				if (enumerated++ >= kMaximumDirectoryEntries)
				{
					result.partial = true;
					break;
				}
				if (!entry.is_regular_file(error) || error)
				{
					error.clear();
					continue;
				}
				ReportKind kind{};
				std::string timestamp;
				const auto basename = entry.path().filename().string();
				if (!TryParseReportName(basename, kind, timestamp))
					continue;
				const auto size = entry.file_size(error);
				if (error)
				{
					error.clear();
					continue;
				}
				auto dump = entry.path();
				dump.replace_extension(".dmp");
				const auto hasDump = std::filesystem::is_regular_file(dump, error);
				error.clear();
				result.reports.push_back({
					kind,
					std::filesystem::absolute(entry.path()).lexically_normal(),
					basename,
					std::move(timestamp),
					size,
					hasDump
				});
			}
		}
		catch (const std::exception& exception)
		{
			result.error = std::string{
				"Reports directory could not be enumerated: "
			} + exception.what();
			result.reports.clear();
			return result;
		}
		std::ranges::sort(
			result.reports,
			[](const ReportRecord& a_left, const ReportRecord& a_right) {
				if (a_left.recordedTimestamp != a_right.recordedTimestamp)
					return a_left.recordedTimestamp > a_right.recordedTimestamp;
				return a_left.basename > a_right.basename;
			});
		if (result.reports.size() > kMaximumIndexedReports)
		{
			result.reports.resize(kMaximumIndexedReports);
			result.partial = true;
		}
		return result;
	}

	ReportReadSnapshot ReadReport(
		const ReportRecord& a_record,
		uint64_t a_directoryGeneration,
		uint64_t a_selectionGeneration)
	{
		ReportReadSnapshot result;
		result.directoryGeneration = a_directoryGeneration;
		result.selectionGeneration = a_selectionGeneration;
		result.path = a_record.path;
		const auto file = CreateFileW(
			a_record.path.c_str(),
			GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			result.error = "Report is missing or unreadable; refresh and retry.";
			return result;
		}
		FileIdentity before{};
		if (!ReadIdentity(file, before))
		{
			CloseHandle(file);
			result.error = "Report identity could not be read.";
			return result;
		}
		const uint64_t fileSize =
			(static_cast<uint64_t>(before.sizeHigh) << 32u) | before.sizeLow;
		const auto toRead = static_cast<DWORD>(
			(std::min)(fileSize, static_cast<uint64_t>(kMaximumLoadedReportBytes)));
		std::vector<std::byte> bytes(toRead);
		DWORD read{};
		const auto readSucceeded =
			toRead == 0 || ReadFile(file, bytes.data(), toRead, &read, nullptr);
		bytes.resize(read);
		FileIdentity after{};
		const auto afterSucceeded = ReadIdentity(file, after);
		CloseHandle(file);
		if (!readSucceeded || !afterSucceeded)
		{
			result.error = "Report could not be read completely.";
			return result;
		}
		if (!(before == after))
		{
			result.error = "Report changed; refresh/retry.";
			return result;
		}
		const auto named = CreateFileW(
			a_record.path.c_str(),
			FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		FileIdentity current{};
		const auto currentSucceeded =
			named != INVALID_HANDLE_VALUE && ReadIdentity(named, current);
		if (named != INVALID_HANDLE_VALUE)
			CloseHandle(named);
		if (!currentSucceeded || !SameFileObject(before, current) ||
			!(before == current))
		{
			result.error = "Report changed; refresh/retry.";
			return result;
		}
		result.truncated = fileSize > kMaximumLoadedReportBytes;
		result.text = NormalizeReportText(
			bytes,
			result.encodingNormalized,
			result.encodingNotice);
		result.metadata = ParseReportMetadata(result.text);
		result.summary = BuildReportSummary(a_record, result.metadata);
		BuildLineOffsets(result);
		return result;
	}

	ReportRepository& ReportRepository::GetSingleton()
	{
		static auto* singleton = new ReportRepository;
		return *singleton;
	}

	ReportRepository::ReportRepository() :
		m_index(std::make_shared<const ReportIndexSnapshot>()),
		m_read(std::make_shared<const ReportReadSnapshot>())
	{
		std::thread([this] { Worker(); }).detach();
	}

	void ReportRepository::RequestRefresh(
		const std::filesystem::path& a_directory)
	{
		{
			const std::scoped_lock lock{ m_mutex };
			m_pendingDirectory = a_directory;
			m_refreshPending = true;
			++m_requestedGeneration;
			auto loading = std::make_shared<ReportIndexSnapshot>(*m_index);
			loading->generation = m_requestedGeneration;
			loading->directory = a_directory;
			loading->loading = true;
			loading->error.clear();
			m_index = std::move(loading);
		}
		m_condition.notify_one();
	}

	void ReportRepository::RequestRead(
		const ReportRecord& a_record,
		uint64_t a_directoryGeneration,
		uint64_t a_selectionGeneration)
	{
		{
			const std::scoped_lock lock{ m_mutex };
			m_pendingRead = ReadRequest{
				a_record,
				a_directoryGeneration,
				a_selectionGeneration
			};
			auto loading = std::make_shared<ReportReadSnapshot>();
			loading->directoryGeneration = a_directoryGeneration;
			loading->selectionGeneration = a_selectionGeneration;
			loading->path = a_record.path;
			loading->loading = true;
			m_read = std::move(loading);
		}
		m_condition.notify_one();
	}

	std::shared_ptr<const ReportIndexSnapshot>
		ReportRepository::IndexSnapshot() const
	{
		const std::scoped_lock lock{ m_mutex };
		return m_index;
	}

	std::shared_ptr<const ReportReadSnapshot>
		ReportRepository::ReadSnapshot() const
	{
		const std::scoped_lock lock{ m_mutex };
		return m_read;
	}

	void ReportRepository::Worker()
	{
		for (;;)
		{
			std::filesystem::path directory;
			std::optional<ReadRequest> read;
			uint64_t generation{};
			{
				std::unique_lock lock{ m_mutex };
				m_condition.wait(lock, [this] {
					return m_refreshPending || m_pendingRead.has_value();
				});
				if (m_refreshPending)
				{
					directory = m_pendingDirectory;
					generation = m_requestedGeneration;
					m_refreshPending = false;
				}
				else
				{
					read = std::move(m_pendingRead);
					m_pendingRead.reset();
				}
			}
			if (!directory.empty() || generation != 0)
			{
				auto result = std::make_shared<ReportIndexSnapshot>(
					IndexReports(directory, generation));
				const std::scoped_lock lock{ m_mutex };
				if (result->generation == m_requestedGeneration)
					m_index = std::move(result);
				continue;
			}
			if (read)
			{
				auto result = std::make_shared<ReportReadSnapshot>(
					ReadReport(
						read->record,
						read->directoryGeneration,
						read->selectionGeneration));
				const std::scoped_lock lock{ m_mutex };
				if (m_read->directoryGeneration == result->directoryGeneration &&
					m_read->selectionGeneration == result->selectionGeneration)
					m_read = std::move(result);
			}
		}
	}
}
