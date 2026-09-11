#include "ReportRepository.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <limits>
#include <map>
#include <sstream>
#include <unordered_map>

namespace CrashUI
{
	using namespace std::literals;

	namespace
	{
		constexpr size_t kMaximumRecordNameBytes = 1024;
		constexpr size_t kMaximumRecordValueBytes = 512;
		constexpr size_t kMaximumParsedRecords = 16384;

		struct Line
		{
			std::string_view text;
			size_t number{};
			size_t offset{};
		};

		struct SectionRange
		{
			std::string_view name;
			SourceAnchor source;
			size_t contentOffset{};
			size_t endOffset{};
			bool closed{};
			bool duplicate{};
		};

		[[nodiscard]] std::string_view Trim(std::string_view a_value) noexcept
		{
			while (!a_value.empty() &&
				(a_value.front() == ' ' || a_value.front() == '\t' ||
					a_value.front() == '\r'))
				a_value.remove_prefix(1);
			while (!a_value.empty() &&
				(a_value.back() == ' ' || a_value.back() == '\t' ||
					a_value.back() == '\r'))
				a_value.remove_suffix(1);
			return a_value;
		}

		[[nodiscard]] std::string FoldAscii(std::string_view a_value)
		{
			std::string result;
			result.reserve(a_value.size());
			for (const auto value : a_value)
				result.push_back(static_cast<char>(
					std::tolower(static_cast<unsigned char>(value))));
			return result;
		}

		[[nodiscard]] bool IsHex(char a_character) noexcept
		{
			return (a_character >= '0' && a_character <= '9') ||
				(a_character >= 'a' && a_character <= 'f') ||
				(a_character >= 'A' && a_character <= 'F');
		}

		[[nodiscard]] bool AllHex(std::string_view a_value) noexcept
		{
			return !a_value.empty() &&
				std::ranges::all_of(a_value, IsHex);
		}

		[[nodiscard]] bool IsKnownSection(std::string_view a_line) noexcept
		{
			static constexpr std::array sections{
				"EXCEPTION RECORD:"sv,
				"POSSIBLE RELEVANT OBJECTS:"sv,
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
				"SETTINGS:"sv,
				"SYSTEM SPECS:"sv,
				"MEMORY:"sv,
				"THREADS:"sv,
				"RAW CALL STACK:"sv
			};
			return std::ranges::find(sections, a_line) != sections.end();
		}

		[[nodiscard]] bool IsProducerError(std::string_view a_line)
		{
			const auto raw = FoldAscii(a_line);
			const auto folded = Trim(raw);
			static constexpr std::array stages{
				"print_modules"sv, "print_xse_plugins"sv, "print_plugins"sv,
				"print_registers"sv, "print_stack"sv, "print_raw_stack"sv,
				"print_probable_callstack"sv, "print_hybrid_callstack"sv, "print_settings"sv,
				"print_sysinfo"sv, "print_exception"sv, "print_process_info"sv,
				"print_thread_context"sv, "print_relevant_objects_section"sv,
				"print_relevant_objects"sv, "hybrid_callstack"sv
			};
			for (const auto stage : stages)
				if (folded.starts_with(stage))
				{
					const auto suffix = folded.substr(stage.size());
					if (suffix == " failed!"sv || suffix.starts_with(":\t"sv))
						return true;
				}
			return (raw.starts_with("skipping module "sv) && raw.find(':') != std::string::npos) ||
				folded == "===== crash logger internal error ====="sv ||
				folded == "failed to read tib"sv ||
				folded.starts_with("callstack analysis failed:"sv);
		}

		template <class F>
		void ForEachLine(std::string_view a_text, F&& a_function)
		{
			size_t offset{};
			size_t number{};
			while (offset <= a_text.size())
			{
				const auto end = a_text.find('\n', offset);
				auto text = a_text.substr(
					offset,
					end == std::string_view::npos ? a_text.size() - offset :
						end - offset);
				if (!text.empty() && text.back() == '\r')
					text.remove_suffix(1);
				if (!a_function(Line{ text, number, offset }))
					return;
				++number;
				if (end == std::string_view::npos)
					return;
				offset = end + 1;
			}
		}

		[[nodiscard]] std::vector<Line> LinesIn(
			std::string_view a_text,
			size_t a_begin,
			size_t a_end,
			bool& a_capped)
		{
			std::vector<Line> result;
			a_capped = false;
			size_t lineNumber{};
			for (size_t index = 0; index < a_begin && index < a_text.size(); ++index)
				lineNumber += a_text[index] == '\n';
			size_t offset = a_begin;
			while (offset < a_end)
			{
				if (result.size() >= kMaximumParsedRecords + 1)
				{
					a_capped = true;
					break;
				}
				const auto end = a_text.find('\n', offset);
				const auto boundedEnd =
					end == std::string_view::npos ? a_end : (std::min)(end, a_end);
				auto text = a_text.substr(offset, boundedEnd - offset);
				if (!text.empty() && text.back() == '\r')
					text.remove_suffix(1);
				result.push_back({ text, lineNumber, offset });
				++lineNumber;
				if (end == std::string_view::npos || end >= a_end)
					break;
				offset = end + 1;
			}
			return result;
		}

		void SetField(
			EvidenceField& a_field,
			std::string a_value,
			SourceAnchor a_source)
		{
			if (a_value.empty())
				return;
			if (a_field.state == EvidenceState::kComplete)
			{
				a_field.state = EvidenceState::kAmbiguous;
				if (a_field.value != a_value)
					a_field.value.clear();
				return;
			}
			if (a_field.state == EvidenceState::kAmbiguous)
				return;
			a_field.state = EvidenceState::kComplete;
			a_field.value = std::move(a_value);
			a_field.source = std::move(a_source);
		}

		[[nodiscard]] std::string VersionAfter(
			std::string_view a_line,
			std::string_view a_prefix)
		{
			if (!a_line.starts_with(a_prefix))
				return {};
			auto value = a_line.substr(a_prefix.size());
			size_t length{};
			while (length < value.size() &&
				((value[length] >= '0' && value[length] <= '9') ||
					value[length] == '.'))
				++length;
			if (length == 0 || length > kMaximumRecordValueBytes)
				return {};
			return std::string{ value.substr(0, length) };
		}

		[[nodiscard]] std::string ExplicitExceptionCode(std::string_view a_line)
		{
			a_line = Trim(a_line);
			std::string_view prefix;
			if (a_line.starts_with("Exception Code:"sv))
				prefix = "Exception Code:"sv;
			else if (a_line.starts_with("EXCEPTION_CODE:"sv))
				prefix = "EXCEPTION_CODE:"sv;
			else
				return {};
			auto value = Trim(a_line.substr(prefix.size()));
			if (value.size() < 10 || value.substr(0, 2) != "0x" ||
				!AllHex(value.substr(2, 8)))
				return {};
			return std::string{ value.substr(0, 10) };
		}

		void ParsePrimaryExceptionLine(
			const Line& a_line,
			ParsedReportView& a_result)
		{
			if (!a_line.text.starts_with("Unhandled exception"sv))
				return;
			const SourceAnchor source{ "Primary exception", a_line.number, a_line.offset };
			constexpr auto quotedPrefix = "Unhandled exception \""sv;
			if (a_line.text.starts_with(quotedPrefix))
			{
				const auto end = a_line.text.find('"', quotedPrefix.size());
				if (end != std::string_view::npos &&
					a_line.text.substr(end).starts_with("\" at 0x"sv))
				{
					const auto name =
						a_line.text.substr(quotedPrefix.size(), end - quotedPrefix.size());
					if (!name.empty() && name.size() <= kMaximumRecordValueBytes)
						SetField(a_result.exceptionName, std::string{ name }, source);
				}
			}

			const auto address = a_line.text.find(" at 0x");
			if (address == std::string_view::npos)
				return;
			auto moduleStart = a_line.text.find(' ', address + 6);
			if (moduleStart == std::string_view::npos)
				return;
			while (moduleStart < a_line.text.size() && a_line.text[moduleStart] == ' ')
				++moduleStart;
			const auto plus = a_line.text.find('+', moduleStart);
			if (plus == std::string_view::npos || plus == moduleStart)
				return;
			const auto module = a_line.text.substr(moduleStart, plus - moduleStart);
			if (module.size() > kMaximumRecordNameBytes ||
				module.find_first_of("\\/:") != std::string_view::npos)
				return;
			auto offsetStart = plus + 1;
			if (a_line.text.substr(offsetStart, 2) == "0x")
				offsetStart += 2;
			auto offsetEnd = offsetStart;
			while (offsetEnd < a_line.text.size() && IsHex(a_line.text[offsetEnd]))
				++offsetEnd;
			if (offsetEnd == offsetStart || offsetEnd - offsetStart > 16)
				return;
			SetField(
				a_result.faultLocation,
				std::string{ module } + "+0x" +
					std::string{ a_line.text.substr(offsetStart, offsetEnd - offsetStart) },
				source);
		}

		[[nodiscard]] bool Retain(
			ParsedReportView& a_result,
			size_t a_bytes,
			SectionEvidence& a_section)
		{
			if (a_result.retainedBytes > a_result.retentionLimit ||
				a_bytes > a_result.retentionLimit - a_result.retainedBytes)
			{
				a_result.retentionCapped = true;
				if (a_section.state != EvidenceState::kAmbiguous)
					a_section.state = EvidenceState::kPartial;
				return false;
			}
			a_result.retainedBytes += a_bytes;
			return true;
		}

		[[nodiscard]] bool ParseModuleLine(
			const Line& a_line,
			ModuleRecord& a_record)
		{
			const auto line = Trim(a_line.text);
			const auto marker = line.rfind(" 0x");
			if (marker == std::string_view::npos)
				return false;
			const auto name = Trim(line.substr(0, marker));
			const auto base = line.substr(marker + 1);
			if (name.empty() || name.size() > kMaximumRecordNameBytes ||
				base.size() < 3 || base.size() > 18 ||
				!base.starts_with("0x") || !AllHex(base.substr(2)))
				return false;
			a_record = {
				std::string{ name },
				std::string{ base },
				{ "MODULES:", a_line.number, a_line.offset }
			};
			return true;
		}

		[[nodiscard]] bool ParseF4SEPluginLine(
			const Line& a_line,
			F4SEPluginRecord& a_record)
		{
			auto line = Trim(a_line.text);
			if (line.empty() || line.size() > kMaximumParsedLineBytes)
				return false;
			std::optional<std::string> version;
			auto name = line;
			const auto marker = line.rfind(" v");
			if (marker != std::string_view::npos)
			{
				const auto candidate = Trim(line.substr(marker + 2));
				if (!candidate.empty() &&
					candidate.front() >= '0' && candidate.front() <= '9' &&
					candidate.size() <= kMaximumRecordValueBytes)
				{
					name = Trim(line.substr(0, marker));
					version = std::string{ candidate };
				}
			}
			if (name.empty() || name.size() > kMaximumRecordNameBytes)
				return false;
			a_record = {
				std::string{ name },
				std::move(version),
				{ "F4SE PLUGINS:", a_line.number, a_line.offset }
			};
			return true;
		}

		[[nodiscard]] bool ParseCount(
			std::string_view a_text,
			size_t& a_value,
			size_t& a_consumed)
		{
			const auto begin = a_text.data();
			const auto end = begin + a_text.size();
			const auto parsed = std::from_chars(begin, end, a_value);
			if (parsed.ec != std::errc{} || parsed.ptr == begin)
				return false;
			a_consumed = static_cast<size_t>(parsed.ptr - begin);
			return true;
		}

		[[nodiscard]] bool ParsePluginCounts(
			std::string_view a_line,
			size_t& a_light,
			size_t& a_regular,
			size_t& a_total)
		{
			a_line = Trim(a_line);
			if (!a_line.starts_with("Light: "))
				return false;
			size_t consumed{};
			if (!ParseCount(a_line.substr(7), a_light, consumed))
				return false;
			a_line.remove_prefix(7 + consumed);
			a_line = Trim(a_line);
			if (!a_line.starts_with("Regular: "))
				return false;
			if (!ParseCount(a_line.substr(9), a_regular, consumed))
				return false;
			a_line.remove_prefix(9 + consumed);
			a_line = Trim(a_line);
			if (!a_line.starts_with("Total: "))
				return false;
			if (!ParseCount(a_line.substr(7), a_total, consumed))
				return false;
			a_line.remove_prefix(7 + consumed);
			return Trim(a_line).empty();
		}

		[[nodiscard]] bool ParsePluginLine(
			const Line& a_line,
			PluginRecord& a_record)
		{
			auto line = Trim(a_line.text);
			if (line.size() < 5 || line.front() != '[')
				return false;
			if (line.starts_with("[FE:"))
			{
				const auto close = line.find(']');
				if (close != 7 || !AllHex(line.substr(4, 3)))
					return false;
				const auto name = Trim(line.substr(close + 1));
				if (name.empty() || name.size() > kMaximumRecordNameBytes)
					return false;
				a_record = {
					std::string{ name },
					PluginKind::kLight,
					std::string{ line.substr(1, close - 1) },
					{ "PLUGINS:", a_line.number, a_line.offset }
				};
				return true;
			}
			const auto close = line.find(']');
			if (close != 3 || !AllHex(line.substr(1, 2)))
				return false;
			const auto name = Trim(line.substr(close + 1));
			if (name.empty() || name.size() > kMaximumRecordNameBytes)
				return false;
			a_record = {
				std::string{ name },
				PluginKind::kRegular,
				std::string{ line.substr(1, 2) },
				{ "PLUGINS:", a_line.number, a_line.offset }
			};
			return true;
		}

		template <class T>
		[[nodiscard]] bool HasFoldedDuplicates(
			const std::vector<T>& a_records)
		{
			std::unordered_map<std::string, std::string> names;
			for (const auto& record : a_records)
			{
				const auto folded = FoldAscii(record.name);
				if (const auto found = names.find(folded); found != names.end())
					return true;
				names.emplace(folded, record.name);
			}
			return false;
		}

		[[nodiscard]] EvidenceState FinalSectionState(
			bool a_closed,
			bool a_fullyRead,
			bool a_problem,
			bool a_duplicate)
		{
			if (a_duplicate)
				return EvidenceState::kAmbiguous;
			if (a_problem || (!a_closed && !a_fullyRead))
				return EvidenceState::kPartial;
			return EvidenceState::kComplete;
		}

		void AddStatusDifference(
			std::vector<ReportDifference>& a_result,
			std::string_view a_label,
			const SectionEvidence& a_left,
			const SectionEvidence& a_right)
		{
			if (a_left.state == EvidenceState::kComplete &&
				a_right.state == EvidenceState::kComplete)
				return;
			if (a_left.state == a_right.state)
			{
				a_result.push_back({
					std::string{ a_label } + " status",
					std::string{ a_label } + " comparison unavailable: both reports are " +
						EvidenceStateName(a_left.state) + ".",
					a_left.source,
					a_right.source
				});
			}
			else
			{
				a_result.push_back({
					std::string{ a_label } + " status",
					std::string{ a_label } + " comparison limited: A is " +
						EvidenceStateName(a_left.state) + ", B is " +
						EvidenceStateName(a_right.state) + ".",
					a_left.source,
					a_right.source
				});
			}
		}

		void CompareField(
			std::vector<ReportDifference>& a_result,
			std::string_view a_label,
			const EvidenceField& a_left,
			const EvidenceField& a_right)
		{
			if (a_left.state == EvidenceState::kComplete &&
				a_right.state == EvidenceState::kComplete)
			{
				if (a_left.value != a_right.value)
					a_result.push_back({
						std::string{ a_label },
						std::string{ a_label } + " differs: A reports \"" +
							a_left.value + "\"; B reports \"" + a_right.value + "\".",
						a_left.source,
						a_right.source
					});
				return;
			}
			if (a_left.state == EvidenceState::kAbsent &&
				a_right.state == EvidenceState::kAbsent)
				return;
			a_result.push_back({
				std::string{ a_label } + " status",
				std::string{ a_label } + " comparison unavailable: A is " +
					EvidenceStateName(a_left.state) + ", B is " +
					EvidenceStateName(a_right.state) + ".",
				a_left.source,
				a_right.source
			});
		}
	}

	const char* EvidenceStateName(EvidenceState a_state) noexcept
	{
		switch (a_state)
		{
		case EvidenceState::kComplete:
			return "complete";
		case EvidenceState::kPartial:
			return "partial";
		case EvidenceState::kAbsent:
			return "absent";
		case EvidenceState::kAmbiguous:
			return "ambiguous";
		default:
			return "unsupported";
		}
	}

	DecodedReportText DecodeReportText(
		std::span<const std::byte> a_bytes,
		size_t a_decodedLimit)
	{
		DecodedReportText result;
		const auto byte = [&](size_t a_index) {
			return std::to_integer<unsigned char>(a_bytes[a_index]);
		};
		bool littleEndian{};
		bool bigEndian{};
		size_t start{};
		if (a_bytes.size() >= 2 && byte(0) == 0xFFu && byte(1) == 0xFEu)
		{
			littleEndian = true;
			start = 2;
			result.encoding = ReportEncoding::kUtf16LE;
			result.normalized = true;
			result.notice = "UTF-16LE text was normalized to UTF-8 for display.";
		}
		else if (a_bytes.size() >= 2 && byte(0) == 0xFEu && byte(1) == 0xFFu)
		{
			bigEndian = true;
			start = 2;
			result.encoding = ReportEncoding::kUtf16BE;
			result.normalized = true;
			result.notice = "UTF-16BE text was normalized to UTF-8 for display.";
		}
		if (littleEndian || bigEndian)
		{
			if ((a_bytes.size() - start) % 2 != 0)
			{
				result.encoding = ReportEncoding::kUnsupported;
				result.notice = "The UTF-16 input ends with an incomplete code unit.";
				return result;
			}
			const auto unitCount = (a_bytes.size() - start) / 2;
			if (unitCount > static_cast<size_t>((std::numeric_limits<int>::max)()))
			{
				result.notice = "The decoded report exceeds the supported text limit.";
				return result;
			}
			std::wstring units;
			units.reserve(unitCount);
			for (size_t offset = start; offset < a_bytes.size(); offset += 2)
			{
				const auto first = byte(offset);
				const auto second = byte(offset + 1);
				units.push_back(static_cast<wchar_t>(
					littleEndian ? first | (second << 8u) :
						(first << 8u) | second));
			}
			if (std::ranges::find(units, wchar_t{}) != units.end())
			{
				result.encoding = ReportEncoding::kUnsupported;
				result.notice =
					"The UTF-16 report contains an embedded null code unit.";
				return result;
			}
			const auto needed = WideCharToMultiByte(
				CP_UTF8,
				WC_ERR_INVALID_CHARS,
				units.data(),
				static_cast<int>(units.size()),
				nullptr,
				0,
				nullptr,
				nullptr);
			if (needed < 0 || static_cast<size_t>(needed) > a_decodedLimit)
			{
				result.encoding = ReportEncoding::kUnsupported;
				result.notice = "The decoded report exceeds the supported text limit.";
				return result;
			}
			if (needed == 0 && !units.empty())
			{
				result.encoding = ReportEncoding::kUnsupported;
				result.notice = "The UTF-16 report contains invalid text.";
				return result;
			}
			result.text.resize(static_cast<size_t>(needed));
			if (needed != 0 && WideCharToMultiByte(
					CP_UTF8,
					WC_ERR_INVALID_CHARS,
					units.data(),
					static_cast<int>(units.size()),
					result.text.data(),
					needed,
					nullptr,
					nullptr) != needed)
			{
				result.text.clear();
				result.encoding = ReportEncoding::kUnsupported;
				result.notice = "The UTF-16 report contains invalid text.";
				return result;
			}
			result.lossless = true;
			return result;
		}

		if (a_bytes.size() >= 3 &&
			byte(0) == 0xEFu && byte(1) == 0xBBu && byte(2) == 0xBFu)
		{
			start = 3;
			result.encoding = ReportEncoding::kUtf8Bom;
			result.normalized = true;
			result.notice = "The UTF-8 byte-order mark was removed for display.";
		}
		else
			result.encoding = ReportEncoding::kUtf8;
		if (a_bytes.size() - start > a_decodedLimit)
		{
			result.encoding = ReportEncoding::kUnsupported;
			result.notice = "The decoded report exceeds the supported text limit.";
			return result;
		}
		const auto bytes = a_bytes.subspan(start);
		if (std::ranges::find(bytes, std::byte{}) != bytes.end())
		{
			result.encoding = ReportEncoding::kUnsupported;
			result.notice =
				"The UTF-8 report contains an embedded null byte.";
			return result;
		}
		size_t offset{};
		while (offset < bytes.size())
		{
			const auto first = std::to_integer<unsigned char>(bytes[offset]);
			size_t length{};
			if (first <= 0x7Fu)
				length = 1;
			else if (first >= 0xC2u && first <= 0xDFu)
				length = 2;
			else if (first >= 0xE0u && first <= 0xEFu)
				length = 3;
			else if (first >= 0xF0u && first <= 0xF4u)
				length = 4;
			if (length == 0 || offset + length > bytes.size())
				break;
			auto valid = true;
			for (size_t index = 1; index < length; ++index)
				valid = valid &&
					(std::to_integer<unsigned char>(bytes[offset + index]) & 0xC0u) ==
						0x80u;
			if (length == 3)
			{
				const auto second =
					std::to_integer<unsigned char>(bytes[offset + 1]);
				valid = valid &&
					!(first == 0xE0u && second < 0xA0u) &&
					!(first == 0xEDu && second >= 0xA0u);
			}
			if (length == 4)
			{
				const auto second =
					std::to_integer<unsigned char>(bytes[offset + 1]);
				valid = valid &&
					!(first == 0xF0u && second < 0x90u) &&
					!(first == 0xF4u && second >= 0x90u);
			}
			if (!valid)
				break;
			offset += length;
		}
		if (offset != bytes.size())
		{
			result.encoding = ReportEncoding::kUnsupported;
			result.notice =
				"Invalid text bytes were replaced for raw preview; structured fields are unsupported.";
			result.lossless = false;
			return result;
		}
		result.text.assign(
			reinterpret_cast<const char*>(bytes.data()),
			bytes.size());
		result.lossless = true;
		return result;
	}

	ParsedReportView ParseReportView(
		std::string_view a_text,
		ReportEncoding a_encoding,
		bool a_fullyRead,
		size_t a_retainedLimit)
	{
		ParsedReportView result;
		result.retentionLimit = (std::min)(a_retainedLimit, kMaximumParsedReportBytes);
		result.encoding = a_encoding;
		result.fullyRead = a_fullyRead;
		if (a_encoding == ReportEncoding::kUnsupported)
		{
			result.gameVersion.state = EvidenceState::kUnsupported;
			result.loggerVersion.state = EvidenceState::kUnsupported;
			result.exceptionCode.state = EvidenceState::kUnsupported;
			result.exceptionName.state = EvidenceState::kUnsupported;
			result.faultLocation.state = EvidenceState::kUnsupported;
			result.modulesSection.state = EvidenceState::kUnsupported;
			result.f4sePluginsSection.state = EvidenceState::kUnsupported;
			result.pluginsSection.state = EvidenceState::kUnsupported;
			result.notes.emplace_back(
				"Structured parsing is unavailable because decoding was not lossless.");
			return result;
		}

		std::vector<SectionRange> ranges;
		bool beforeLaterSections = true;
		bool internalErrorSeen{};
		bool sectionRangesCapped{};
		ForEachLine(a_text, [&](const Line& line) {
			if (line.text.size() > kMaximumParsedLineBytes)
			{
				result.notes.emplace_back(
					"A line exceeded the 8 KiB parser limit; affected evidence is partial.");
				return true;
			}
			if (line.text.starts_with("===== CRASH LOGGER INTERNAL ERROR ====="sv))
			{
				internalErrorSeen = true;
				beforeLaterSections = false;
			}
			if (line.text.starts_with("Nested Exception (depth "sv))
				beforeLaterSections = false;
			if (IsKnownSection(line.text))
			{
				if (line.text != "EXCEPTION RECORD:"sv)
					beforeLaterSections = false;
				if (!ranges.empty())
				{
					ranges.back().endOffset = line.offset;
					ranges.back().closed = true;
				}
				if (ranges.size() >= 256)
				{
					sectionRangesCapped = true;
					return true;
				}
				const auto duplicate = std::ranges::any_of(
					ranges,
					[&](const SectionRange& range) {
						return range.name == line.text;
					});
				ranges.push_back({
					line.text,
					{ std::string{ line.text }, line.number, line.offset },
					line.offset + line.text.size(),
					a_text.size(),
					false,
					duplicate
				});
			}
			const SourceAnchor headerSource{ "Header", line.number, line.offset };
			if (const auto value = VersionAfter(line.text, "Fallout 4 v"sv);
				!value.empty())
				SetField(result.gameVersion, value, headerSource);
			if (const auto value =
					VersionAfter(line.text, "Addictol Crash Logger v"sv);
				!value.empty())
			{
				result.supportedGrammar = true;
				SetField(result.loggerVersion, value, headerSource);
			}
			if (beforeLaterSections && !internalErrorSeen)
			{
				ParsePrimaryExceptionLine(line, result);
				if (const auto code = ExplicitExceptionCode(line.text); !code.empty())
					SetField(
						result.exceptionCode,
						code,
						{ "Primary exception", line.number, line.offset });
			}
			return true;
		});
		if (!ranges.empty())
			ranges.back().endOffset = a_text.size();

		if (!result.supportedGrammar)
		{
			result.gameVersion.state = EvidenceState::kUnsupported;
			result.loggerVersion.state = EvidenceState::kUnsupported;
			result.exceptionCode.state = EvidenceState::kUnsupported;
			result.exceptionName.state = EvidenceState::kUnsupported;
			result.faultLocation.state = EvidenceState::kUnsupported;
			result.modulesSection.state = EvidenceState::kUnsupported;
			result.f4sePluginsSection.state = EvidenceState::kUnsupported;
			result.pluginsSection.state = EvidenceState::kUnsupported;
			result.notes.emplace_back(
				"The logger grammar is not recognized; raw preview remains available.");
			return result;
		}

		const auto finishMissingField = [&](EvidenceField& field) {
			if (field.state == EvidenceState::kAbsent && !a_fullyRead)
				field.state = EvidenceState::kPartial;
		};
		finishMissingField(result.gameVersion);
		finishMissingField(result.loggerVersion);
		finishMissingField(result.exceptionCode);
		finishMissingField(result.exceptionName);
		finishMissingField(result.faultLocation);

		for (const auto& range : ranges)
		{
			auto problem = false;
			bool linesCapped{};
			for (const auto& line : LinesIn(
				a_text, range.contentOffset, range.endOffset, linesCapped))
				if (line.text.size() > kMaximumParsedLineBytes ||
					IsProducerError(line.text))
					problem = true;
			problem = problem || linesCapped;
			SectionEvidence section{
				std::string{ range.name },
				FinalSectionState(
					range.closed,
					a_fullyRead,
					problem,
					range.duplicate),
				range.source
			};
			result.sections.push_back(section);
			if (range.name == "MODULES:"sv)
				result.modulesSection = section;
			else if (range.name == "F4SE PLUGINS:"sv)
				result.f4sePluginsSection = section;
			else if (range.name == "PLUGINS:"sv)
				result.pluginsSection = section;
		}
		for (auto& section : result.sections)
		{
			const auto duplicates = static_cast<size_t>(std::ranges::count(
				result.sections,
				section.name,
				&SectionEvidence::name));
			if (duplicates > 1)
				section.state = EvidenceState::kAmbiguous;
		}

		const auto parseRange = [&](std::string_view name, auto&& parser) {
			for (const auto& range : ranges)
				if (range.name == name)
					parser(range);
		};

		parseRange("MODULES:"sv, [&](const SectionRange& range) {
			auto problem = range.duplicate;
			bool linesCapped{};
			for (const auto& line : LinesIn(
				a_text, range.contentOffset, range.endOffset, linesCapped))
			{
				if (Trim(line.text).empty())
					continue;
				if (line.text.size() > kMaximumParsedLineBytes ||
					IsProducerError(line.text))
				{
					problem = true;
					continue;
				}
				ModuleRecord record;
				if (!ParseModuleLine(line, record))
				{
					problem = true;
					continue;
				}
				if (result.modules.size() >= kMaximumParsedRecords ||
					!Retain(
						result,
						sizeof(ModuleRecord) + record.name.size() +
							record.aslrBase.size() + record.source.section.size(),
						result.modulesSection))
				{
					problem = true;
					break;
				}
				result.modules.push_back(std::move(record));
			}
			problem = problem || linesCapped;
			if (HasFoldedDuplicates(result.modules))
			{
				result.modulesSection.state = EvidenceState::kAmbiguous;
				problem = true;
			}
			if (result.modulesSection.state != EvidenceState::kAmbiguous)
				result.modulesSection.state = FinalSectionState(
					range.closed,
					a_fullyRead,
					problem,
					range.duplicate);
		});

		parseRange("F4SE PLUGINS:"sv, [&](const SectionRange& range) {
			auto problem = range.duplicate;
			bool linesCapped{};
			for (const auto& line : LinesIn(
				a_text, range.contentOffset, range.endOffset, linesCapped))
			{
				if (Trim(line.text).empty())
					continue;
				if (line.text.size() > kMaximumParsedLineBytes ||
					IsProducerError(line.text))
				{
					problem = true;
					continue;
				}
				F4SEPluginRecord record;
				if (!ParseF4SEPluginLine(line, record))
				{
					problem = true;
					continue;
				}
				const auto versionSize =
					record.reportedVersion ? record.reportedVersion->size() : 0;
				if (result.f4sePlugins.size() >= kMaximumParsedRecords ||
					!Retain(
						result,
						sizeof(F4SEPluginRecord) + record.name.size() +
							versionSize + record.source.section.size(),
						result.f4sePluginsSection))
				{
					problem = true;
					break;
				}
				result.f4sePlugins.push_back(std::move(record));
			}
			problem = problem || linesCapped;
			if (HasFoldedDuplicates(result.f4sePlugins))
			{
				result.f4sePluginsSection.state = EvidenceState::kAmbiguous;
				problem = true;
			}
			if (result.f4sePluginsSection.state != EvidenceState::kAmbiguous)
				result.f4sePluginsSection.state = FinalSectionState(
					range.closed,
					a_fullyRead,
					problem,
					range.duplicate);
		});

		parseRange("PLUGINS:"sv, [&](const SectionRange& range) {
			auto problem = range.duplicate;
			auto sawCounts = false;
			bool linesCapped{};
			for (const auto& line : LinesIn(
				a_text, range.contentOffset, range.endOffset, linesCapped))
			{
				if (Trim(line.text).empty())
					continue;
				if (line.text.size() > kMaximumParsedLineBytes ||
					IsProducerError(line.text))
				{
					problem = true;
					continue;
				}
				size_t light{};
				size_t regular{};
				size_t total{};
				if (ParsePluginCounts(line.text, light, regular, total))
				{
					if (sawCounts)
						problem = true;
					sawCounts = true;
					result.reportedLightCount = light;
					result.reportedRegularCount = regular;
					result.reportedTotalCount = total;
					continue;
				}
				PluginRecord record;
				if (!ParsePluginLine(line, record))
				{
					problem = true;
					continue;
				}
				if (result.plugins.size() >= kMaximumParsedRecords ||
					!Retain(
						result,
						sizeof(PluginRecord) + record.name.size() +
							record.reportedIndex.size() +
							record.source.section.size(),
						result.pluginsSection))
				{
					problem = true;
					break;
				}
				result.plugins.push_back(std::move(record));
			}
			problem = problem || linesCapped;
			if (!sawCounts)
				problem = true;
			else
			{
				const auto light = static_cast<size_t>(std::ranges::count(
					result.plugins,
					PluginKind::kLight,
					&PluginRecord::kind));
				const auto regular = result.plugins.size() - light;
				if (*result.reportedLightCount != light ||
					*result.reportedRegularCount != regular ||
					*result.reportedTotalCount != result.plugins.size() ||
					*result.reportedTotalCount !=
						*result.reportedLightCount + *result.reportedRegularCount)
					problem = true;
			}
			if (HasFoldedDuplicates(result.plugins))
			{
				result.pluginsSection.state = EvidenceState::kAmbiguous;
				problem = true;
			}
			if (result.pluginsSection.state != EvidenceState::kAmbiguous)
				result.pluginsSection.state = FinalSectionState(
					range.closed,
					a_fullyRead,
					problem,
					range.duplicate);
		});
		for (auto& section : result.sections)
		{
			if (section.name == result.modulesSection.name)
				section.state = result.modulesSection.state;
			else if (section.name == result.f4sePluginsSection.name)
				section.state = result.f4sePluginsSection.state;
			else if (section.name == result.pluginsSection.name)
				section.state = result.pluginsSection.state;
		}

		if (!a_fullyRead)
			for (auto* section : {
				&result.modulesSection,
				&result.f4sePluginsSection,
				&result.pluginsSection })
				if (section->state == EvidenceState::kAbsent)
					section->state = EvidenceState::kPartial;
		if (sectionRangesCapped)
		{
			result.retentionCapped = true;
			for (auto* section : {
				&result.modulesSection,
				&result.f4sePluginsSection,
				&result.pluginsSection })
				if (section->state == EvidenceState::kAbsent)
					section->state = EvidenceState::kPartial;
			result.notes.emplace_back(
				"Section headings exceeded the parser cap; later sections are partial.");
		}
		if (result.retentionCapped)
			result.notes.emplace_back(
				"Parsed evidence reached the 2 MiB retention limit; affected sections are partial.");
		if (internalErrorSeen)
			result.notes.emplace_back(
				"The report contains a crash-logger internal error marker.");
		return result;
	}

	ReportMetadata AdaptReportMetadata(const ParsedReportView& a_view)
	{
		ReportMetadata result;
		result.gameVersion = a_view.gameVersion.value;
		result.loggerVersion = a_view.loggerVersion.value;
		result.exceptionCode = a_view.exceptionCode.value;
		result.exceptionName = a_view.exceptionName.value;
		result.faultLocation = a_view.faultLocation.value;
		if (!a_view.supportedGrammar)
			result.status = EvidenceState::kUnsupported;
		else if (!a_view.fullyRead || a_view.retentionCapped)
			result.status = EvidenceState::kPartial;
		else if (std::ranges::any_of(
				std::array{
					&a_view.gameVersion,
					&a_view.loggerVersion,
					&a_view.exceptionCode,
					&a_view.exceptionName,
					&a_view.faultLocation },
				[](const EvidenceField* field) {
					return field->state == EvidenceState::kAmbiguous;
				}))
			result.status = EvidenceState::kAmbiguous;
		else
			result.status = EvidenceState::kComplete;
		for (const auto& section : a_view.sections)
			if (section.source)
				result.sections.emplace_back(section.name, section.source->line);
		for (const auto& [label, field] : std::array{
			std::pair{ "Game version"s, &a_view.gameVersion },
			std::pair{ "Logger version"s, &a_view.loggerVersion },
			std::pair{ "Exception code"s, &a_view.exceptionCode },
			std::pair{ "Exception name"s, &a_view.exceptionName },
			std::pair{ "Fault location"s, &a_view.faultLocation } })
		{
			if (field->source)
				result.summarySources.emplace_back(label, *field->source);
		}
		return result;
	}

	ReportMetadata ParseReportMetadata(std::string_view a_text)
	{
		return AdaptReportMetadata(ParseReportView(
			a_text,
			ReportEncoding::kUtf8,
			true));
	}

	std::string BuildReportSummary(
		const ReportRecord& a_record,
		const ParsedReportView& a_view)
	{
		std::ostringstream output;
		output << "Report kind: " <<
			(a_record.kind == ReportKind::kCrash ? "Crash" : "Thread dump") <<
			'\n';
		output << "Report file: " << a_record.basename << '\n';
		output << "Recorded timestamp: " << a_record.recordedTimestamp << '\n';
		const auto append = [&](std::string_view label, const EvidenceField& field) {
			if (field.state == EvidenceState::kComplete)
				output << label << ": " << field.value << '\n';
			else if (field.state == EvidenceState::kPartial ||
				field.state == EvidenceState::kAmbiguous ||
				field.state == EvidenceState::kUnsupported)
				output << label << ": [" << EvidenceStateName(field.state) << "]\n";
		};
		append("Game version", a_view.gameVersion);
		append("Logger version", a_view.loggerVersion);
		append("Exception name", a_view.exceptionName);
		append("Exception code", a_view.exceptionCode);
		append("Fault location", a_view.faultLocation);
		if (!a_view.supportedGrammar)
			output << "Report status: unsupported structured format; raw preview only\n";
		if (!a_view.fullyRead)
			output << "Report status: partial (the selected input was not fully read)\n";
		else if (a_view.retentionCapped)
			output << "Report status: partial (parsed evidence retention limit reached)\n";
		return output.str();
	}

	std::string BuildReportSummary(
		const ReportRecord& a_record,
		const ReportMetadata& a_metadata)
	{
		ParsedReportView view;
		view.supportedGrammar = a_metadata.status != EvidenceState::kUnsupported;
		view.fullyRead = a_metadata.status == EvidenceState::kComplete;
		const auto copy = [](std::string_view value) {
			EvidenceField field;
			field.state = value.empty() ? EvidenceState::kAbsent :
				EvidenceState::kComplete;
			field.value = value;
			return field;
		};
		view.gameVersion = copy(a_metadata.gameVersion);
		view.loggerVersion = copy(a_metadata.loggerVersion);
		view.exceptionCode = copy(a_metadata.exceptionCode);
		view.exceptionName = copy(a_metadata.exceptionName);
		view.faultLocation = copy(a_metadata.faultLocation);
		return BuildReportSummary(a_record, view);
	}

	std::vector<size_t> FindLiteralReportMatches(
		std::string_view a_text,
		std::string_view a_query,
		bool& a_capped)
	{
		std::vector<size_t> result;
		a_capped = false;
		if (a_query.empty() || a_query.size() > kMaximumSearchQueryBytes)
			return result;
		size_t offset{};
		while (offset <= a_text.size())
		{
			const auto found = a_text.find(a_query, offset);
			if (found == std::string_view::npos)
				break;
			if (result.size() == kMaximumSearchHits)
			{
				a_capped = true;
				break;
			}
			result.push_back(found);
			offset = found + 1;
		}
		return result;
	}

	ReportDifferences CompareParsedReports(
		const ParsedReportView& a_left,
		const ParsedReportView& a_right)
	{
		std::vector<ReportDifference> result;
		CompareField(result, "Game version", a_left.gameVersion, a_right.gameVersion);
		CompareField(result, "Logger version", a_left.loggerVersion, a_right.loggerVersion);
		CompareField(result, "Exception name", a_left.exceptionName, a_right.exceptionName);
		CompareField(result, "Exception code", a_left.exceptionCode, a_right.exceptionCode);
		CompareField(result, "Fault location", a_left.faultLocation, a_right.faultLocation);

		AddStatusDifference(
			result, "Modules", a_left.modulesSection, a_right.modulesSection);
		if (a_left.modulesSection.state == EvidenceState::kComplete &&
			a_right.modulesSection.state == EvidenceState::kComplete)
		{
			std::map<std::string, const ModuleRecord*> left;
			std::map<std::string, const ModuleRecord*> right;
			for (const auto& record : a_left.modules)
				left.emplace(FoldAscii(record.name), &record);
			for (const auto& record : a_right.modules)
				right.emplace(FoldAscii(record.name), &record);
			for (const auto& [key, record] : left)
			{
				const auto found = right.find(key);
				if (found == right.end())
					result.push_back({
						"Modules",
						record->name + " is only listed in A.",
						record->source,
						std::nullopt
					});
				else if (record->aslrBase != found->second->aslrBase)
					result.push_back({
						"Modules",
						record->name +
							" reported ASLR base differs (not a version difference): A " +
							record->aslrBase + "; B " + found->second->aslrBase + ".",
						record->source,
						found->second->source
					});
			}
			for (const auto& [key, record] : right)
				if (!left.contains(key))
					result.push_back({
						"Modules",
						record->name + " is only listed in B.",
						std::nullopt,
						record->source
					});
		}

		AddStatusDifference(
			result,
			"F4SE plugins",
			a_left.f4sePluginsSection,
			a_right.f4sePluginsSection);
		if (a_left.f4sePluginsSection.state == EvidenceState::kComplete &&
			a_right.f4sePluginsSection.state == EvidenceState::kComplete)
		{
			std::map<std::string, const F4SEPluginRecord*> left;
			std::map<std::string, const F4SEPluginRecord*> right;
			for (const auto& record : a_left.f4sePlugins)
				left.emplace(FoldAscii(record.name), &record);
			for (const auto& record : a_right.f4sePlugins)
				right.emplace(FoldAscii(record.name), &record);
			for (const auto& [key, record] : left)
			{
				const auto found = right.find(key);
				if (found == right.end())
					result.push_back({
						"F4SE plugins",
						record->name + " is only listed in A.",
						record->source,
						std::nullopt
					});
				else if (record->reportedVersion &&
					found->second->reportedVersion &&
					*record->reportedVersion != *found->second->reportedVersion)
					result.push_back({
						"F4SE plugins",
						record->name + " reported version differs: A " +
							*record->reportedVersion + "; B " +
							*found->second->reportedVersion +
							". Equal versions would not prove equal binaries.",
						record->source,
						found->second->source
					});
				else if (record->reportedVersion.has_value() !=
					found->second->reportedVersion.has_value())
					result.push_back({
						"F4SE plugins",
						record->name +
							" has a reported version in only one report; the other version is unknown.",
						record->source,
						found->second->source
					});
			}
			for (const auto& [key, record] : right)
				if (!left.contains(key))
					result.push_back({
						"F4SE plugins",
						record->name + " is only listed in B.",
						std::nullopt,
						record->source
					});
		}

		AddStatusDifference(
			result, "Plugins", a_left.pluginsSection, a_right.pluginsSection);
		if (a_left.pluginsSection.state == EvidenceState::kComplete &&
			a_right.pluginsSection.state == EvidenceState::kComplete)
		{
			std::map<std::string, const PluginRecord*> left;
			std::map<std::string, const PluginRecord*> right;
			for (const auto& record : a_left.plugins)
				left.emplace(FoldAscii(record.name), &record);
			for (const auto& record : a_right.plugins)
				right.emplace(FoldAscii(record.name), &record);
			for (const auto& [key, record] : left)
			{
				const auto found = right.find(key);
				if (found == right.end())
					result.push_back({
						"Plugins",
						record->name + " is only listed in A.",
						record->source,
						std::nullopt
					});
				else if (record->kind != found->second->kind ||
					record->reportedIndex != found->second->reportedIndex)
					result.push_back({
						"Plugins",
						record->name + " reported load index differs: A " +
							(record->kind == PluginKind::kLight ? "light " : "regular ") +
							record->reportedIndex + "; B " +
							(found->second->kind == PluginKind::kLight ?
								"light " : "regular ") +
							found->second->reportedIndex + ".",
						record->source,
						found->second->source
					});
			}
			for (const auto& [key, record] : right)
				if (!left.contains(key))
					result.push_back({
						"Plugins",
						record->name + " is only listed in B.",
						std::nullopt,
						record->source
					});
		}
		const auto leftBytes = (std::min)(kMaximumComparisonRetainedBytes, a_left.retainedBytes);
		const auto parsedBytes = leftBytes + (std::min)(
			kMaximumComparisonRetainedBytes - leftBytes, a_right.retainedBytes);
		auto retainedBytes = parsedBytes + size_t{ 4096 };
		size_t retained{};
		for (const auto& difference : result)
		{
			const auto sourceBytes =
				(difference.leftSource ?
					difference.leftSource->section.size() : 0) +
				(difference.rightSource ?
					difference.rightSource->section.size() : 0);
			const auto bytes = sizeof(ReportDifference) + sourceBytes +
				2 * (difference.category.size() + difference.message.size());
			if (retainedBytes > kMaximumComparisonRetainedBytes ||
				bytes > kMaximumComparisonRetainedBytes - retainedBytes)
			{
				break;
			}
			retainedBytes += bytes;
			++retained;
		}
		return {
			std::vector<ReportDifference>(
				std::make_move_iterator(result.begin()),
				std::make_move_iterator(result.begin() + retained)),
			retained < result.size()
		};
	}

	std::string BuildComparisonSummary(
		const ReportRecord& a_left,
		const ReportRecord& a_right,
		std::span<const ReportDifference> a_differences,
		bool a_truncated)
	{
		std::ostringstream output;
		output << "Report comparison (recorded facts only)\n";
		output << "A: " << a_left.basename << " (" << a_left.recordedTimestamp << ")\n";
		output << "B: " << a_right.basename << " (" << a_right.recordedTimestamp << ")\n";
		if (a_truncated)
			output << "Comparison is incomplete: differences were omitted at the retention limit.\n";
		if (a_differences.empty() && !a_truncated)
			output << "No differences were found among complete supported fields. "
				"This does not prove identical binaries or a shared cause.\n";
		else
			for (const auto& difference : a_differences)
				output << difference.category << ": " << difference.message << '\n';
		return output.str();
	}
}
