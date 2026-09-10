#include "Introspection/ReadonlyIntrospection.h"

#include "Capture/DbgHelpGate.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <DbgHelp.h>
#include <filesystem>
#include <fmt/format.h>
#include <limits>

namespace Crash::Introspection::ReadOnly
{
	namespace
	{
		[[nodiscard]] Capture::Error error(
			Capture::ErrorCode a_code,
			DWORD a_systemError,
			std::string a_message,
			std::size_t a_completed = 0)
		{
			return Capture::Error{
				.code = a_code,
				.systemError = a_systemError,
				.message = std::move(a_message),
				.bytesCompleted = a_completed
			};
		}

		[[nodiscard]] bool readable(DWORD a_protect) noexcept
		{
			if ((a_protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
				return false;
			switch (a_protect & 0xFF)
			{
			case PAGE_READONLY:
			case PAGE_READWRITE:
			case PAGE_WRITECOPY:
			case PAGE_EXECUTE:
			case PAGE_EXECUTE_READ:
			case PAGE_EXECUTE_READWRITE:
			case PAGE_EXECUTE_WRITECOPY:
				return true;
			default:
				return false;
			}
		}

		[[nodiscard]] bool looks_like_form(std::string_view a_name) noexcept
		{
			return a_name.find("TESForm") != std::string_view::npos ||
			       a_name.find("TESObject") != std::string_view::npos ||
			       a_name.find("Character") != std::string_view::npos ||
			       a_name.find("PlayerCharacter") != std::string_view::npos ||
			       a_name.find("TESQuest") != std::string_view::npos ||
			       a_name.find("TESNPC") != std::string_view::npos ||
			       a_name.find("TESFaction") != std::string_view::npos;
		}

		[[nodiscard]] std::string display_type_name(
			std::string_view a_decorated)
		{
			auto gate = Capture::DbgHelpGate::try_lock();
			if (!gate.owns_lock())
				return std::string(a_decorated);
			std::array<char, 1024> buffer{};
			const auto* source = a_decorated.data();
			if (!a_decorated.empty() && a_decorated.front() == '.')
				++source;
			if (::UnDecorateSymbolName(
					source,
					buffer.data(),
					static_cast<DWORD>(buffer.size()),
					UNDNAME_NAME_ONLY) == 0)
				return std::string(a_decorated);
			return std::string(buffer.data());
		}

		struct CompleteObjectLocator
		{
			std::uint32_t signature{};
			std::uint32_t offset{};
			std::uint32_t constructorDisplacement{};
			std::uint32_t typeDescriptorRva{};
			std::uint32_t classDescriptorRva{};
			std::uint32_t selfRva{};
		};

		struct StringPoolEntry
		{
			std::uint64_t left{};
			std::uint16_t flags{};
			std::uint16_t crc{};
			std::uint32_t padding{};
			std::uint64_t lengthOrRight{};
		};

		constexpr std::uint16_t kStringShallow = 1u << 14;
		constexpr std::uint16_t kStringWide = 1u << 15;
	}

	std::expected<TargetAddress, Capture::Error> TargetAddress::add(
		std::uint64_t a_offset) const noexcept
	{
		if (a_offset > std::numeric_limits<std::uint64_t>::max() - m_value)
			return std::unexpected(error(
				Capture::ErrorCode::kAddressOverflow,
				ERROR_ARITHMETIC_OVERFLOW,
				"target address addition overflow"));
		return TargetAddress(m_value + a_offset);
	}

	std::expected<Capture::ReadResult, Capture::Error> LiveMemoryReader::read(
		TargetAddress a_address,
		std::span<std::byte> a_destination) const
	{
		if (a_destination.empty())
			return Capture::ReadResult{};
		if (a_destination.size() >
			std::numeric_limits<std::uint64_t>::max() - a_address.value())
			return std::unexpected(error(
				Capture::ErrorCode::kAddressOverflow,
				ERROR_ARITHMETIC_OVERFLOW,
				"live read address overflow"));

		std::size_t completed{};
		while (completed < a_destination.size())
		{
			const auto current = a_address.value() + completed;
			MEMORY_BASIC_INFORMATION information{};
			if (::VirtualQuery(
					reinterpret_cast<const void*>(current),
					std::addressof(information),
					sizeof(information)) != sizeof(information) ||
				information.State != MEM_COMMIT ||
				!readable(information.Protect))
				return std::unexpected(error(
					completed == 0 ?
						Capture::ErrorCode::kUnreadable :
						Capture::ErrorCode::kPartialRead,
					ERROR_NOACCESS,
					"live best-effort reader encountered unavailable memory",
					completed));
			const auto regionBase =
				reinterpret_cast<std::uint64_t>(information.BaseAddress);
			const auto regionEnd = regionBase + information.RegionSize;
			const auto chunk = static_cast<std::size_t>(
				std::min<std::uint64_t>(
					a_destination.size() - completed,
					regionEnd - current));
			SIZE_T bytesRead{};
			if (!::ReadProcessMemory(
					::GetCurrentProcess(),
					reinterpret_cast<const void*>(current),
					a_destination.data() + completed,
					chunk,
					std::addressof(bytesRead)) ||
				bytesRead != chunk)
				return std::unexpected(error(
					completed == 0 ?
						Capture::ErrorCode::kUnreadable :
						Capture::ErrorCode::kPartialRead,
					::GetLastError(),
					"live best-effort ReadProcessMemory failed",
					completed + bytesRead));
			completed += chunk;
		}
		return Capture::ReadResult{
			.bytesRead = completed,
			.provenance = Capture::MemoryProvenance::kUnknownWeak
		};
	}

	std::expected<Capture::ReadResult, Capture::Error> SnapshotMemoryReader::read(
		TargetAddress a_address,
		std::span<std::byte> a_destination) const
	{
		return m_operation.read(a_address.value(), a_destination);
	}

	AnalysisSession::AnalysisSession(
		const MemoryReader& a_reader,
		std::span<const ModuleRange> a_modules,
		RuntimeProfile a_profile,
		AnalysisBudgets a_budgets) :
		m_reader(a_reader),
		m_modules(a_modules),
		m_profile(a_profile),
		m_budgets(a_budgets)
	{}

	std::expected<Capture::ReadResult, Capture::Error> AnalysisSession::read_bytes(
		TargetAddress a_address,
		std::span<std::byte> a_destination)
	{
		if (m_diagnostics.reads >= m_budgets.maximumReads)
		{
			m_diagnostics.readBudgetExceeded = true;
			return std::unexpected(error(
				Capture::ErrorCode::kBudgetExceeded,
				ERROR_INSUFFICIENT_BUFFER,
				"read-only analysis exhausted its read budget"));
		}
		if (a_destination.size() >
			m_budgets.maximumBytes -
				std::min(m_diagnostics.bytes, m_budgets.maximumBytes))
		{
			m_diagnostics.byteBudgetExceeded = true;
			return std::unexpected(error(
				Capture::ErrorCode::kBudgetExceeded,
				ERROR_INSUFFICIENT_BUFFER,
				"read-only analysis exhausted its byte budget"));
		}
		++m_diagnostics.reads;
		auto result = m_reader.read(a_address, a_destination);
		if (result)
		{
			m_diagnostics.bytes += result->bytesRead;
			if (result->provenance == Capture::MemoryProvenance::kSharedImageWeak ||
				result->provenance == Capture::MemoryProvenance::kSharedMappedWeak ||
				result->provenance == Capture::MemoryProvenance::kUnknownWeak)
				++m_diagnostics.weakReads;
		}
		return result;
	}

	std::expected<std::string, Capture::Error> AnalysisSession::read_c_string(
		TargetAddress a_address,
		std::size_t a_maximum)
	{
		const auto maximum = std::min(
			a_maximum == 0 ? m_budgets.maximumStringBytes : a_maximum,
			m_budgets.maximumStringBytes);
		std::string value;
		value.reserve(maximum);
		for (std::size_t index = 0; index < maximum; ++index)
		{
			char character{};
			auto read = read_bytes(
				TargetAddress(a_address.value() + index),
				std::span<std::byte>{
					reinterpret_cast<std::byte*>(std::addressof(character)),
					sizeof(character) });
			if (!read)
				return std::unexpected(read.error());
			if (character == '\0')
				return value;
			const auto byte = static_cast<unsigned char>(character);
			if ((byte < 0x20 && character != '\t') || byte > 0x7E)
				return std::unexpected(error(
					Capture::ErrorCode::kInvalidMetadata,
					ERROR_INVALID_DATA,
					"target string contains non-printable bytes"));
			value.push_back(character);
		}
		return value;
	}

	const ModuleRange* AnalysisSession::module_for(TargetAddress a_address) const noexcept
	{
		for (const auto& module : m_modules)
			if (a_address.value() >= module.base &&
				a_address.value() - module.base < module.size)
				return std::addressof(module);
		return nullptr;
	}

	std::expected<AnalysisSession::RttiResult, Capture::Error>
	AnalysisSession::decode_rtti(TargetAddress a_address)
	{
		auto vtable = read_pod<std::uint64_t>(a_address);
		if (!vtable || !module_for(TargetAddress(vtable->value)))
			return std::unexpected(error(
				Capture::ErrorCode::kInvalidMetadata,
				ERROR_INVALID_DATA,
				"object vtable is outside the captured module catalog"));
		if (vtable->value < sizeof(std::uint64_t))
			return std::unexpected(error(
				Capture::ErrorCode::kInvalidMetadata,
				ERROR_INVALID_DATA,
				"object vtable address underflows RTTI locator slot"));
		auto locatorPointer = read_pod<std::uint64_t>(
			TargetAddress(vtable->value - sizeof(std::uint64_t)));
		if (!locatorPointer)
			return std::unexpected(locatorPointer.error());
		const auto* image = module_for(TargetAddress(locatorPointer->value));
		if (!image)
			return std::unexpected(error(
				Capture::ErrorCode::kInvalidMetadata,
				ERROR_INVALID_DATA,
				"RTTI complete-object locator is outside the module catalog"));
		auto locator = read_pod<CompleteObjectLocator>(
			TargetAddress(locatorPointer->value));
		if (!locator ||
			locator->value.signature != 1 ||
			locator->value.offset > 0x10000 ||
			locator->value.selfRva >= image->size ||
			locatorPointer->value - image->base != locator->value.selfRva ||
			locator->value.typeDescriptorRva >= image->size)
			return std::unexpected(error(
				Capture::ErrorCode::kInvalidMetadata,
				ERROR_INVALID_DATA,
				"RTTI complete-object locator failed image validation"));
		auto name = read_c_string(
			TargetAddress(image->base + locator->value.typeDescriptorRva + 16));
		if (!name || name->empty())
			return std::unexpected(name ?
				error(
					Capture::ErrorCode::kInvalidMetadata,
					ERROR_INVALID_DATA,
					"RTTI type descriptor has an empty name") :
				name.error());
		return RttiResult{
			.decoratedName = std::move(*name),
			.completeObject = a_address.value() - locator->value.offset,
			.baseOffset = locator->value.offset
		};
	}

	bool AnalysisSession::begin_object(TargetAddress a_address)
	{
		if (m_visited.contains(a_address.value()))
			return false;
		if (m_diagnostics.objects >= m_budgets.maximumObjects)
		{
			m_diagnostics.objectBudgetExceeded = true;
			return false;
		}
		m_visited.insert(a_address.value());
		++m_diagnostics.objects;
		return true;
	}

	std::string AnalysisSession::unavailable(std::string_view a_field)
	{
		++m_diagnostics.unavailableFields;
		return fmt::format("{}=<unavailable: requires live engine semantics>", a_field);
	}

	std::string AnalysisSession::read_fixed_string(TargetAddress a_fixedStringAddress)
	{
		auto entryPointer = read_pod<std::uint64_t>(a_fixedStringAddress);
		if (!entryPointer || entryPointer->value == 0)
			return {};
		std::unordered_set<std::uint64_t> chain;
		auto current = entryPointer->value;
		for (std::size_t depth = 0; depth < 16; ++depth)
		{
			if (!chain.insert(current).second)
				return "<unavailable: BSStringPoolEntry cycle>";
			auto entry = read_pod<StringPoolEntry>(TargetAddress(current));
			if (!entry)
				return "<unavailable: BSStringPoolEntry unreadable>";
			if ((entry->value.flags & kStringShallow) != 0)
			{
				current = entry->value.lengthOrRight;
				if (current == 0)
					return {};
				continue;
			}
			const auto length = static_cast<std::uint32_t>(entry->value.lengthOrRight);
			if (length > m_budgets.maximumStringBytes)
				return "<unavailable: BSStringPoolEntry length exceeds budget>";
			if ((entry->value.flags & kStringWide) != 0)
				return "<unavailable: wide BSStringPoolEntry>";
			auto value = read_c_string(
				TargetAddress(current + sizeof(StringPoolEntry)),
				length + 1);
			return value ? *value : "<unavailable: BSStringPoolEntry data unreadable>";
		}
		return "<unavailable: BSStringPoolEntry chain too deep>";
	}

	std::string AnalysisSession::decode_known_fields(
		TargetAddress,
		const RttiResult& a_rtti,
		std::size_t)
	{
		std::vector<std::string> fields;
		const auto complete = TargetAddress(a_rtti.completeObject);
		if (looks_like_form(a_rtti.decoratedName))
		{
			struct FormScalars
			{
				std::uint32_t flags{};
				std::uint32_t formId{};
				std::uint16_t inGameFlags{};
				std::uint8_t formType{};
				std::uint8_t padding{};
			};
			auto values = read_pod<FormScalars>(
				TargetAddress(complete.value() + 0x10));
			if (values)
			{
				fields.push_back(fmt::format("Flags=0x{:08X}", values->value.flags));
				fields.push_back(fmt::format("FormID=0x{:08X}", values->value.formId));
				fields.push_back(fmt::format(
					"FormType={} (scalar; virtual type name unavailable)",
					values->value.formType));
			}
			else
				fields.push_back("TESForm fields=<unavailable: unreadable direct layout>");
			fields.push_back(unavailable("Description owner file"));
			fields.push_back(unavailable("EditorID"));
			fields.push_back(
				"SourceFiles=<unavailable: bounded TESFileArray layout was not verified>");
		}

		if (a_rtti.decoratedName.find("NiObjectNET") != std::string::npos ||
			a_rtti.decoratedName.find("NiAVObject") != std::string::npos)
		{
			const auto name = read_fixed_string(TargetAddress(complete.value() + 0x10));
			fields.push_back(name.empty() ? "Name=<empty>" : fmt::format("Name=\"{}\"", name));
		}
		if (a_rtti.decoratedName.find("NiAVObject") != std::string::npos)
		{
			auto parent = read_pod<std::uint64_t>(TargetAddress(complete.value() + 0x28));
			auto flags = read_pod<std::uint64_t>(TargetAddress(complete.value() + 0x108));
			fields.push_back(parent ?
				fmt::format("Parent=0x{:016X}", parent->value) :
				"Parent=<unavailable>");
			fields.push_back(flags ?
				fmt::format("Flags=0x{:016X}", flags->value) :
				"Flags=<unavailable>");
			fields.push_back(unavailable("TESObjectREFR for 3D"));
		}
		if (a_rtti.decoratedName.find("TESObjectREFR") != std::string::npos)
		{
			auto parentCell = read_pod<std::uint64_t>(
				TargetAddress(complete.value() + 0xB8));
			auto base = read_pod<std::uint64_t>(
				TargetAddress(complete.value() + 0xC0));
			fields.push_back(parentCell ?
				fmt::format("ParentCell=0x{:016X}", parentCell->value) :
				"ParentCell=<unavailable>");
			fields.push_back(base ?
				fmt::format("BaseForm=0x{:016X}", base->value) :
				"BaseForm=<unavailable>");
			fields.push_back(
				"LeveledBase=<unavailable: ExtraDataList container decoding not verified for this runtime>");
		}
		if (a_rtti.decoratedName.find("TESQuest") != std::string::npos)
		{
			auto stage = read_pod<std::uint16_t>(
				TargetAddress(complete.value() + 0x2B4));
			auto alreadyRun = read_pod<std::uint8_t>(
				TargetAddress(complete.value() + 0x2B6));
			fields.push_back(stage ?
				fmt::format("CurrentStage={}", stage->value) :
				"CurrentStage=<unavailable>");
			fields.push_back(alreadyRun ?
				fmt::format("AlreadyRun={}", alreadyRun->value != 0) :
				"AlreadyRun=<unavailable>");
		}
		if (a_rtti.decoratedName.find("CodeTasklet") != std::string::npos)
		{
			fields.push_back(
				"CapturedScriptFrames=<unavailable: runtime stack layout not verified>");
			fields.push_back(
				"NumericIP=<unavailable: runtime CodeTasklet frame layout not verified>");
			fields.push_back(
				"Function/Object/State/Source=<unavailable: no virtual Papyrus calls are permitted>");
			fields.push_back(
				"ObjectHandle=<unavailable: raw handle is not a FormID and live handle policy is forbidden>");
			fields.push_back(unavailable("TranslateIPToLineNumber"));
		}
		if (a_rtti.decoratedName.find("NiStream") != std::string::npos)
		{
			fields.push_back(
				"FixedArrays/Header=<unavailable: NiStream layout not verified for captured runtime>");
			fields.push_back(unavailable("Virtual resource stream name"));
		}
		if (a_rtti.decoratedName.find("NativeFunctionBase") !=
			std::string::npos)
			fields.push_back(
				"Object/Function/State=<unavailable: direct Papyrus layout not verified; virtual getters forbidden>");
		if (a_rtti.decoratedName.find("ObjectTypeInfo") !=
			std::string::npos)
			fields.push_back(
				"Name/DocString=<unavailable: direct ObjectTypeInfo layout not verified>");
		if (a_rtti.decoratedName.find("BSShaderProperty") !=
			std::string::npos)
			fields.push_back(
				"ShaderName/Flags/Extra=<unavailable: direct shader layout not verified; virtual getters forbidden>");
		if (a_rtti.decoratedName.find("TESFullName") !=
			std::string::npos)
			fields.push_back(
				"FullNameStorage=<unavailable: localized-string subobject offset not verified>");

		if (m_profile == RuntimeProfile::kUnsupported)
			fields.push_back(
				"RuntimeFields=<unsupported profile: generic RTTI and raw memory only>");

		std::string joined;
		for (const auto& field : fields)
		{
			if (!joined.empty())
				joined += ", ";
			joined += field;
		}
		return joined;
	}

	std::string AnalysisSession::analyze_one(
		TargetAddress a_address,
		std::string_view a_label)
	{
		if (a_address.value() == 0)
			return {};
		if (const auto found = m_results.find(a_address.value());
			found != m_results.end())
			return fmt::format(
				"(already seen{}{})",
				a_label.empty() ? "" : " at ",
				a_label);
		if (!begin_object(a_address))
			return "<unavailable: object budget or cycle>";

		std::string result;
		auto rtti = decode_rtti(a_address);
		if (rtti)
		{
			result = fmt::format(
				"({}*) 0x{:016X}",
				display_type_name(rtti->decoratedName),
				a_address.value());
			const auto fields = decode_known_fields(a_address, *rtti, 0);
			if (!fields.empty())
				result += fmt::format(" [{}]", fields);
		}
		else if (const auto* module = module_for(a_address))
		{
			result = fmt::format(
				"({}+0x{:X}) 0x{:016X}",
				module->name,
				a_address.value() - module->base,
				a_address.value());
		}
		else
		{
			result = fmt::format(
				"(void*) 0x{:016X} [memory region classification only; heap ownership unavailable]",
				a_address.value());
		}
		m_results.emplace(a_address.value(), result);
		return result;
	}

	std::vector<std::string> AnalysisSession::analyze(
		std::span<const std::uint64_t> a_addresses,
		std::span<const std::string> a_labels)
	{
		std::vector<std::string> results;
		results.reserve(a_addresses.size());
		for (std::size_t index = 0; index < a_addresses.size(); ++index)
			results.push_back(analyze_one(
				TargetAddress(a_addresses[index]),
				index < a_labels.size() ? a_labels[index] : std::string_view{}));
		return results;
	}

	const AnalysisDiagnostics& AnalysisSession::diagnostics() const noexcept
	{
		return m_diagnostics;
	}

	RuntimeProfile AnalysisSession::profile() const noexcept
	{
		return m_profile;
	}

	RuntimeProfile runtime_profile(
		std::uint16_t a_major,
		std::uint16_t a_minor,
		std::uint16_t a_build,
		std::uint16_t a_revision) noexcept
	{
		if (a_major == 1 && a_minor == 10 && a_build == 163)
			return RuntimeProfile::kOldGen_1_10_163;
		if (a_major == 1 && a_minor == 10 && a_build == 984)
			return RuntimeProfile::kNextGen_1_10_984;
		if (a_major == 1 && a_minor == 11 && a_build == 191)
			return RuntimeProfile::kAnniversary_1_11_191;
		if (a_major == 1 && a_minor == 11 && a_build == 221)
			return RuntimeProfile::kAnniversary_1_11_221;
		if (a_major == 1 && a_minor == 11 && a_build == 240)
			return RuntimeProfile::kAnniversary_1_11_240;
		(void)a_revision;
		return RuntimeProfile::kUnsupported;
	}

	std::vector<ModuleRange> snapshot_module_ranges(
		const Capture::SnapshotOperation& a_operation)
	{
		std::vector<ModuleRange> ranges;
		ranges.reserve(a_operation.modules().size());
		for (const auto& module : a_operation.modules())
		{
			std::filesystem::path path(module.path);
			ranges.push_back(ModuleRange{
				.base = module.base,
				.size = module.size,
				.name = path.filename().string(),
				.path = path.string()
			});
		}
		return ranges;
	}
}
