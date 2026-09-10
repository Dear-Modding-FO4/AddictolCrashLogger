#pragma once

#include <cstdint>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace CrashUI
{
	inline constexpr size_t kMaximumDirectoryEntries = 10000;
	inline constexpr size_t kMaximumIndexedReports = 1000;
	inline constexpr size_t kMaximumLoadedReportBytes = 2 * 1024 * 1024;
	inline constexpr size_t kPreviewLinesPerPage = 200;

	enum class ReportKind
	{
		kCrash,
		kThreadDump
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
	};

	struct ReportReadSnapshot
	{
		uint64_t directoryGeneration{};
		uint64_t selectionGeneration{};
		std::filesystem::path path;
		std::string text;
		std::vector<size_t> lineOffsets;
		ReportMetadata metadata;
		std::string summary;
		bool loading{};
		bool truncated{};
		bool encodingNormalized{};
		std::string encodingNotice;
		std::string error;
	};

	[[nodiscard]] bool TryParseReportName(
		std::string_view a_name,
		ReportKind& a_kind,
		std::string& a_timestamp) noexcept;
	[[nodiscard]] std::string NormalizeReportText(
		std::span<const std::byte> a_bytes,
		bool& a_normalized,
		std::string& a_notice);
	[[nodiscard]] ReportMetadata ParseReportMetadata(std::string_view a_text);
	[[nodiscard]] std::string BuildReportSummary(
		const ReportRecord& a_record,
		const ReportMetadata& a_metadata);
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
		[[nodiscard]] std::shared_ptr<const ReportIndexSnapshot> IndexSnapshot() const;
		[[nodiscard]] std::shared_ptr<const ReportReadSnapshot> ReadSnapshot() const;

	private:
		ReportRepository();
		void Worker();

		struct ReadRequest
		{
			ReportRecord record;
			uint64_t directoryGeneration{};
			uint64_t selectionGeneration{};
		};

		mutable std::mutex m_mutex;
		std::condition_variable m_condition;
		std::filesystem::path m_pendingDirectory;
		std::optional<ReadRequest> m_pendingRead;
		std::shared_ptr<const ReportIndexSnapshot> m_index;
		std::shared_ptr<const ReportReadSnapshot> m_read;
		uint64_t m_requestedGeneration{};
		bool m_refreshPending{};
	};
}
