#include "ReportRepository.h"

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <exception>
#include <functional>
#include <thread>

namespace CrashUI
{
	using namespace std::literals;

	namespace
	{
		constexpr DWORD kReadChunkBytes = 64 * 1024;

		struct OpenedReport
		{
			std::vector<std::byte> bytes;
			ReportFileIdentity identity;
			bool truncated{};
			bool cancelled{};
			std::string error;
		};

		[[nodiscard]] bool ReadIdentity(
			HANDLE a_file,
			ReportFileIdentity& a_identity)
		{
			BY_HANDLE_FILE_INFORMATION info{};
			if (!GetFileInformationByHandle(a_file, &info))
				return false;
			a_identity = {
				info.dwVolumeSerialNumber,
				(static_cast<uint64_t>(info.nFileIndexHigh) << 32u) |
					info.nFileIndexLow,
				(static_cast<uint64_t>(info.nFileSizeHigh) << 32u) |
					info.nFileSizeLow,
				(static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32u) |
					info.ftLastWriteTime.dwLowDateTime,
				true
			};
			return true;
		}

		[[nodiscard]] bool SameFileObject(
			const ReportFileIdentity& a_left,
			const ReportFileIdentity& a_right) noexcept
		{
			return a_left.valid && a_right.valid &&
				a_left.volume == a_right.volume &&
				a_left.fileIndex == a_right.fileIndex;
		}

		[[nodiscard]] bool RevalidateNamedPath(
			const std::filesystem::path& a_path,
			const ReportFileIdentity& a_expected)
		{
			const auto named = CreateFileW(
				a_path.c_str(),
				FILE_READ_ATTRIBUTES,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);
			if (named == INVALID_HANDLE_VALUE)
				return false;
			ReportFileIdentity current{};
			const auto succeeded = ReadIdentity(named, current);
			CloseHandle(named);
			return succeeded && SameFileObject(a_expected, current) &&
				a_expected == current;
		}

		[[nodiscard]] OpenedReport ReadOpenedReport(
			const std::filesystem::path& a_path,
			size_t a_maximumBytes,
			const std::function<bool()>& a_current)
		{
			OpenedReport result;
			const auto file = CreateFileW(
				a_path.c_str(),
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
			ReportFileIdentity before{};
			if (!ReadIdentity(file, before))
			{
				CloseHandle(file);
				result.error = "Report identity could not be read.";
				return result;
			}
			const auto retained = static_cast<size_t>((std::min)(
				before.size,
				static_cast<uint64_t>(a_maximumBytes)));
			result.bytes.resize(retained);
			size_t total{};
			while (total < retained)
			{
				if (!a_current())
				{
					result.cancelled = true;
					break;
				}
				const auto chunk = static_cast<DWORD>((std::min)(
					retained - total,
					static_cast<size_t>(kReadChunkBytes)));
				DWORD read{};
				if (!ReadFile(
						file,
						result.bytes.data() + total,
						chunk,
						&read,
						nullptr))
				{
					result.error = "Report could not be read completely.";
					break;
				}
				total += read;
				if (read != chunk)
				{
					result.error = "Report ended before its captured file size.";
					break;
				}
			}
			result.bytes.resize(total);
			ReportFileIdentity after{};
			const auto afterSucceeded = ReadIdentity(file, after);
			CloseHandle(file);
			if (result.cancelled)
				return result;
			if (!result.error.empty() || !afterSucceeded)
			{
				if (result.error.empty())
					result.error = "Report identity could not be rechecked.";
				return result;
			}
			if (!(before == after) || !RevalidateNamedPath(a_path, before))
			{
				result.error = "Report changed; refresh/retry.";
				return result;
			}
			result.identity = before;
			result.truncated = before.size > a_maximumBytes;
			return result;
		}

		[[nodiscard]] std::string Utf16ToUtf8(
			const wchar_t* a_text,
			size_t a_length)
		{
			if (a_length == 0)
				return {};
			const auto needed = WideCharToMultiByte(
				CP_UTF8,
				WC_ERR_INVALID_CHARS,
				a_text,
				static_cast<int>(a_length),
				nullptr,
				0,
				nullptr,
				nullptr);
			if (needed <= 0)
				return {};
			std::string result(static_cast<size_t>(needed), '\0');
			if (WideCharToMultiByte(
					CP_UTF8,
					WC_ERR_INVALID_CHARS,
					a_text,
					static_cast<int>(a_length),
					result.data(),
					needed,
					nullptr,
					nullptr) <= 0)
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

		void BuildLineOffsets(ReportReadSnapshot& a_snapshot)
		{
			a_snapshot.lineOffsets.clear();
			a_snapshot.lineOffsets.push_back(0);
			for (size_t i = 0; i < a_snapshot.text.size(); ++i)
				if (a_snapshot.text[i] == '\n' && i + 1 < a_snapshot.text.size())
					a_snapshot.lineOffsets.push_back(i + 1);
		}

		[[nodiscard]] ReportReadSnapshot ReadReportImpl(
			const ReportRecord& a_record,
			uint64_t a_directoryGeneration,
			uint64_t a_selectionGeneration,
			const std::function<bool()>& a_current)
		{
			ReportReadSnapshot result;
			result.directoryGeneration = a_directoryGeneration;
			result.selectionGeneration = a_selectionGeneration;
			result.path = a_record.path;
			auto opened = ReadOpenedReport(
				a_record.path,
				kMaximumLoadedReportBytes,
				a_current);
			if (opened.cancelled)
			{
				result.error = "Report read was superseded.";
				return result;
			}
			if (!opened.error.empty())
			{
				result.error = std::move(opened.error);
				return result;
			}
			result.truncated = opened.truncated;
			const auto decoded = DecodeReportText(opened.bytes);
			result.encodingNormalized = decoded.normalized;
			result.encodingNotice = decoded.notice;
			if (decoded.lossless)
				result.text = decoded.text;
			else
				result.text = NormalizeReportText(
					opened.bytes,
					result.encodingNormalized,
					result.encodingNotice);
			result.parsed = ParseReportView(
				result.text,
				decoded.lossless ? decoded.encoding : ReportEncoding::kUnsupported,
				!result.truncated);
			result.metadata = AdaptReportMetadata(result.parsed);
			result.summary = BuildReportSummary(a_record, result.parsed);
			BuildLineOffsets(result);
			return result;
		}

		struct ParsedOpenedReport
		{
			std::shared_ptr<const ParsedReportView> parsed;
			ReportFileIdentity identity;
			std::string error;
			bool cancelled{};
		};

		[[nodiscard]] ParsedOpenedReport ParseOpenedReport(
			const ReportRecord& a_record,
			const std::function<bool()>& a_current)
		{
			ParsedOpenedReport result;
			auto opened = ReadOpenedReport(
				a_record.path,
				kMaximumComparisonRawBytes,
				a_current);
			result.cancelled = opened.cancelled;
			if (opened.cancelled)
				return result;
			if (!opened.error.empty())
			{
				result.error = std::move(opened.error);
				return result;
			}
			result.identity = opened.identity;
			auto decoded = DecodeReportText(opened.bytes);
			if (!decoded.lossless)
			{
				auto parsed = ParseReportView(
					{},
					ReportEncoding::kUnsupported,
					!opened.truncated);
				parsed.notes.push_back(decoded.notice);
				result.parsed =
					std::make_shared<const ParsedReportView>(std::move(parsed));
				return result;
			}
			auto parsed = ParseReportView(
				decoded.text,
				decoded.encoding,
				!opened.truncated,
				kMaximumComparisonParsedBytes);
			result.parsed =
				std::make_shared<const ParsedReportView>(std::move(parsed));
			return result;
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
			a_notice =
				"Invalid text bytes were replaced with Unicode replacement characters.";
		}
		return result;
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
		return ReadReportImpl(
			a_record,
			a_directoryGeneration,
			a_selectionGeneration,
			[] { return true; });
	}

	ReportRepository& ReportRepository::GetSingleton()
	{
		static auto* singleton = new ReportRepository;
		return *singleton;
	}

	ReportRepository::ReportRepository() :
		m_index(std::make_shared<const ReportIndexSnapshot>()),
		m_read(std::make_shared<const ReportReadSnapshot>()),
		m_search(std::make_shared<const ReportSearchSnapshot>()),
		m_comparison(std::make_shared<const ReportComparisonSnapshot>())
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
			if (m_comparison->comparisonGeneration != 0)
			{
				auto stale =
					std::make_shared<ReportComparisonSnapshot>(*m_comparison);
				stale->loading = false;
				stale->stale = true;
				stale->validated = false;
				stale->validationPending = false;
				m_comparison = std::move(stale);
			}
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

	void ReportRepository::RequestSearch(
		std::shared_ptr<const ReportReadSnapshot> a_read,
		std::string a_query,
		uint64_t a_searchGeneration)
	{
		if (!a_read)
			return;
		if (a_query.size() > kMaximumSearchQueryBytes)
			a_query.resize(kMaximumSearchQueryBytes);
		{
			const std::scoped_lock lock{ m_mutex };
			m_pendingSearch = SearchRequest{
				a_read,
				std::move(a_query),
				a_searchGeneration
			};
			auto loading = std::make_shared<ReportSearchSnapshot>();
			loading->directoryGeneration = a_read->directoryGeneration;
			loading->selectionGeneration = a_read->selectionGeneration;
			loading->searchGeneration = a_searchGeneration;
			loading->path = a_read->path;
			loading->query = m_pendingSearch->query;
			loading->loading = true;
			m_search = std::move(loading);
		}
		m_condition.notify_one();
	}

	void ReportRepository::RequestComparison(
		const ReportRecord& a_left,
		const ReportRecord& a_right,
		uint64_t a_directoryGeneration,
		uint64_t a_comparisonGeneration)
	{
		{
			const std::scoped_lock lock{ m_mutex };
			m_pendingComparison = ComparisonRequest{
				a_left,
				a_right,
				a_directoryGeneration,
				a_comparisonGeneration,
				false,
				{},
				{}
			};
			auto loading = std::make_shared<ReportComparisonSnapshot>();
			loading->directoryGeneration = a_directoryGeneration;
			loading->comparisonGeneration = a_comparisonGeneration;
			loading->left = a_left;
			loading->right = a_right;
			loading->loading = true;
			m_comparison = std::move(loading);
		}
		m_condition.notify_one();
	}

	void ReportRepository::RequestComparisonValidation(
		uint64_t a_comparisonGeneration)
	{
		{
			const std::scoped_lock lock{ m_mutex };
			if (m_comparison->comparisonGeneration != a_comparisonGeneration ||
				!m_comparison->leftIdentity.valid ||
				!m_comparison->rightIdentity.valid)
				return;
			m_pendingComparison = ComparisonRequest{
				m_comparison->left,
				m_comparison->right,
				m_comparison->directoryGeneration,
				a_comparisonGeneration,
				true,
				m_comparison->leftIdentity,
				m_comparison->rightIdentity
			};
			auto validating =
				std::make_shared<ReportComparisonSnapshot>(*m_comparison);
			validating->validated = false;
			validating->validationPending = true;
			m_comparison = std::move(validating);
		}
		m_condition.notify_one();
	}

	void ReportRepository::MarkComparisonStale(
		uint64_t a_comparisonGeneration)
	{
		const std::scoped_lock lock{ m_mutex };
		if (m_comparison->comparisonGeneration == 0)
			return;
		auto stale = std::make_shared<ReportComparisonSnapshot>(*m_comparison);
		if (a_comparisonGeneration != 0)
			stale->comparisonGeneration = a_comparisonGeneration;
		stale->loading = false;
		stale->stale = true;
		stale->validated = false;
		stale->validationPending = false;
		m_comparison = std::move(stale);
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

	std::shared_ptr<const ReportSearchSnapshot>
		ReportRepository::SearchSnapshot() const
	{
		const std::scoped_lock lock{ m_mutex };
		return m_search;
	}

	std::shared_ptr<const ReportComparisonSnapshot>
		ReportRepository::ComparisonSnapshot() const
	{
		const std::scoped_lock lock{ m_mutex };
		return m_comparison;
	}

	void ReportRepository::Worker()
	{
		for (;;)
		{
			std::filesystem::path directory;
			std::optional<ReadRequest> read;
			std::optional<SearchRequest> search;
			std::optional<ComparisonRequest> comparison;
			uint64_t generation{};
			{
				std::unique_lock lock{ m_mutex };
				m_condition.wait(lock, [this] {
					return m_refreshPending || m_pendingRead.has_value() ||
						m_pendingSearch.has_value() ||
						m_pendingComparison.has_value();
				});
				if (m_pendingRead)
				{
					read = std::move(m_pendingRead);
					m_pendingRead.reset();
				}
				else if (m_pendingSearch)
				{
					search = std::move(m_pendingSearch);
					m_pendingSearch.reset();
				}
				else if (m_pendingComparison)
				{
					comparison = std::move(m_pendingComparison);
					m_pendingComparison.reset();
				}
				else
				{
					directory = m_pendingDirectory;
					generation = m_requestedGeneration;
					m_refreshPending = false;
				}
			}

			if (read)
			{
				const auto current = [this, request = *read] {
					const std::scoped_lock lock{ m_mutex };
					return m_read->directoryGeneration ==
							request.directoryGeneration &&
						m_read->selectionGeneration ==
							request.selectionGeneration &&
						(!m_pendingRead ||
							m_pendingRead->selectionGeneration <=
								request.selectionGeneration);
				};
				auto result = std::make_shared<ReportReadSnapshot>(
					ReadReportImpl(
						read->record,
						read->directoryGeneration,
						read->selectionGeneration,
						current));
				const std::scoped_lock lock{ m_mutex };
				if (m_read->directoryGeneration == result->directoryGeneration &&
					m_read->selectionGeneration == result->selectionGeneration &&
					result->error != "Report read was superseded.")
					m_read = std::move(result);
				continue;
			}

			if (search)
			{
				auto result = std::make_shared<ReportSearchSnapshot>();
				result->directoryGeneration = search->read->directoryGeneration;
				result->selectionGeneration = search->read->selectionGeneration;
				result->searchGeneration = search->searchGeneration;
				result->path = search->read->path;
				result->query = search->query;
				result->byteOffsets = FindLiteralReportMatches(
					search->read->text,
					search->query,
					result->capped);
				const std::scoped_lock lock{ m_mutex };
				if (m_search->searchGeneration == result->searchGeneration &&
					m_search->selectionGeneration == result->selectionGeneration &&
					m_search->path == result->path)
					m_search = std::move(result);
				continue;
			}

			if (comparison)
			{
				if (comparison->validateOnly)
				{
					const auto valid =
						RevalidateNamedPath(
							comparison->left.path,
							comparison->expectedLeftIdentity) &&
						RevalidateNamedPath(
							comparison->right.path,
							comparison->expectedRightIdentity);
					const std::scoped_lock lock{ m_mutex };
					if (m_comparison->comparisonGeneration ==
						comparison->comparisonGeneration)
					{
						auto result =
							std::make_shared<ReportComparisonSnapshot>(*m_comparison);
						result->validationPending = false;
						result->validated = valid;
						result->stale = !valid ||
							result->directoryGeneration != m_index->generation;
						if (!valid)
							result->error =
								"A selected report was replaced or changed; choose the reports again.";
						m_comparison = std::move(result);
					}
					continue;
				}
				const auto current = [this, request = *comparison] {
					const std::scoped_lock lock{ m_mutex };
					return m_comparison->comparisonGeneration ==
							request.comparisonGeneration &&
						m_comparison->directoryGeneration ==
							request.directoryGeneration &&
						!m_pendingRead &&
						!m_pendingSearch &&
						!m_refreshPending &&
						(!m_pendingComparison ||
							m_pendingComparison->comparisonGeneration <=
								request.comparisonGeneration);
				};
				const auto requeueIfPreempted = [this, request = *comparison] {
					{
						const std::scoped_lock lock{ m_mutex };
						if (m_comparison->comparisonGeneration !=
								request.comparisonGeneration ||
							m_comparison->directoryGeneration !=
								request.directoryGeneration ||
							m_requestedGeneration !=
								request.directoryGeneration ||
							m_pendingComparison)
							return;
						m_pendingComparison = request;
					}
					m_condition.notify_one();
				};
				auto result = std::make_shared<ReportComparisonSnapshot>();
				result->directoryGeneration = comparison->directoryGeneration;
				result->comparisonGeneration = comparison->comparisonGeneration;
				result->left = comparison->left;
				result->right = comparison->right;
				const auto left = ParseOpenedReport(comparison->left, current);
				if (left.cancelled)
				{
					requeueIfPreempted();
					continue;
				}
				if (!left.error.empty())
				{
					result->error = "A: " + left.error;
					result->stale = true;
				}
				else
				{
					result->leftParsed = left.parsed;
					result->leftIdentity = left.identity;
					const auto right =
						ParseOpenedReport(comparison->right, current);
					if (right.cancelled)
					{
						requeueIfPreempted();
						continue;
					}
					if (!right.error.empty())
					{
						result->error = "B: " + right.error;
						result->stale = true;
					}
					else
					{
						result->rightParsed = right.parsed;
						result->rightIdentity = right.identity;
						if (!RevalidateNamedPath(
								comparison->left.path,
								left.identity) ||
							!RevalidateNamedPath(
								comparison->right.path,
								right.identity))
						{
							result->error =
								"A selected report changed before comparison publication.";
							result->stale = true;
						}
						else
						{
							auto differences = CompareParsedReports(
								*left.parsed,
								*right.parsed);
							result->differences = std::move(differences.entries);
							result->differencesTruncated = differences.truncated;
							result->summary = BuildComparisonSummary(
								comparison->left,
								comparison->right,
								result->differences,
								result->differencesTruncated);
							result->validated = true;
						}
					}
				}
				const std::scoped_lock lock{ m_mutex };
				if (m_comparison->comparisonGeneration ==
						result->comparisonGeneration &&
					m_comparison->directoryGeneration ==
						result->directoryGeneration &&
					m_index->generation == result->directoryGeneration)
					m_comparison = std::move(result);
				continue;
			}

			auto result = std::make_shared<ReportIndexSnapshot>(
				IndexReports(directory, generation));
			const std::scoped_lock lock{ m_mutex };
			if (result->generation == m_requestedGeneration)
				m_index = std::move(result);
		}
	}
}
