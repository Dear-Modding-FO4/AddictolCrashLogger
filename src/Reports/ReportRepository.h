#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace CrashUI
{
	inline constexpr size_t kMaximumDirectoryEntries = 10000;
	inline constexpr size_t kMaximumIndexedReports = 1000;
	inline constexpr size_t kMaximumLoadedReportBytes = 2 * 1024 * 1024;
	inline constexpr size_t kMaximumComparisonRawBytes = 8 * 1024 * 1024;
	inline constexpr size_t kMaximumDecodedReportBytes = 24 * 1024 * 1024;
	inline constexpr size_t kMaximumParsedReportBytes = 2 * 1024 * 1024;
	inline constexpr size_t kMaximumComparisonRetainedBytes = 4 * 1024 * 1024;
	inline constexpr size_t kMaximumComparisonParsedBytes =
		(kMaximumComparisonRetainedBytes - 512 * 1024) / 2;
	inline constexpr size_t kMaximumParsedLineBytes = 8 * 1024;
	inline constexpr size_t kMaximumSearchQueryBytes = 512;
	inline constexpr size_t kMaximumSearchHits = 10000;
	inline constexpr size_t kPreviewLinesPerPage = 200;

	enum class ReportKind
	{
		kCrash,
		kThreadDump
	};

	enum class EvidenceState
	{
		kComplete,
		kPartial,
		kAbsent,
		kAmbiguous,
		kUnsupported
	};

	enum class ReportEncoding
	{
		kUtf8,
		kUtf8Bom,
		kUtf16LE,
		kUtf16BE,
		kUnsupported
	};

	struct SourceAnchor
	{
		std::string section;
		size_t line{};
		size_t byteOffset{};
	};

	struct EvidenceField
	{
		EvidenceState state{ EvidenceState::kAbsent };
		std::string value;
		std::optional<SourceAnchor> source;
	};

	struct SectionEvidence
	{
		std::string name;
		EvidenceState state{ EvidenceState::kAbsent };
		std::optional<SourceAnchor> source;
	};

	struct ModuleRecord
	{
		std::string name;
		std::string aslrBase;
		SourceAnchor source;
	};

	struct F4SEPluginRecord
	{
		std::string name;
		std::optional<std::string> reportedVersion;
		SourceAnchor source;
	};

	enum class PluginKind
	{
		kRegular,
		kLight
	};

	struct PluginRecord
	{
		std::string name;
		PluginKind kind{ PluginKind::kRegular };
		std::string reportedIndex;
		SourceAnchor source;
	};

	struct ParsedReportView
	{
		bool supportedGrammar{};
		bool fullyRead{};
		ReportEncoding encoding{ ReportEncoding::kUnsupported };
		EvidenceField gameVersion;
		EvidenceField loggerVersion;
		EvidenceField exceptionCode;
		EvidenceField exceptionName;
		EvidenceField faultLocation;
		std::vector<SectionEvidence> sections;
		SectionEvidence modulesSection{ "MODULES:" };
		SectionEvidence f4sePluginsSection{ "F4SE PLUGINS:" };
		SectionEvidence pluginsSection{ "PLUGINS:" };
		std::vector<ModuleRecord> modules;
		std::vector<F4SEPluginRecord> f4sePlugins;
		std::vector<PluginRecord> plugins;
		std::optional<size_t> reportedLightCount;
		std::optional<size_t> reportedRegularCount;
		std::optional<size_t> reportedTotalCount;
		size_t retainedBytes{};
		size_t retentionLimit{ kMaximumParsedReportBytes };
		bool retentionCapped{};
		std::vector<std::string> notes;
	};

	struct ReportRecord
	{
		ReportKind kind{ ReportKind::kCrash };
		std::filesystem::path path;
		std::string basename;
		std::string recordedTimestamp;
		uint64_t size{};
		bool hasMiniDump{};
	};

	struct ReportIndexSnapshot
	{
		uint64_t generation{};
		std::filesystem::path directory;
		std::vector<ReportRecord> reports;
		bool loading{};
		bool partial{};
		std::string error;
	};

	struct ReportMetadata
	{
		std::string gameVersion;
		std::string loggerVersion;
		std::string exceptionCode;
		std::string exceptionName;
		std::string faultLocation;
		std::vector<std::pair<std::string, size_t>> sections;
		std::vector<std::pair<std::string, SourceAnchor>> summarySources;
		EvidenceState status{ EvidenceState::kUnsupported };
	};

	struct DecodedReportText
	{
		std::string text;
		ReportEncoding encoding{ ReportEncoding::kUnsupported };
		bool normalized{};
		bool lossless{};
		std::string notice;
	};

	struct ReportReadSnapshot
	{
		uint64_t directoryGeneration{};
		uint64_t selectionGeneration{};
		std::filesystem::path path;
		std::string text;
		std::vector<size_t> lineOffsets;
		ReportMetadata metadata;
		ParsedReportView parsed;
		std::string summary;
		bool loading{};
		bool truncated{};
		bool encodingNormalized{};
		std::string encodingNotice;
		std::string error;
	};

	struct ReportSearchSnapshot
	{
		uint64_t directoryGeneration{};
		uint64_t selectionGeneration{};
		uint64_t searchGeneration{};
		std::filesystem::path path;
		std::string query;
		std::vector<size_t> byteOffsets;
		bool loading{};
		bool capped{};
		std::string error;
	};

	struct ReportFileIdentity
	{
		uint64_t volume{};
		uint64_t fileIndex{};
		uint64_t size{};
		uint64_t writeTime{};
		bool valid{};

		bool operator==(const ReportFileIdentity&) const noexcept = default;
	};

	struct ReportDifference
	{
		std::string category;
		std::string message;
		std::optional<SourceAnchor> leftSource;
		std::optional<SourceAnchor> rightSource;
	};

	struct ReportComparisonSnapshot
	{
		uint64_t directoryGeneration{};
		uint64_t comparisonGeneration{};
		ReportRecord left;
		ReportRecord right;
		ReportFileIdentity leftIdentity;
		ReportFileIdentity rightIdentity;
		std::shared_ptr<const ParsedReportView> leftParsed;
		std::shared_ptr<const ParsedReportView> rightParsed;
		std::vector<ReportDifference> differences;
		bool differencesTruncated{};
		std::string summary;
		bool loading{};
		bool stale{};
		bool validationPending{};
		bool validated{};
		std::string error;
	};

	struct ReportDifferences
	{
		std::vector<ReportDifference> entries;
		bool truncated{};
	};

	[[nodiscard]] const char* EvidenceStateName(EvidenceState a_state) noexcept;
	[[nodiscard]] bool TryParseReportName(
		std::string_view a_name,
		ReportKind& a_kind,
		std::string& a_timestamp) noexcept;
	[[nodiscard]] DecodedReportText DecodeReportText(
		std::span<const std::byte> a_bytes,
		size_t a_decodedLimit = kMaximumDecodedReportBytes);
	[[nodiscard]] std::string NormalizeReportText(
		std::span<const std::byte> a_bytes,
		bool& a_normalized,
		std::string& a_notice);
	[[nodiscard]] ParsedReportView ParseReportView(
		std::string_view a_text,
		ReportEncoding a_encoding,
		bool a_fullyRead,
		size_t a_retainedLimit = kMaximumParsedReportBytes);
	[[nodiscard]] ReportMetadata AdaptReportMetadata(
		const ParsedReportView& a_view);
	[[nodiscard]] ReportMetadata ParseReportMetadata(std::string_view a_text);
	[[nodiscard]] std::string BuildReportSummary(
		const ReportRecord& a_record,
		const ParsedReportView& a_view);
	[[nodiscard]] std::string BuildReportSummary(
		const ReportRecord& a_record,
		const ReportMetadata& a_metadata);
	[[nodiscard]] std::vector<size_t> FindLiteralReportMatches(
		std::string_view a_text,
		std::string_view a_query,
		bool& a_capped);
	[[nodiscard]] ReportDifferences CompareParsedReports(
		const ParsedReportView& a_left,
		const ParsedReportView& a_right);
	[[nodiscard]] std::string BuildComparisonSummary(
		const ReportRecord& a_left,
		const ReportRecord& a_right,
		std::span<const ReportDifference> a_differences,
		bool a_truncated);
	[[nodiscard]] ReportIndexSnapshot IndexReports(
		const std::filesystem::path& a_directory,
		uint64_t a_generation);
	[[nodiscard]] ReportReadSnapshot ReadReport(
		const ReportRecord& a_record,
		uint64_t a_directoryGeneration,
		uint64_t a_selectionGeneration);

	class ReportRepository
	{
	public:
		static ReportRepository& GetSingleton();

		void RequestRefresh(const std::filesystem::path& a_directory);
		void RequestRead(
			const ReportRecord& a_record,
			uint64_t a_directoryGeneration,
			uint64_t a_selectionGeneration);
		void RequestSearch(
			std::shared_ptr<const ReportReadSnapshot> a_read,
			std::string a_query,
			uint64_t a_searchGeneration);
		void RequestComparison(
			const ReportRecord& a_left,
			const ReportRecord& a_right,
			uint64_t a_directoryGeneration,
			uint64_t a_comparisonGeneration);
		void RequestComparisonValidation(uint64_t a_comparisonGeneration);
		void MarkComparisonStale(uint64_t a_comparisonGeneration = 0);
		[[nodiscard]] std::shared_ptr<const ReportIndexSnapshot> IndexSnapshot() const;
		[[nodiscard]] std::shared_ptr<const ReportReadSnapshot> ReadSnapshot() const;
		[[nodiscard]] std::shared_ptr<const ReportSearchSnapshot> SearchSnapshot() const;
		[[nodiscard]] std::shared_ptr<const ReportComparisonSnapshot> ComparisonSnapshot() const;

	private:
		ReportRepository();
		void Worker();

		struct ReadRequest
		{
			ReportRecord record;
			uint64_t directoryGeneration{};
			uint64_t selectionGeneration{};
		};

		struct SearchRequest
		{
			std::shared_ptr<const ReportReadSnapshot> read;
			std::string query;
			uint64_t searchGeneration{};
		};

		struct ComparisonRequest
		{
			ReportRecord left;
			ReportRecord right;
			uint64_t directoryGeneration{};
			uint64_t comparisonGeneration{};
			bool validateOnly{};
			ReportFileIdentity expectedLeftIdentity;
			ReportFileIdentity expectedRightIdentity;
		};

		mutable std::mutex m_mutex;
		std::condition_variable m_condition;
		std::filesystem::path m_pendingDirectory;
		std::optional<ReadRequest> m_pendingRead;
		std::optional<SearchRequest> m_pendingSearch;
		std::optional<ComparisonRequest> m_pendingComparison;
		std::shared_ptr<const ReportIndexSnapshot> m_index;
		std::shared_ptr<const ReportReadSnapshot> m_read;
		std::shared_ptr<const ReportSearchSnapshot> m_search;
		std::shared_ptr<const ReportComparisonSnapshot> m_comparison;
		uint64_t m_requestedGeneration{};
		bool m_refreshPending{};
	};
}
