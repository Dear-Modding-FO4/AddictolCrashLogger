#include "MenuUI.h"

#include "MenuStartupState.h"
#include "Menu/SymbolDiagnostics.h"
#include "Reports/ReportRepository.h"
#include "Settings/SettingsRepository.h"

#include <DearModdingUI/Client.h>
#include <DearModdingUI/IconGlyphs.h>

#include <REX/REX.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <format>
#include <limits>

namespace CrashUI
{
	namespace
	{
		constexpr dmui::ClientOptions kClientOptions{
			.capabilities = DMUI_CLIENT_CAPABILITY_NONE,
			.requiredServices = DMUI_HOST_SERVICE_EXTERNAL_OPEN,
			.minimumUIRevision = DMUI_UI_REVISION_1,
			.minimumUIAPISize = DMUI_UI_API_REQUIRED_SIZE
		};

		dmui::Client s_client{
			"dear-modding.addictol-crash-logger",
			"Addictol Crash Logger",
			dmui::Version{ PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR },
			"bug-beetle",
			{},
			kClientOptions
		};
		std::once_flag s_installOnce;
		std::atomic_bool s_installResult{ true };
		std::atomic_bool s_pagesOperational{ false };
		DMUI_PageHandle s_homePage{ DMUI_INVALID_PAGE_HANDLE };
		DMUI_PageHandle s_reportsPage{ DMUI_INVALID_PAGE_HANDLE };
		DMUI_PageHandle s_settingsPage{ DMUI_INVALID_PAGE_HANDLE };
		DMUI_PageHandle s_comparePage{ DMUI_INVALID_PAGE_HANDLE };
		DMUI_PageHandle s_diagnosticsPage{ DMUI_INVALID_PAGE_HANDLE };
		bool s_reportsActivated{};
		bool s_settingsActivated{};
		bool s_compareActivated{};
		uint64_t s_selectionGeneration{};
		std::filesystem::path s_selectedReport;
		size_t s_previewPage{};
		std::string s_reportFilter;
		std::string s_previewSearch;
		uint64_t s_searchGeneration{};
		uint64_t s_searchSelectionGeneration{};
		size_t s_currentSearchHit{ std::string::npos };
		std::string s_searchStatus;
		int s_reportKindFilter{};
		std::filesystem::path s_compareLeft;
		std::filesystem::path s_compareRight;
		uint64_t s_comparisonGeneration{};
		uint64_t s_compareValidatedIndexGeneration{};
		std::optional<uint64_t> s_pendingComparisonCopy;
		std::string s_comparisonActionStatus;
		SettingsPageState s_settingsState;
		uint64_t s_nextSaveOperation{ 1 };
		uint64_t s_settingsViewGeneration{};

		[[nodiscard]] bool HasRequiredHostOperations(
			const DMUI_HostAPI* a_api) noexcept
		{
			if (!a_api || a_api->structSize < DMUI_HOST_API_QUERY_UI_API_SIZE)
				return false;
			return a_api->registerClient &&
				a_api->registerCategory &&
				a_api->registerPage &&
				a_api->selectPage &&
				a_api->registerPageActivityObserver &&
				a_api->setStatus &&
				a_api->getThemeColors &&
				a_api->pushFont &&
				a_api->popFont &&
				a_api->drawSectionHeader &&
				a_api->drawSearchInput &&
				a_api->drawCollapsingSectionHeader &&
				a_api->drawSettingsActionButton &&
				a_api->settingsActionButtonWidth &&
				a_api->settingsActionButtonExtent &&
				a_api->beginSettingsTable &&
				a_api->beginSettingsRow &&
				a_api->endSettingsRow &&
				a_api->endSettingsTable &&
				a_api->drawLinkRow &&
				a_api->queryServices &&
				a_api->queryUIAPI &&
				a_api->openExternal;
		}

		[[nodiscard]] bool PreflightRequiredHostOperations() noexcept
		{
			using GetAPIFn =
				const DMUI_HostAPI* (DMUI_CALL*)(uint32_t) noexcept;
			const auto getAPI =
				dmui::detail::ResolveHostSymbol<GetAPIFn>("DMUI_GetAPI");
			if (!getAPI)
				return !dmui::detail::HostModulePresent();
			return HasRequiredHostOperations(getAPI(DMUI_HOST_ABI_CURRENT));
		}

		void ReportPresentationFailure() noexcept
		{
			if (s_client.LastResult() == DMUI_RESULT_OK)
				return;
			REX::ERROR(
				"Crash Logger UI: presentation failed, result {}."sv,
				DMUI_ResultToString(s_client.LastResult()));
		}

		void DrawUnavailable()
		{
			(void)dmui::DrawStyledText(
				s_client,
				"This page is unavailable because DearModdingUI registration "
				"did not complete. Crash logging remains independent.",
				{
					.tone = dmui::TextTone::kError,
					.wrapped = true
				});
		}

		[[nodiscard]] std::string InstallOutcomeText(
			InstallOutcome a_outcome)
		{
			switch (a_outcome)
			{
			case InstallOutcome::kDisabled:
				return "Disabled for this session";
			case InstallOutcome::kInstalled:
				return "Installed";
			case InstallOutcome::kFailed:
				return "Failed";
			default:
				return "Not attempted";
			}
		}

		[[nodiscard]] bool DrawOpenLink(
			const char* a_id,
			const char* a_label,
			const std::filesystem::path& a_path,
			DMUI_ExternalTargetKind a_kind,
			const char* a_note = nullptr)
		{
			const auto target = a_path.string();
			const std::array links{
				dmui::Link{
					.label = a_label,
					.external = {
						.targetKind = a_kind,
						.target = target.c_str()
					},
					.note = a_note,
					.enabled = !target.empty(),
					.action = dmui::LinkAction::kOpenExternal
				}
			};
			return s_client.DrawLinkRow(a_id, links);
		}

		void DrawHome()
		{
			if (!s_pagesOperational.load(std::memory_order_acquire))
			{
				DrawUnavailable();
				return;
			}
			const auto startup = GetStartupSnapshot();
			const auto reports = ReportRepository::GetSingleton().IndexSnapshot();
			if (!s_client.DrawSectionHeader("Session status"))
				return;
			(void)dmui::DrawLabeledValue(
				s_client,
				"Logger:",
				InstallOutcomeText(startup->installOutcome));
			(void)dmui::DrawLabeledValue(
				s_client,
				"Installation:",
				startup->installDetail);
			(void)dmui::DrawLabeledValue(
				s_client,
				"Game version:",
				startup->gameVersion);
			(void)dmui::DrawLabeledValue(
				s_client,
				"Logger version:",
				startup->loggerVersion);

			(void)s_client.DrawSectionHeader("Output");
			(void)dmui::DrawLabeledValue(
				s_client,
				"Resolved reports:",
				startup->resolvedReportDirectory.string());
			if (!startup->configuredReportDirectory.empty() &&
				startup->configuredReportDirectory != startup->resolvedReportDirectory)
			{
				(void)dmui::DrawLabeledValue(
					s_client,
					"Configured reports:",
					startup->configuredReportDirectory.string(),
					{ .valueStyle = {
						.tone = dmui::TextTone::kWarning,
						.wrapped = true
					} });
			}
			(void)dmui::DrawStyledText(
				s_client,
				startup->reportDirectoryDetail,
				{ .tone = dmui::TextTone::kMuted, .wrapped = true });
			(void)dmui::DrawLabeledValue(
				s_client,
				"Startup/operational log:",
				startup->startupLogPath.string());
			if (reports->loading)
			{
				(void)dmui::DrawLabeledValue(
					s_client, "Latest saved report:", "Indexing...");
			}
			else if (!reports->error.empty())
			{
				(void)dmui::DrawLabeledValue(
					s_client, "Latest saved report:", "Unavailable");
			}
			else if (reports->generation == 0)
			{
				(void)dmui::DrawLabeledValue(
				s_client,
				"Latest saved crash:",
				"Not indexed until Reports is opened");
			}
			else
			{
				const auto latest = std::ranges::find(
				reports->reports,
				ReportKind::kCrash,
				&ReportRecord::kind);
				if (latest == reports->reports.end())
				{
				(void)dmui::DrawLabeledValue(
					s_client, "Latest saved crash:", "No indexed crash report");
				}
				else
				{
				(void)dmui::DrawLabeledValue(
					s_client,
					reports->partial ?
						"Newest retained indexed crash:" :
						"Latest saved crash:",
					latest->basename);
				if (reports->partial)
					(void)dmui::DrawStyledText(
						s_client,
						"The index is partial; this is not claimed as the global latest crash.",
						{ .tone = dmui::TextTone::kWarning, .wrapped = true });
				}
			}

			(void)s_client.DrawSectionHeader("Open");
			if (!DrawOpenLink(
					"home-open-reports",
					"Open Reports Folder",
					startup->resolvedReportDirectory,
					DMUI_EXTERNAL_TARGET_DIRECTORY,
					"Opens the resolved reports directory through the host."))
				return;
			if (!DrawOpenLink(
					"home-open-startup-log",
					"Open Startup Log",
					startup->startupLogPath,
					DMUI_EXTERNAL_TARGET_FILE,
					"AddictolCrashLogger.log is an operational log, not a crash report."))
				return;
			if (dmui::ui::Button("Reports"))
				(void)s_client.SelectPage(s_reportsPage);
			dmui::ui::SameLine();
			if (dmui::ui::Button("Settings"))
				(void)s_client.SelectPage(s_settingsPage);
			ReportPresentationFailure();
		}

		[[nodiscard]] bool MatchesReportFilter(const ReportRecord& a_record)
		{
			if (s_reportKindFilter == 1 && a_record.kind != ReportKind::kCrash)
				return false;
			if (s_reportKindFilter == 2 && a_record.kind != ReportKind::kThreadDump)
				return false;
			return s_reportFilter.empty() ||
				dmui::ContainsFolded(a_record.basename, s_reportFilter);
		}

		void DrawReportList(
			const std::shared_ptr<const ReportIndexSnapshot>& a_index)
		{
			if (dmui::ui::Button("Refresh"))
			{
				ReportRepository::GetSingleton().RequestRefresh(a_index->directory);
				s_selectedReport.clear();
				++s_selectionGeneration;
			}
			dmui::ui::SameLine();
			if (dmui::ui::Button("All"))
				s_reportKindFilter = 0;
			dmui::ui::SameLine();
			if (dmui::ui::Button("Crash"))
				s_reportKindFilter = 1;
			dmui::ui::SameLine();
			if (dmui::ui::Button("Thread dump"))
				s_reportKindFilter = 2;
			(void)s_client.DrawSearchInput(
				"report-filter",
				"Filter report filenames...",
				s_reportFilter);
			if (a_index->loading)
				(void)dmui::DrawStyledText(s_client, "Indexing reports...");
			if (!a_index->error.empty())
				(void)dmui::DrawStyledText(
					s_client,
					a_index->error,
					{ .tone = dmui::TextTone::kError, .wrapped = true });
			if (a_index->partial)
				(void)dmui::DrawStyledText(
					s_client,
					"The index is partial: enumeration or retained-record limits were reached.",
					{ .tone = dmui::TextTone::kWarning, .wrapped = true });
			if (!a_index->loading && a_index->error.empty() &&
				a_index->reports.empty())
				(void)dmui::DrawStyledText(
					s_client,
					"No timestamp-named crash or thread-dump reports were found.");

			if (!dmui::ui::BeginTable(
					"reports",
					5,
					dmui::ui::TableFlags::kRowBg |
						dmui::ui::TableFlags::kBorders |
						dmui::ui::TableFlags::kSizingStretchProp))
				return;
			dmui::ui::TableSetupColumn("Report");
			dmui::ui::TableSetupColumn("Kind");
			dmui::ui::TableSetupColumn("Timestamp");
			dmui::ui::TableSetupColumn("Size");
			dmui::ui::TableSetupColumn("Dump");
			dmui::ui::TableHeadersRow();
			for (const auto& report : a_index->reports)
			{
				if (!MatchesReportFilter(report))
					continue;
				dmui::ui::TableNextRow();
				(void)dmui::ui::TableSetColumnIndex(0);
				const auto selected = report.path == s_selectedReport;
				if (dmui::ui::Selectable(
						report.basename.c_str(),
						selected,
						dmui::ui::SelectableFlags::kSpanAllColumns))
				{
					++s_selectionGeneration;
					s_previewPage = 0;
					s_previewSearch.clear();
					if (selected)
						s_selectedReport.clear();
					else
					{
						s_selectedReport = report.path;
						ReportRepository::GetSingleton().RequestRead(
							report,
							a_index->generation,
							s_selectionGeneration);
					}
				}
				(void)dmui::ui::TableSetColumnIndex(1);
				dmui::ui::TextUnformatted(
					report.kind == ReportKind::kCrash ? "Crash" : "Thread dump");
				(void)dmui::ui::TableSetColumnIndex(2);
				dmui::ui::TextUnformatted(report.recordedTimestamp);
				(void)dmui::ui::TableSetColumnIndex(3);
				dmui::ui::TextUnformatted(std::format("{} bytes", report.size));
				(void)dmui::ui::TableSetColumnIndex(4);
				dmui::ui::TextUnformatted(report.hasMiniDump ? "Paired" : "No");
			}
			dmui::ui::EndTable();
		}

		[[nodiscard]] std::string_view LineAt(
			const ReportReadSnapshot& a_read,
			size_t a_line)
		{
			if (a_line >= a_read.lineOffsets.size())
				return {};
			const auto start = a_read.lineOffsets[a_line];
			auto end = a_line + 1 < a_read.lineOffsets.size() ?
				a_read.lineOffsets[a_line + 1] :
				a_read.text.size();
			while (end > start &&
				(a_read.text[end - 1] == '\n' || a_read.text[end - 1] == '\r'))
				--end;
			return std::string_view{ a_read.text }.substr(start, end - start);
		}

		void SelectSearchHit(
			const ReportReadSnapshot& a_read,
			const ReportSearchSnapshot& a_search,
			bool a_forward)
		{
			if (a_search.byteOffsets.empty())
			{
				s_currentSearchHit = std::string::npos;
				s_searchStatus = "No matches in the loaded preview.";
				return;
			}
			const auto pageFirstLine = s_previewPage * kPreviewLinesPerPage;
			const auto pageLastLine = (std::min)(
				pageFirstLine + kPreviewLinesPerPage,
				a_read.lineOffsets.size());
			const auto pageStart =
				pageFirstLine < a_read.lineOffsets.size() ?
					a_read.lineOffsets[pageFirstLine] : a_read.text.size();
			const auto pageEnd =
				pageLastLine < a_read.lineOffsets.size() ?
					a_read.lineOffsets[pageLastLine] : a_read.text.size();
			bool wrapped{};
			size_t index{};
			if (s_currentSearchHit == std::string::npos)
			{
				if (a_forward)
				{
					const auto match = std::lower_bound(
						a_search.byteOffsets.begin(),
						a_search.byteOffsets.end(),
						pageStart);
					if (match != a_search.byteOffsets.end() && *match < pageEnd)
						index = static_cast<size_t>(
							match - a_search.byteOffsets.begin());
					else if (match != a_search.byteOffsets.end())
						index = static_cast<size_t>(
							match - a_search.byteOffsets.begin());
					else
						wrapped = true;
				}
				else
				{
					const auto match = std::lower_bound(
						a_search.byteOffsets.begin(),
						a_search.byteOffsets.end(),
						pageEnd);
					if (match != a_search.byteOffsets.begin() &&
						*(match - 1) >= pageStart)
						index = static_cast<size_t>(
							(match - 1) - a_search.byteOffsets.begin());
					else
					{
						const auto before = std::lower_bound(
							a_search.byteOffsets.begin(),
							a_search.byteOffsets.end(),
							pageStart);
						if (before != a_search.byteOffsets.begin())
							index = static_cast<size_t>(
								(before - 1) - a_search.byteOffsets.begin());
						else
						{
							index = a_search.byteOffsets.size() - 1;
							wrapped = true;
						}
					}
				}
			}
			else if (a_forward)
			{
				const auto match = std::upper_bound(
					a_search.byteOffsets.begin(),
					a_search.byteOffsets.end(),
					s_currentSearchHit);
				if (match == a_search.byteOffsets.end())
					wrapped = true;
				else
					index = static_cast<size_t>(
						match - a_search.byteOffsets.begin());
			}
			else
			{
				const auto match = std::lower_bound(
					a_search.byteOffsets.begin(),
					a_search.byteOffsets.end(),
					s_currentSearchHit);
				if (match == a_search.byteOffsets.begin())
				{
					index = a_search.byteOffsets.size() - 1;
					wrapped = true;
				}
				else
					index = static_cast<size_t>(
						(match - 1) - a_search.byteOffsets.begin());
			}
			if (wrapped && a_forward)
				index = 0;
			s_currentSearchHit = a_search.byteOffsets[index];
			const auto line = static_cast<size_t>(std::upper_bound(
				a_read.lineOffsets.begin(),
				a_read.lineOffsets.end(),
				s_currentSearchHit) - a_read.lineOffsets.begin() - 1);
			s_previewPage = line / kPreviewLinesPerPage;
			s_searchStatus = std::format(
				"Match {} of {}{}{}.",
				index + 1,
				a_search.byteOffsets.size(),
				a_search.capped ? "+" : "",
				wrapped ? "; wrapped" : "");
		}

		void DrawDetailSearch(
			const std::shared_ptr<const ReportReadSnapshot>& a_read)
		{
			if (s_searchSelectionGeneration != a_read->selectionGeneration)
			{
				s_searchSelectionGeneration = a_read->selectionGeneration;
				s_currentSearchHit = std::string::npos;
				s_searchStatus.clear();
				++s_searchGeneration;
				if (!s_previewSearch.empty())
					ReportRepository::GetSingleton().RequestSearch(
						a_read,
						s_previewSearch,
						s_searchGeneration);
			}
			if (s_client.DrawSearchInput(
					"preview-search",
					"Search loaded text (literal, case-sensitive)...",
					s_previewSearch).value_or(false))
			{
				if (s_previewSearch.size() > kMaximumSearchQueryBytes)
				{
					s_previewSearch.resize(kMaximumSearchQueryBytes);
					auto sequenceStart = s_previewSearch.size() - 1;
					while (sequenceStart > 0 &&
						(static_cast<unsigned char>(
							s_previewSearch[sequenceStart]) & 0xC0u) == 0x80u)
						--sequenceStart;
					const auto lead = static_cast<unsigned char>(
						s_previewSearch[sequenceStart]);
					const auto sequenceLength =
						lead < 0x80u ? size_t{ 1 } :
						lead < 0xE0u ? size_t{ 2 } :
						lead < 0xF0u ? size_t{ 3 } : size_t{ 4 };
					if (sequenceStart + sequenceLength >
						s_previewSearch.size())
						s_previewSearch.resize(sequenceStart);
					s_searchStatus =
						"Query was limited to 512 UTF-8 bytes.";
				}
				else
					s_searchStatus.clear();
				s_currentSearchHit = std::string::npos;
				++s_searchGeneration;
				ReportRepository::GetSingleton().RequestSearch(
					a_read,
					s_previewSearch,
					s_searchGeneration);
			}
			if (s_previewSearch.empty())
				return;
			const auto search =
				ReportRepository::GetSingleton().SearchSnapshot();
			const auto current =
				search->directoryGeneration == a_read->directoryGeneration &&
				search->selectionGeneration == a_read->selectionGeneration &&
				search->searchGeneration == s_searchGeneration &&
				search->path == a_read->path &&
				search->query == s_previewSearch;
			if (!current || search->loading)
			{
				(void)dmui::DrawStyledText(
					s_client,
					"Searching the loaded preview...");
				return;
			}
			if (!search->error.empty())
			{
				(void)dmui::DrawStyledText(
					s_client,
					search->error,
					{ .tone = dmui::TextTone::kError, .wrapped = true });
				return;
			}
			if (dmui::ui::Button("Previous match"))
				SelectSearchHit(*a_read, *search, false);
			dmui::ui::SameLine();
			if (dmui::ui::Button("Next match"))
				SelectSearchHit(*a_read, *search, true);
			if (search->byteOffsets.empty())
				(void)dmui::DrawStyledText(
					s_client,
					"No matches in the loaded preview.",
					{ .tone = dmui::TextTone::kMuted });
			else
			{
				const auto count = std::format(
					"{} match{}{} retained.",
					search->byteOffsets.size(),
					search->byteOffsets.size() == 1 ? "" : "es",
					search->capped ? " (10,000+ total)" : "");
				(void)dmui::DrawStyledText(
					s_client,
					count,
					{ .tone = dmui::TextTone::kMuted });
			}
			if (!s_searchStatus.empty())
				(void)dmui::DrawStyledText(
					s_client,
					s_searchStatus,
					{ .tone = dmui::TextTone::kMuted });
		}

		void DrawReportDetails(
			const std::shared_ptr<const ReportIndexSnapshot>& a_index,
			const std::shared_ptr<const ReportReadSnapshot>& a_read)
		{
			if (s_selectedReport.empty())
				return;
			(void)s_client.DrawSectionHeader("Selected report");
			if (a_read->loading &&
				a_read->selectionGeneration == s_selectionGeneration)
			{
				(void)dmui::DrawStyledText(s_client, "Loading report text...");
				return;
			}
			if (a_read->selectionGeneration != s_selectionGeneration ||
				a_read->directoryGeneration != a_index->generation ||
				a_read->path != s_selectedReport)
				return;
			if (!a_read->error.empty())
			{
				(void)dmui::DrawStyledText(
					s_client,
					a_read->error,
					{ .tone = dmui::TextTone::kError, .wrapped = true });
				return;
			}
			if (a_read->truncated)
				(void)dmui::DrawStyledText(
					s_client,
					"Preview and Copy Loaded Text contain only the first 2 MiB. "
					"Open Report to inspect the original file.",
					{ .tone = dmui::TextTone::kWarning, .wrapped = true });
			if (!a_read->encodingNotice.empty())
				(void)dmui::DrawStyledText(
					s_client,
					a_read->encodingNotice,
					{ .tone = dmui::TextTone::kMuted, .wrapped = true });

			if (!a_read->metadata.sections.empty())
			{
				const auto style = s_client.GetStyleMetrics();
				if (!style)
					ReportPresentationFailure();
				else
				{
					const auto available = dmui::ui::GetContentRegionAvail().x;
					float usedWidth{};
					for (const auto& [name, line] : a_read->metadata.sections)
					{
						if (available <= 0.0f)
							break;
						const auto naturalWidth =
							dmui::ui::CalcTextSize(name).x + style->framePadding.x * 2.0f;
						const auto width = (std::min)(naturalWidth, available);
						if (usedWidth > 0.0f &&
							usedWidth + style->itemSpacing.x + width <= available)
						{
							dmui::ui::SameLine();
							usedWidth += style->itemSpacing.x;
						}
						else
							usedWidth = 0.0f;
						const auto label = name + "##section-" + std::to_string(line);
						if (dmui::ui::Button(label.c_str(), { width, 0.0f }))
						{
							s_previewPage = line / kPreviewLinesPerPage;
							s_currentSearchHit = std::string::npos;
						}
						if (naturalWidth > width && dmui::ui::IsItemHovered())
							dmui::ui::SetTooltip("%s", name.c_str());
						usedWidth += width;
					}
				}
			}
			DrawDetailSearch(a_read);
			const auto pageCount = (std::max)(
				size_t{ 1 },
				(a_read->lineOffsets.size() + kPreviewLinesPerPage - 1) /
					kPreviewLinesPerPage);
			s_previewPage = (std::min)(s_previewPage, pageCount - 1);
			if (dmui::ui::Button("Previous 200"))
			{
				s_previewPage = s_previewPage == 0 ? 0 : s_previewPage - 1;
				s_currentSearchHit = std::string::npos;
			}
			dmui::ui::SameLine();
			if (dmui::ui::Button("Next 200"))
			{
				s_previewPage = (std::min)(s_previewPage + 1, pageCount - 1);
				s_currentSearchHit = std::string::npos;
			}
			dmui::ui::SameLine();
			dmui::ui::TextUnformatted(std::format(
				"Page {} of {}",
				s_previewPage + 1,
				pageCount));

			const auto first = s_previewPage * kPreviewLinesPerPage;
			const auto last = (std::min)(
				first + kPreviewLinesPerPage,
				a_read->lineOffsets.size());
			for (size_t line = first; line < last; ++line)
				dmui::ui::TextUnformatted(LineAt(*a_read, line));

			(void)s_client.DrawSectionHeader("Explicit local actions");
			(void)dmui::DrawStyledText(
				s_client,
				"Raw report text can contain paths, load-order details, memory "
				"strings, and other sensitive data. Nothing is copied automatically.",
				{ .tone = dmui::TextTone::kWarning, .wrapped = true });
			const auto copyLabel = a_read->truncated ?
				"Copy Loaded Text (truncated; potentially sensitive)" :
				"Copy Loaded Text (potentially sensitive)";
			if (dmui::ui::Button(copyLabel))
				dmui::ui::SetClipboardText(a_read->text);
			(void)dmui::DrawStyledText(
				s_client,
				"Exact summary preview:",
				{ .fontRole = DMUI_FONT_ROLE_SUBHEADING });
			dmui::ui::TextUnformatted(a_read->summary);
			if (!a_read->metadata.summarySources.empty())
			{
				(void)dmui::DrawStyledText(
					s_client,
					"Summary evidence:",
					{ .fontRole = DMUI_FONT_ROLE_SUBHEADING });
				for (const auto& [label, source] :
					a_read->metadata.summarySources)
				{
					dmui::ui::TextUnformatted(label);
					dmui::ui::SameLine();
					const auto button = std::format(
						"Line {}##summary-{}-{}",
						source.line + 1,
						label,
						source.byteOffset);
					if (dmui::ui::Button(button.c_str()))
					{
						s_previewPage = source.line / kPreviewLinesPerPage;
						s_currentSearchHit = std::string::npos;
					}
				}
			}
			if (dmui::ui::Button("Copy Selected Summary"))
				dmui::ui::SetClipboardText(a_read->summary);
			(void)DrawOpenLink(
				"report-open-file",
				"Open Report",
				a_read->path,
				DMUI_EXTERNAL_TARGET_FILE);
			(void)DrawOpenLink(
				"report-open-folder",
				"Open Reports Folder",
				a_index->directory,
				DMUI_EXTERNAL_TARGET_DIRECTORY);
		}

		void DrawReports()
		{
			if (!s_pagesOperational.load(std::memory_order_acquire))
			{
				DrawUnavailable();
				return;
			}
			const auto index = ReportRepository::GetSingleton().IndexSnapshot();
			const auto read = ReportRepository::GetSingleton().ReadSnapshot();
			DrawReportList(index);
			DrawReportDetails(index, read);
			ReportPresentationFailure();
		}

		[[nodiscard]] const ReportRecord* FindReport(
			const ReportIndexSnapshot& a_index,
			const std::filesystem::path& a_path)
		{
			const auto found = std::ranges::find(
				a_index.reports,
				a_path,
				&ReportRecord::path);
			return found == a_index.reports.end() ? nullptr :
				std::addressof(*found);
		}

		[[nodiscard]] bool DrawReportSelector(
			const char* a_label,
			const char* a_id,
			const ReportIndexSnapshot& a_index,
			std::filesystem::path& a_selection)
		{
			const auto preview = a_selection.empty() ?
				std::string{ "Choose report" } :
				a_selection.filename().string();
			auto changed = false;
			const auto comboLabel = std::string{ a_label } + "##" + a_id;
			if (dmui::ui::BeginCombo(comboLabel.c_str(), preview.c_str()))
			{
				for (const auto& report : a_index.reports)
				{
					const auto selected = report.path == a_selection;
					const auto item = report.basename + "##" + a_id;
					if (dmui::ui::Selectable(item.c_str(), selected))
					{
						a_selection = report.path;
						changed = true;
					}
				}
				dmui::ui::EndCombo();
			}
			return changed;
		}

		void DrawComparisonContents(
			const ReportComparisonSnapshot& a_comparison)
		{
			if (a_comparison.loading)
			{
				(void)dmui::DrawStyledText(
					s_client,
					"Reading and comparing the selected reports...");
				return;
			}
			if (!a_comparison.error.empty())
				(void)dmui::DrawStyledText(
					s_client,
					a_comparison.error,
					{ .tone = dmui::TextTone::kError, .wrapped = true });
			if (a_comparison.stale)
				(void)dmui::DrawStyledText(
					s_client,
					"This comparison is stale because the report index or a selected "
					"file changed. Select Compare again for fresh recorded facts.",
					{ .tone = dmui::TextTone::kWarning, .wrapped = true });
			if (a_comparison.validationPending)
				(void)dmui::DrawStyledText(
					s_client,
					"Revalidating both named files and captured file identities...");
			if (!a_comparison.leftParsed || !a_comparison.rightParsed)
				return;

			(void)s_client.DrawSectionHeader("Recorded evidence status");
			dmui::ui::TextUnformatted(std::format(
				"A: {} (modules {}, F4SE plugins {}, plugins {})",
				a_comparison.left.basename,
				EvidenceStateName(a_comparison.leftParsed->modulesSection.state),
				EvidenceStateName(a_comparison.leftParsed->f4sePluginsSection.state),
				EvidenceStateName(a_comparison.leftParsed->pluginsSection.state)));
			dmui::ui::TextUnformatted(std::format(
				"B: {} (modules {}, F4SE plugins {}, plugins {})",
				a_comparison.right.basename,
				EvidenceStateName(a_comparison.rightParsed->modulesSection.state),
				EvidenceStateName(a_comparison.rightParsed->f4sePluginsSection.state),
				EvidenceStateName(a_comparison.rightParsed->pluginsSection.state)));

			(void)s_client.DrawSectionHeader("Differences");
			if (a_comparison.differencesTruncated)
				(void)dmui::DrawStyledText(
					s_client,
					"Comparison is incomplete: differences were omitted at the retention limit.",
					{ .tone = dmui::TextTone::kWarning, .wrapped = true });
			if (a_comparison.differences.empty() && !a_comparison.differencesTruncated)
				(void)dmui::DrawStyledText(
					s_client,
					"No differences were found among complete supported fields. "
					"This does not prove identical binaries or a shared cause.",
					{ .tone = dmui::TextTone::kMuted, .wrapped = true });
			else
			{
				std::string previousCategory;
				for (const auto& difference : a_comparison.differences)
				{
					if (difference.category != previousCategory)
					{
						previousCategory = difference.category;
						(void)dmui::DrawStyledText(
							s_client,
							previousCategory,
							{ .fontRole = DMUI_FONT_ROLE_SUBHEADING });
					}
					dmui::ui::TextUnformatted(difference.message);
					if (difference.leftSource || difference.rightSource)
					{
						std::string source = "Source:";
						if (difference.leftSource)
							source += std::format(
								" A line {}",
								difference.leftSource->line + 1);
						if (difference.rightSource)
							source += std::format(
								" B line {}",
								difference.rightSource->line + 1);
						(void)dmui::DrawStyledText(
							s_client,
							source,
							{ .tone = dmui::TextTone::kMuted });
					}
				}
			}

			(void)s_client.DrawSectionHeader("Explicit local actions");
			(void)dmui::DrawStyledText(
				s_client,
				"Comparison text contains report filenames and recorded load-order "
				"facts. Nothing is copied automatically.",
				{ .tone = dmui::TextTone::kWarning, .wrapped = true });
			auto queuedCopy = false;
			if (dmui::ui::Button("Copy comparison after revalidation"))
			{
				queuedCopy = true;
				s_pendingComparisonCopy = a_comparison.comparisonGeneration;
				s_comparisonActionStatus =
					"Copy requested; waiting for file identity revalidation.";
				ReportRepository::GetSingleton().RequestComparisonValidation(
					a_comparison.comparisonGeneration);
			}
			if (!queuedCopy &&
				s_pendingComparisonCopy ==
					a_comparison.comparisonGeneration &&
				!a_comparison.validationPending)
			{
				if (a_comparison.validated && !a_comparison.stale)
				{
					dmui::ui::SetClipboardText(a_comparison.summary);
					s_comparisonActionStatus =
						"Validated comparison copied.";
				}
				else
					s_comparisonActionStatus =
						"Copy cancelled because a selected report changed.";
				s_pendingComparisonCopy.reset();
			}
			if (!s_comparisonActionStatus.empty())
				(void)dmui::DrawStyledText(
					s_client,
					s_comparisonActionStatus,
					{ .tone = dmui::TextTone::kMuted, .wrapped = true });
		}

		void DrawCompare()
		{
			try
			{
				if (!s_pagesOperational.load(std::memory_order_acquire))
				{
					DrawUnavailable();
					return;
				}
				auto& repository = ReportRepository::GetSingleton();
				const auto index = repository.IndexSnapshot();
				(void)s_client.DrawSectionHeader("Read-only report comparison");
				(void)dmui::DrawStyledText(
					s_client,
					"Compares only facts recorded in two saved Fallout 4 reports. "
					"Missing or partial evidence remains unknown; differences are not "
					"claims about cause or binary equality.",
					{ .tone = dmui::TextTone::kMuted, .wrapped = true });
				if (dmui::ui::Button("Refresh report index"))
				{
					repository.RequestRefresh(index->directory);
					repository.MarkComparisonStale();
					s_compareValidatedIndexGeneration = 0;
					s_pendingComparisonCopy.reset();
					s_comparisonActionStatus.clear();
				}
				if (index->loading)
					(void)dmui::DrawStyledText(s_client, "Indexing reports...");
				if (!index->error.empty())
				{
					(void)dmui::DrawStyledText(
						s_client,
						index->error,
						{ .tone = dmui::TextTone::kError, .wrapped = true });
					return;
				}
				if (index->partial)
					(void)dmui::DrawStyledText(
						s_client,
						"The shared report index is partial.",
						{ .tone = dmui::TextTone::kWarning });
				const auto leftChanged = DrawReportSelector(
					"Report A",
					"compare-left",
					*index,
					s_compareLeft);
				const auto rightChanged = DrawReportSelector(
					"Report B",
					"compare-right",
					*index,
					s_compareRight);
				if (leftChanged || rightChanged)
				{
					++s_comparisonGeneration;
					s_compareValidatedIndexGeneration = 0;
					s_pendingComparisonCopy.reset();
					s_comparisonActionStatus.clear();
					repository.MarkComparisonStale(s_comparisonGeneration);
				}
				const auto left = FindReport(*index, s_compareLeft);
				const auto right = FindReport(*index, s_compareRight);
				if ((!s_compareLeft.empty() && !left) ||
					(!s_compareRight.empty() && !right))
					(void)dmui::DrawStyledText(
						s_client,
						"A selected report is no longer in the current index.",
						{ .tone = dmui::TextTone::kWarning, .wrapped = true });
				dmui::ui::BeginDisabled(
					!left || !right || left->path == right->path ||
					index->loading);
				if (dmui::ui::Button("Compare selected reports"))
				{
					++s_comparisonGeneration;
					s_compareValidatedIndexGeneration = index->generation;
					s_pendingComparisonCopy.reset();
					s_comparisonActionStatus.clear();
					repository.RequestComparison(
						*left,
						*right,
						index->generation,
						s_comparisonGeneration);
				}
				dmui::ui::EndDisabled();
				if (left && right && left->path == right->path)
					(void)dmui::DrawStyledText(
						s_client,
						"Choose two different reports.",
						{ .tone = dmui::TextTone::kMuted });

				const auto comparison = repository.ComparisonSnapshot();
				const auto selectionMatches =
					comparison->comparisonGeneration == s_comparisonGeneration &&
					comparison->left.path == s_compareLeft &&
					comparison->right.path == s_compareRight;
				if (selectionMatches && comparison->comparisonGeneration != 0 &&
					index->generation != s_compareValidatedIndexGeneration &&
					!comparison->loading && !comparison->validationPending)
				{
					s_compareValidatedIndexGeneration = index->generation;
					repository.RequestComparisonValidation(
						comparison->comparisonGeneration);
				}
				if (selectionMatches)
					DrawComparisonContents(*comparison);
				ReportPresentationFailure();
			}
			catch (const std::exception& exception)
			{
				(void)dmui::DrawStyledText(
					s_client,
					std::string{ "Compare page failed: " } + exception.what(),
					{ .tone = dmui::TextTone::kError, .wrapped = true });
			}
			catch (...)
			{
				(void)dmui::DrawStyledText(
					s_client,
					"Compare page failed with an unknown error.",
					{ .tone = dmui::TextTone::kError, .wrapped = true });
			}
		}

		[[nodiscard]] dmui::TextTone DiagnosticTextTone(
			DiagnosticTone a_tone) noexcept
		{
			switch (a_tone)
			{
			case DiagnosticTone::kGood:
				return dmui::TextTone::kSuccess;
			case DiagnosticTone::kWarning:
				return dmui::TextTone::kWarning;
			case DiagnosticTone::kError:
				return dmui::TextTone::kError;
			default:
				return dmui::TextTone::kInherit;
			}
		}

		void DrawDiagnostics()
		{
			try
			{
				if (!s_pagesOperational.load(std::memory_order_acquire))
				{
					DrawUnavailable();
					return;
				}
				(void)s_client.DrawSectionHeader(
					"Current installation / local probe");
				(void)dmui::DrawStyledText(
					s_client,
					"This page probes the current executable and active startup "
					"configuration only. It does not diagnose a historical report. "
					"Settings edits remain next-launch values until restart.",
					{ .tone = dmui::TextTone::kMuted, .wrapped = true });
				const auto startup = GetStartupSnapshot();
				(void)dmui::DrawLabeledValue(
					s_client,
					"Active logger setting:",
					startup->activeSettings.enableCrashLogger ?
						"Enabled" : "Disabled");
				(void)dmui::DrawLabeledValue(
					s_client,
					"Active symbol cache:",
					startup->activeSettings.symcacheDirectory.empty() ?
						"(empty)" : startup->activeSettings.symcacheDirectory);
				auto& diagnostics = SymbolDiagnostics::GetSingleton();
				const auto snapshot = diagnostics.Snapshot();
				if (dmui::ui::Button(
						snapshot->phase == ProbePhase::kIdle ?
							"Run local probe" : "Refresh local probe"))
				{
					diagnostics.RequestProbe({
						startup->gameVersion,
						startup->loggerVersion,
						startup->activeSettings.symcacheDirectory,
						startup->installOutcome == InstallOutcome::kInstalled
					});
				}
				if (snapshot->workerBusy || snapshot->pending ||
					snapshot->phase == ProbePhase::kQueued ||
					snapshot->phase == ProbePhase::kRunning)
				{
					dmui::ui::SameLine();
					if (dmui::ui::Button("Cancel probe request"))
						diagnostics.CancelProbe();
				}
				switch (snapshot->phase)
				{
				case ProbePhase::kQueued:
					(void)dmui::DrawStyledText(
						s_client,
						snapshot->workerBusy ?
							"Probe queued; the single worker is still finishing an "
							"earlier synchronous operation." :
							"Probe queued.");
					break;
				case ProbePhase::kRunning:
					(void)dmui::DrawStyledText(
						s_client,
						"Local probe running. Cancel abandons this UI request but "
						"cannot interrupt a synchronous COM or filesystem call.",
						{ .tone = dmui::TextTone::kMuted, .wrapped = true });
					break;
				case ProbePhase::kCancelled:
					(void)dmui::DrawStyledText(
						s_client,
						snapshot->error.empty() ?
							"Probe request cancelled." : snapshot->error,
						{ .tone = dmui::TextTone::kWarning, .wrapped = true });
					break;
				default:
					break;
				}
				if (!snapshot->observedAt.empty())
					(void)dmui::DrawLabeledValue(
						s_client,
						"Observed:",
						snapshot->observedAt);
				for (const auto& field : snapshot->fields)
					(void)dmui::DrawLabeledValue(
						s_client,
						field.label + ":",
						field.value,
						{ .valueStyle = {
							.tone = DiagnosticTextTone(field.tone),
							.wrapped = true
						} });
				if (!snapshot->error.empty() &&
					snapshot->phase != ProbePhase::kCancelled)
					(void)dmui::DrawStyledText(
						s_client,
						snapshot->error,
						{ .tone = dmui::TextTone::kError, .wrapped = true });
				if (!snapshot->compactSummary.empty())
				{
					(void)s_client.DrawSectionHeader("Compact status");
					dmui::ui::TextUnformatted(snapshot->compactSummary);
					if (dmui::ui::Button("Copy compact status"))
						dmui::ui::SetClipboardText(snapshot->compactSummary);
				}
				ReportPresentationFailure();
			}
			catch (const std::exception& exception)
			{
				(void)dmui::DrawStyledText(
					s_client,
					std::string{ "Diagnostics page failed: " } + exception.what(),
					{ .tone = dmui::TextTone::kError, .wrapped = true });
			}
			catch (...)
			{
				(void)dmui::DrawStyledText(
					s_client,
					"Diagnostics page failed with an unknown error.",
					{ .tone = dmui::TextTone::kError, .wrapped = true });
			}
		}

		template <class T>
		dmui::SettingBinding BindSettingValue(
			SettingKey a_key,
			T Settings::Values::* a_member)
		{
			return dmui::BindSetting(
				[a_member]() -> T {
					return s_settingsState.Draft().*a_member;
				},
				[a_key, a_member](T a_value) -> T {
					s_settingsState.Draft().*a_member = std::move(a_value);
					s_settingsState.Edit(a_key);
					return s_settingsState.Draft().*a_member;
				});
		}

		dmui::SettingBinding BindSettingValue(
			SettingKey a_key,
			int32_t Settings::Values::* a_member)
		{
			return dmui::BindSetting(
				[a_member]() -> int64_t {
					return s_settingsState.Draft().*a_member;
				},
				[a_key, a_member](int64_t a_value) -> int64_t {
					const auto clamped = static_cast<int32_t>(std::clamp(
						a_value,
						static_cast<int64_t>((std::numeric_limits<int32_t>::min)()),
						static_cast<int64_t>((std::numeric_limits<int32_t>::max)())));
					s_settingsState.Draft().*a_member = clamped;
					s_settingsState.Edit(a_key);
					return clamped;
				});
		}

		template <class T>
		[[nodiscard]] dmui::SettingValue MakeDefaultValue(T a_value)
		{
			if constexpr (std::is_same_v<T, int32_t>)
				return static_cast<int64_t>(a_value);
			else
				return std::move(a_value);
		}

		template <class T>
		dmui::SettingDescriptor MakeSetting(
			const char* a_id,
			const char* a_label,
			const char* a_description,
			SettingKey a_key,
			T Settings::Values::* a_member,
			T a_default,
			dmui::SettingControl a_control)
		{
			return {
				.id = a_id,
				.label = a_label,
				.description = a_description,
				.control = std::move(a_control),
				.defaultValue = MakeDefaultValue(std::move(a_default)),
				.binding = BindSettingValue(a_key, a_member),
				.applyTiming = dmui::SettingApplyTiming::kNextLaunch,
				.isEnabled = [] { return !s_settingsState.SavePending(); },
				.isDirty = [a_key] {
					return s_settingsState.Dirty().test(
						static_cast<size_t>(a_key));
				},
				.isModified = [a_member, a_default] {
					return s_settingsState.Draft().*a_member != a_default;
				}
			};
		}

		[[nodiscard]] std::vector<dmui::SettingGroup> BuildSettingsGroups()
		{
			using Checkbox = dmui::CheckboxSettingControl;
			using Signed = dmui::SignedSettingControl;
			using Text = dmui::TextSettingControl;
			const auto signedRange = [](int64_t a_minimum, int64_t a_maximum) {
				return Signed{
					.range = dmui::NumericSettingRange<int64_t>{
						a_minimum, a_maximum
					},
					.format = "%lld",
					.dragSpeed = 1.0f
				};
			};
			std::vector<dmui::SettingGroup> groups;
			dmui::SettingGroup general{
				.id = "general",
				.label = "General",
				.glyph = DearModdingUI::PhosphorGlyph::kGear
			};
			general.settings.push_back(MakeSetting(
				"General.bEnableCrashLogger",
				"Master enablement",
				"Enables crash-handler installation on the next launch. "
				"The UI remains available while disabled.",
				SettingKey::kEnableCrashLogger,
				&Settings::Values::enableCrashLogger,
				Settings::kBuiltInDefaults.enableCrashLogger,
				Checkbox{}));
			general.settings.push_back(MakeSetting(
				"General.bPrintSettings",
				"Include Addictol settings",
				"Includes Addictol settings in newly written reports.",
				SettingKey::kPrintSettings,
				&Settings::Values::printSettings,
				Settings::kBuiltInDefaults.printSettings,
				Checkbox{}));
			general.settings.push_back(MakeSetting(
				"General.bAutoOpenLogs",
				"Auto-open new reports",
				"Preserves the existing automatic open behavior for newly "
				"created crash and thread-dump reports.",
				SettingKey::kAutoOpenLogs,
				&Settings::Values::autoOpenLogs,
				Settings::kBuiltInDefaults.autoOpenLogs,
				Checkbox{}));
			general.settings.push_back({
				.id = "Pastebin.bAutoUploadCrashLog",
				.label = "Pastebin upload (file-configured)",
				.description =
					"The upload toggle and API key remain file-only. Reports can "
					"contain sensitive local data; the API key is never shown.",
				.control = dmui::ReadOnlySettingControl{
					.draw = [enabled =
						GetStartupSnapshot()->activeSettings.autoUploadCrashLog] {
						dmui::ui::TextUnformatted(enabled ? "Enabled" : "Disabled");
					}
				},
				.defaultValue = false,
				.applyTiming = dmui::SettingApplyTiming::kNextLaunch,
				.showReset = false
			});
			groups.push_back(std::move(general));

			dmui::SettingGroup output{
				.id = "output",
				.label = "Output",
				.glyph = DearModdingUI::PhosphorGlyph::kFiles
			};
			output.settings.push_back(MakeSetting(
				"Directories.sCrashLogDirectory",
				"Report directory",
				"Empty uses the F4SE log directory. A new nonempty value must "
				"name an existing directory. The configured value is retained "
				"when startup falls back.",
				SettingKey::kCrashLogDirectory,
				&Settings::Values::crashLogDirectory,
				Settings::kBuiltInDefaults.crashLogDirectory,
				Text{ 1024, false }));
			output.settings.push_back(MakeSetting(
				"General.iMaxCrashLogs",
				"Crash/thread report retention",
				"Applied separately to crash- and threaddump- families during "
				"report creation. Zero means unlimited. Log cleanup can remove "
				"its paired dump.",
				SettingKey::kMaxCrashLogs,
				&Settings::Values::maxCrashLogs,
				Settings::kBuiltInDefaults.maxCrashLogs,
				signedRange(0, (std::numeric_limits<int32_t>::max)())));
			output.settings.push_back(MakeSetting(
				"General.iMaxMiniDumps",
				"Minidump retention",
				"Maximum retained dumps per report family. Zero means unlimited.",
				SettingKey::kMaxMiniDumps,
				&Settings::Values::maxMiniDumps,
				Settings::kBuiltInDefaults.maxMiniDumps,
				signedRange(0, (std::numeric_limits<int32_t>::max)())));
			groups.push_back(std::move(output));

			dmui::SettingGroup capture{
				.id = "capture",
				.label = "Capture",
				.glyph = DearModdingUI::PhosphorGlyph::kClipboardText
			};
			capture.settings.push_back(MakeSetting(
				"Debugging.bCrashLogWriteMiniDump",
				"Crash minidump",
				"Writes a minidump alongside newly created crash reports.",
				SettingKey::kCrashLogWriteMiniDump,
				&Settings::Values::crashLogWriteMiniDump,
				Settings::kBuiltInDefaults.crashLogWriteMiniDump,
				Checkbox{}));
			capture.settings.push_back(MakeSetting(
				"Debugging.bThreadDumpWriteMiniDump",
				"Thread-dump minidump",
				"Writes a minidump alongside manually triggered thread dumps.",
				SettingKey::kThreadDumpWriteMiniDump,
				&Settings::Values::threadDumpWriteMiniDump,
				Settings::kBuiltInDefaults.threadDumpWriteMiniDump,
				Checkbox{}));
			capture.settings.push_back(MakeSetting(
				"Debugging.bFullMemoryMiniDump",
				"Full-memory minidump",
				"Full-memory dumps can be very large and can contain sensitive "
				"process memory.",
				SettingKey::kFullMemoryMiniDump,
				&Settings::Values::fullMemoryMiniDump,
				Settings::kBuiltInDefaults.fullMemoryMiniDump,
				Checkbox{}));
			capture.settings.push_back(MakeSetting(
				"Hotkeys.bEnableThreadDumpHotkey",
				"Ctrl+Shift+F12 thread dump",
				"Enables the existing fixed Ctrl+Shift+F12 chord on the next "
				"launch. The chord is not rebindable in this release.",
				SettingKey::kEnableThreadDumpHotkey,
				&Settings::Values::enableThreadDumpHotkey,
				Settings::kBuiltInDefaults.enableThreadDumpHotkey,
				Checkbox{}));
			groups.push_back(std::move(capture));

			dmui::SettingGroup advanced{
				.id = "advanced",
				.label = "Advanced",
				.glyph = DearModdingUI::PhosphorGlyph::kTerminalWindow
			};
			advanced.settings.push_back(MakeSetting(
				"Debugging.bWaitForDebugger",
				"Wait for debugger",
				"Intentionally waits during a crash until a debugger attaches, "
				"preventing ordinary crash completion.",
				SettingKey::kWaitForDebugger,
				&Settings::Values::waitForDebugger,
				Settings::kBuiltInDefaults.waitForDebugger,
				Checkbox{}));
			advanced.settings.push_back(MakeSetting(
				"Directories.sSymcacheDirectory",
				"Symbol-cache directory",
				"Directory used for cached symbols. A changed value must name "
				"an existing directory.",
				SettingKey::kSymcacheDirectory,
				&Settings::Values::symcacheDirectory,
				Settings::kBuiltInDefaults.symcacheDirectory,
				Text{ 1024, false }));
			advanced.settings.push_back(MakeSetting(
				"Debugging.bHeapAnalysis",
				"Heap analysis",
				"Enables bounded heap-allocation analysis in future reports.",
				SettingKey::kHeapAnalysis,
				&Settings::Values::heapAnalysis,
				Settings::kBuiltInDefaults.heapAnalysis,
				Checkbox{}));
			advanced.settings.push_back(MakeSetting(
				"Debugging.iMaxHeapsToCheck",
				"Maximum heaps to check",
				"Must be at least one.",
				SettingKey::kMaxHeapsToCheck,
				&Settings::Values::maxHeapsToCheck,
				Settings::kBuiltInDefaults.maxHeapsToCheck,
				signedRange(1, (std::numeric_limits<int32_t>::max)())));
			advanced.settings.push_back(MakeSetting(
				"Debugging.iMaxHeapIterationsPerHeap",
				"Maximum allocations per heap",
				"Must be at least one.",
				SettingKey::kMaxHeapIterationsPerHeap,
				&Settings::Values::maxHeapIterationsPerHeap,
				Settings::kBuiltInDefaults.maxHeapIterationsPerHeap,
				signedRange(1, (std::numeric_limits<int32_t>::max)())));
			groups.push_back(std::move(advanced));
			return groups;
		}

		void PrepareSettingsPage(dmui::SettingsPage& a_page)
		{
			const auto state =
				SettingsRepository::GetSingleton().Snapshot();
			const auto& snapshot = state.settings;
			const auto& saveResult = state.saveResult;
			s_settingsState.Reconcile(
				*snapshot,
				saveResult ? &*saveResult : nullptr);
			if (!s_settingsState.Active() && !snapshot->loading &&
				snapshot->configurationValid)
				s_settingsState.Activate(*snapshot);
			if (snapshot->generation != s_settingsViewGeneration)
			{
				a_page.groups = snapshot->configurationValid ?
					BuildSettingsGroups() :
					std::vector<dmui::SettingGroup>{};
				s_settingsViewGeneration = snapshot->generation;
			}
			a_page.notes.clear();
			a_page.notes.push_back({
				"All edits apply on the next game launch. Active crash-handler "
				"settings are never mutated at runtime.",
				false
			});
			a_page.notes.push_back({
				"Leaving Settings or closing the menu discards unapplied edits. "
				"An accepted save continues and is reconciled when the page reopens.",
				true
			});
			if (snapshot->loading)
				a_page.notes.push_back({ "Loading configured settings...", false });
			if (!snapshot->configurationValid)
				a_page.notes.push_back({
					"Apply is disabled: " + snapshot->error,
					false
				});
			if (!snapshot->saveMessage.empty())
				a_page.notes.push_back({
					snapshot->saveMessage,
					snapshot->lastSaveSucceeded
				});
			const auto startup = GetStartupSnapshot();
			if (!snapshot->loading && snapshot->configurationValid &&
				snapshot->savedValues != startup->activeSettings)
				a_page.notes.push_back({
					"Saved values differ from the active session and will take "
					"effect only after restart.",
					false
				});
			if (!snapshot->reportDirectoryDetail.empty())
				a_page.notes.push_back({
					"Effective report path preview: " +
						snapshot->effectiveReportDirectory.string() + ". " +
						snapshot->reportDirectoryDetail,
					true
				});
		}

		[[nodiscard]] dmui::SettingsPage BuildSettingsPage()
		{
			return {
				.actions = {
					.showReset = true,
					.reset = [] { s_settingsState.Reset(); },
					.revert = [] { s_settingsState.Revert(); },
					.apply = [] {
						std::string error;
						const auto operation = s_nextSaveOperation++;
						const auto request =
							s_settingsState.AcceptApply(operation, error);
						if (!request)
						{
							if (!error.empty())
							{
								(void)s_client.SetStatus(
									DMUI_STATUS_SEVERITY_ERROR,
									error.c_str());
								REX::WARN(
									"Crash Logger UI settings: {}"sv,
									error);
							}
							return;
						}
						if (!SettingsRepository::GetSingleton().RequestSave(*request))
						{
							s_settingsState.CancelApply(operation);
							REX::WARN(
								"Crash Logger UI settings: save queue is busy."sv);
							(void)s_client.SetStatus(
								DMUI_STATUS_SEVERITY_WARNING,
								"Settings save queue is busy.");
						}
					}
				},
				.actionTooltips = {
					.reset =
						"Reset exposed controls to immutable compiled defaults. "
						"Hidden Pastebin settings are untouched.",
					.revert =
						"Discard the draft and restore configured values.",
					.apply = [](size_t a_pending) {
						return "Save " + std::to_string(a_pending) +
							" owned change(s) to the logical custom TOML for next launch.";
					}
				},
				.filterOptions = {
					.showSearch = true,
					.showModifiedOnly = true,
					.searchHint = "Search Crash Logger settings..."
				},
				.prepareView = &PrepareSettingsPage
			};
		}

		void OnPageActivity(const dmui::PageActivity& a_activity)
		{
			if (a_activity.kind == dmui::PageActivityKind::kActivated ||
				a_activity.kind == dmui::PageActivityKind::kChanged)
			{
				if (a_activity.activePage == s_reportsPage && !s_reportsActivated)
				{
					s_reportsActivated = true;
					const auto startup = GetStartupSnapshot();
					ReportRepository::GetSingleton().RequestRefresh(
						startup->resolvedReportDirectory);
				}
				if (a_activity.activePage == s_settingsPage && !s_settingsActivated)
				{
					s_settingsActivated = true;
					auto& repository = SettingsRepository::GetSingleton();
					repository.SetDefaultReportDirectory(
						GetStartupSnapshot()->startupLogPath.parent_path());
					repository.RequestLoad();
				}
				if (a_activity.activePage == s_comparePage && !s_compareActivated)
				{
					s_compareActivated = true;
					try
					{
						auto& repository = ReportRepository::GetSingleton();
						const auto index = repository.IndexSnapshot();
						const auto startup = GetStartupSnapshot();
						const auto indexValid = index->generation != 0 &&
							index->directory == startup->resolvedReportDirectory;
						if (!indexValid)
						{
							repository.RequestRefresh(
								startup->resolvedReportDirectory);
							s_compareValidatedIndexGeneration = 0;
						}
						const auto comparison = repository.ComparisonSnapshot();
						if (indexValid &&
							comparison->comparisonGeneration != 0 &&
							!comparison->loading &&
							!comparison->validationPending)
						{
							s_compareValidatedIndexGeneration = index->generation;
							repository.RequestComparisonValidation(
								comparison->comparisonGeneration);
						}
					}
					catch (const std::exception& exception)
					{
						REX::WARN(
							"Crash Logger UI: Compare activation failed: {}"sv,
							exception.what());
						(void)s_client.SetStatus(
							DMUI_STATUS_SEVERITY_WARNING,
							"The optional Compare page could not refresh.");
					}
					catch (...)
					{
						REX::WARN(
							"Crash Logger UI: Compare activation failed."sv);
					}
				}
			}
			if (a_activity.kind == dmui::PageActivityKind::kDeactivated ||
				a_activity.previousPage == s_settingsPage)
			{
				s_settingsActivated = false;
				s_settingsState.Deactivate();
				s_settingsViewGeneration = 0;
			}
			if (a_activity.kind == dmui::PageActivityKind::kDeactivated ||
				a_activity.previousPage == s_comparePage)
				s_compareActivated = false;
		}

		[[nodiscard]] bool RegisterPages() noexcept
		{
			if (!s_client.AddCategory({
					.id = "general",
					.displayName = "General",
					.sortKey = 0
				}))
			{
				REX::ERROR(
					"Crash Logger UI: category registration failed, result {}."sv,
					DMUI_ResultToString(s_client.LastResult()));
				return false;
			}
			const auto home = s_client.AddPage(
				{
					.id = "home",
					.displayName = "Home",
					.categoryId = "general",
					.summary = "Active logger status and resolved output locations.",
					.sortKey = 0
				},
				&DrawHome);
			const auto reports = s_client.AddPage(
				{
					.id = "reports",
					.displayName = "Reports",
					.categoryId = "general",
					.summary = "Browse bounded saved crash and thread-dump text reports.",
					.sortKey = 100
				},
				&DrawReports);
			const auto settings = s_client.AddSettingsPage(
				{
					.id = "settings",
					.displayName = "Settings",
					.categoryId = "general",
					.summary = "Edit owned configuration values for the next launch.",
					.sortKey = 200
				},
				BuildSettingsPage());
			if (!home || !reports || !settings)
			{
				REX::ERROR(
					"Crash Logger UI: page registration failed, result {}."sv,
					DMUI_ResultToString(s_client.LastResult()));
				return false;
			}
			s_homePage = *home;
			s_reportsPage = *reports;
			s_settingsPage = *settings;
			if (!s_client.AddPageActivityObserver(&OnPageActivity))
			{
				REX::ERROR(
					"Crash Logger UI: page lifecycle registration failed, result {}."sv,
					DMUI_ResultToString(s_client.LastResult()));
				return false;
			}
			s_pagesOperational.store(true, std::memory_order_release);
			return true;
		}

		void RegisterOptionalPages() noexcept
		{
			try
			{
				const auto compare = s_client.AddPage(
					{
						.id = "compare",
						.displayName = "Compare",
						.categoryId = "general",
						.summary =
							"Compare recorded facts from two saved reports.",
						.sortKey = 300
					},
					&DrawCompare);
				if (compare)
					s_comparePage = *compare;
				else
				{
					REX::WARN(
						"Crash Logger UI: optional Compare page registration failed, result {}."sv,
						DMUI_ResultToString(s_client.LastResult()));
					(void)s_client.SetStatus(
						DMUI_STATUS_SEVERITY_WARNING,
						"The optional Compare page is unavailable.");
				}
			}
			catch (...)
			{
				REX::WARN(
					"Crash Logger UI: optional Compare page registration threw."sv);
			}
			try
			{
				const auto diagnostics = s_client.AddPage(
					{
						.id = "diagnostics",
						.displayName = "Diagnostics",
						.categoryId = "general",
						.summary =
							"Run an explicit current-installation local symbol probe.",
						.sortKey = 400
					},
					&DrawDiagnostics);
				if (diagnostics)
					s_diagnosticsPage = *diagnostics;
				else
				{
					REX::WARN(
						"Crash Logger UI: optional Diagnostics page registration failed, result {}."sv,
						DMUI_ResultToString(s_client.LastResult()));
					(void)s_client.SetStatus(
						DMUI_STATUS_SEVERITY_WARNING,
						"The optional Diagnostics page is unavailable.");
				}
			}
			catch (...)
			{
				REX::WARN(
					"Crash Logger UI: optional Diagnostics page registration threw."sv);
			}
		}
	}

	bool InstallMenu() noexcept
	{
		std::call_once(s_installOnce, [] {
			if (!dmui::detail::HostModulePresent())
			{
				REX::INFO(
					"Crash Logger UI: DearModdingUI.dll is not loaded; "
					"crash logging continues headless."sv);
				return;
			}
			if (!PreflightRequiredHostOperations())
			{
				s_installResult.store(false, std::memory_order_relaxed);
				REX::ERROR(
					"Crash Logger UI: host is missing required registration, "
					"navigation, settings, status, theme, link, or external-open operations."sv);
				return;
			}
			if (!s_client.Connect())
			{
				s_installResult.store(false, std::memory_order_relaxed);
				REX::ERROR(
					"Crash Logger UI: connection failed, result {}. "
					"Crash logging is unchanged."sv,
					DMUI_ResultToString(s_client.LastResult()));
				return;
			}
			if (!RegisterPages())
			{
				s_installResult.store(false, std::memory_order_relaxed);
				(void)s_client.SetStatus(
					DMUI_STATUS_SEVERITY_ERROR,
					"Crash Logger UI registration did not complete.");
				return;
			}
			RegisterOptionalPages();
			REX::INFO(
				"Crash Logger UI: connected to DearModdingUI API revision {}."sv,
				DMUI_UI_REVISION_1);
		});
		return s_installResult.load(std::memory_order_relaxed);
	}
}
