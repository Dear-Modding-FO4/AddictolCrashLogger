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

		struct ClassHierarchyDescriptor
		{
			std::uint32_t signature{};
			std::uint32_t attributes{};
			std::uint32_t baseClassCount{};
			std::uint32_t baseClassArrayRva{};
		};

		struct Pmd
		{
			std::int32_t memberDisplacement{};
			std::int32_t vbtableDisplacement{};
			std::int32_t displacementInVbtable{};
		};

		struct BaseClassDescriptor
		{
			std::uint32_t typeDescriptorRva{};
			std::uint32_t containedBaseCount{};
			Pmd pmd{};
			std::uint32_t attributes{};
		};

		static_assert(sizeof(CompleteObjectLocator) == 0x18);
		static_assert(sizeof(ClassHierarchyDescriptor) == 0x10);
		static_assert(sizeof(Pmd) == 0xC);
		static_assert(sizeof(BaseClassDescriptor) == 0x18);

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
		constexpr std::size_t kMaximumRttiBaseClasses = 256;
		constexpr std::uint64_t kMaximumBaseDisplacement = 0x100000;

		constexpr std::string_view kTesForm = ".?AVTESForm@@";
		constexpr std::string_view kNiObjectNet = ".?AVNiObjectNET@@";
		constexpr std::string_view kNiAvObject = ".?AVNiAVObject@@";
		constexpr std::string_view kTesObjectRefr = ".?AVTESObjectREFR@@";
		constexpr std::string_view kTesQuest = ".?AVTESQuest@@";
		constexpr std::string_view kCodeTasklet =
			".?AVCodeTasklet@Internal@BSScript@@";
		constexpr std::string_view kNiStream = ".?AVNiStream@@";
		constexpr std::string_view kNativeFunctionBase =
			".?AVNativeFunctionBase@NF_util@BSScript@@";
		constexpr std::string_view kObjectTypeInfo =
			".?AVObjectTypeInfo@BSScript@@";
		constexpr std::string_view kBsShaderProperty =
			".?AVBSShaderProperty@@";
		constexpr std::string_view kTesFullName = ".?AVTESFullName@@";

		[[nodiscard]] std::expected<TargetAddress, Capture::Error> module_rva(
			const ModuleRange& a_module,
			std::uint32_t a_rva,
			std::size_t a_size,
			std::string_view a_description)
		{
			if (a_rva >= a_module.size ||
				a_size > a_module.size - a_rva ||
				a_rva > std::numeric_limits<std::uint64_t>::max() - a_module.base ||
				a_size > std::numeric_limits<std::uint64_t>::max() -
					(a_module.base + a_rva))
				return std::unexpected(error(
					Capture::ErrorCode::kInvalidMetadata,
					ERROR_INVALID_DATA,
					fmt::format(
						"{} is outside its captured module",
						a_description)));
			return TargetAddress(a_module.base + a_rva);
		}

		[[nodiscard]] bool module_contains(
			const ModuleRange& a_module,
			TargetAddress a_address,
			std::size_t a_size) noexcept
		{
			if (a_address.value() < a_module.base)
				return false;
			const auto offset = a_address.value() - a_module.base;
			return offset < a_module.size &&
			       a_size <= a_module.size - offset;
		}
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
			if (information.RegionSize >
				std::numeric_limits<std::uint64_t>::max() - regionBase)
				return std::unexpected(error(
					completed == 0 ?
						Capture::ErrorCode::kAddressOverflow :
						Capture::ErrorCode::kPartialRead,
					ERROR_ARITHMETIC_OVERFLOW,
					"live memory region range overflow",
					completed));
			const auto regionEnd = regionBase + information.RegionSize;
			if (current < regionBase || current >= regionEnd)
				return std::unexpected(error(
					completed == 0 ?
						Capture::ErrorCode::kUnreadable :
						Capture::ErrorCode::kPartialRead,
					ERROR_NOACCESS,
					"live memory query did not cover the requested address",
					completed));
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
			{
				const auto total = completed + static_cast<std::size_t>(bytesRead);
				return std::unexpected(error(
					total == 0 ?
						Capture::ErrorCode::kUnreadable :
						Capture::ErrorCode::kPartialRead,
					::GetLastError(),
					"live best-effort ReadProcessMemory failed",
					total));
			}
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
		const auto completed = result ?
			result->bytesRead :
			result.error().bytesCompleted;
		m_diagnostics.bytes += std::min(completed, a_destination.size());
		if (result)
		{
			if (result->bytesRead > a_destination.size())
				return std::unexpected(error(
					Capture::ErrorCode::kInvalidMetadata,
					ERROR_INVALID_DATA,
					"memory reader reported more bytes than requested",
					a_destination.size()));
			if (result->bytesRead < a_destination.size())
				return std::unexpected(error(
					Capture::ErrorCode::kPartialRead,
					ERROR_PARTIAL_COPY,
					"memory reader returned successful partial coverage",
					result->bytesRead));
			if (result->provenance == Capture::MemoryProvenance::kSharedImageWeak ||
				result->provenance == Capture::MemoryProvenance::kSharedMappedWeak ||
				result->provenance == Capture::MemoryProvenance::kUnknownWeak)
				++m_diagnostics.weakReads;
		}
		else
		{
			if (result.error().bytesCompleted > a_destination.size())
				return std::unexpected(error(
					Capture::ErrorCode::kInvalidMetadata,
					ERROR_INVALID_DATA,
					"memory reader reported more completed bytes than requested",
					a_destination.size()));
			if (completed != 0)
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
		if (maximum == 0)
			return std::unexpected(error(
				Capture::ErrorCode::kBudgetExceeded, ERROR_INSUFFICIENT_BUFFER,
				"target string has no available read budget"));

		static const std::size_t pageSize = [] {
			SYSTEM_INFO information{};
			::GetSystemInfo(std::addressof(information));
			return information.dwPageSize == 0 ?
				std::size_t{ 4096 } :
				static_cast<std::size_t>(information.dwPageSize);
		}();
		std::array<char, 64> buffer{};
		std::size_t completed{};
		while (completed < maximum)
		{
			auto current = a_address.add(completed);
			if (!current)
				return std::unexpected(current.error());
			const auto pageOffset =
				static_cast<std::size_t>(current->value() % pageSize);
			const auto remainingByteBudget = m_budgets.maximumBytes -
				std::min(m_diagnostics.bytes, m_budgets.maximumBytes);
			if (remainingByteBudget == 0)
			{
				m_diagnostics.byteBudgetExceeded = true;
				return std::unexpected(error(
					Capture::ErrorCode::kBudgetExceeded, ERROR_INSUFFICIENT_BUFFER,
					"target string exhausted the analysis byte budget", completed));
			}
			const auto chunk = std::min({
				buffer.size(),
				maximum - completed,
				pageSize - pageOffset,
				remainingByteBudget
			});
			auto read = read_bytes(
				*current,
				std::span<std::byte>{
					reinterpret_cast<std::byte*>(buffer.data()),
					chunk });
			if (!read && read.error().code != Capture::ErrorCode::kPartialRead)
				return std::unexpected(read.error());
			const auto available = read ?
				chunk :
				std::min(read.error().bytesCompleted, chunk);
			for (std::size_t index = 0; index < available; ++index)
			{
				const auto character = buffer[index];
				if (character == '\0')
					return value;
				const auto byte = static_cast<unsigned char>(character);
				if ((byte < 0x20 && character != '\t' && character != '\n' && character != '\r') || byte > 0x7E)
					return std::unexpected(error(
						Capture::ErrorCode::kInvalidMetadata,
						ERROR_INVALID_DATA,
						"target string contains non-printable bytes",
						completed + index));
				value.push_back(character);
			}
			if (!read)
				return std::unexpected(read.error());
			completed += chunk;
		}
		return std::unexpected(error(
			Capture::ErrorCode::kUnavailable, ERROR_INSUFFICIENT_BUFFER,
			"target string is not terminated within its length limit", completed));
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
		if (!vtable)
			return std::unexpected(vtable.error());
		const auto* vtableImage = module_for(TargetAddress(vtable->value));
		if (!vtableImage)
			return std::unexpected(error(
				Capture::ErrorCode::kInvalidMetadata,
				ERROR_INVALID_DATA,
				"object vtable is outside the captured module catalog"));
		if (vtable->value < sizeof(std::uint64_t))
			return std::unexpected(error(
				Capture::ErrorCode::kInvalidMetadata,
				ERROR_INVALID_DATA,
				"object vtable address underflows RTTI locator slot"));
		const auto locatorSlot =
			TargetAddress(vtable->value - sizeof(std::uint64_t));
		if (!module_contains(*vtableImage, locatorSlot, sizeof(std::uint64_t)))
			return std::unexpected(error(
				Capture::ErrorCode::kInvalidMetadata,
				ERROR_INVALID_DATA,
				"RTTI locator slot is outside the vtable module"));
		auto locatorPointer = read_pod<std::uint64_t>(locatorSlot);
		if (!locatorPointer)
			return std::unexpected(locatorPointer.error());
		const auto* image = module_for(TargetAddress(locatorPointer->value));
		if (!image || image != vtableImage ||
			!module_contains(
				*image,
				TargetAddress(locatorPointer->value),
				sizeof(CompleteObjectLocator)))
			return std::unexpected(error(
				Capture::ErrorCode::kInvalidMetadata,
				ERROR_INVALID_DATA,
				"RTTI complete-object locator is outside the vtable module"));
		auto locator = read_pod<CompleteObjectLocator>(
			TargetAddress(locatorPointer->value));
		if (!locator)
			return std::unexpected(locator.error());
		if (locator->value.signature != 1 ||
			locator->value.offset > 0x10000 ||
			locator->value.selfRva >= image->size ||
			locatorPointer->value - image->base != locator->value.selfRva ||
			locator->value.typeDescriptorRva == 0 ||
			locator->value.classDescriptorRva == 0 ||
			locator->value.offset > a_address.value())
			return std::unexpected(error(
				Capture::ErrorCode::kInvalidMetadata,
				ERROR_INVALID_DATA,
				"RTTI complete-object locator failed image validation"));
		if (locator->value.constructorDisplacement != 0)
			return std::unexpected(error(
				Capture::ErrorCode::kUnavailable,
				ERROR_NOT_SUPPORTED,
				"RTTI construction displacement layout is unsupported"));

		auto read_type_name =
			[this, image](std::uint32_t a_typeDescriptorRva)
			-> std::expected<std::string, Capture::Error>
		{
			auto descriptor = module_rva(
				*image,
				a_typeDescriptorRva,
				17,
				"RTTI type descriptor");
			if (!descriptor)
				return std::unexpected(descriptor.error());
			auto nameAddress = descriptor->add(16);
			if (!nameAddress)
				return std::unexpected(nameAddress.error());
			const auto available = static_cast<std::size_t>(
				std::min<std::uint64_t>(
					m_budgets.maximumStringBytes,
					image->size - a_typeDescriptorRva - 16));
			auto name = read_c_string(*nameAddress, available);
			if (!name)
				return std::unexpected(name.error());
			if (name->empty())
				return std::unexpected(error(
					Capture::ErrorCode::kInvalidMetadata,
					ERROR_INVALID_DATA,
					"RTTI type descriptor has an empty name"));
			return name;
		};

		auto name = read_type_name(locator->value.typeDescriptorRva);
		if (!name || name->empty())
			return std::unexpected(name.error());
		RttiResult result{
			.decoratedName = std::move(*name),
			.completeObject = a_address.value() - locator->value.offset,
			.baseOffset = locator->value.offset
		};

		auto hierarchyAddress = module_rva(
			*image,
			locator->value.classDescriptorRva,
			sizeof(ClassHierarchyDescriptor),
			"RTTI class hierarchy descriptor");
		if (!hierarchyAddress)
		{
			result.hierarchyUnavailable =
				"invalid RTTI class hierarchy descriptor";
			return result;
		}
		auto hierarchy = read_pod<ClassHierarchyDescriptor>(*hierarchyAddress);
		if (!hierarchy)
		{
			result.hierarchyUnavailable =
				hierarchy.error().code == Capture::ErrorCode::kBudgetExceeded ?
				"RTTI class hierarchy analysis budget exhausted" :
				"RTTI class hierarchy descriptor unreadable";
			return result;
		}
		if (hierarchy->value.signature != 0 ||
			(hierarchy->value.attributes & ~0x7u) != 0 ||
			hierarchy->value.baseClassCount == 0 ||
			hierarchy->value.baseClassCount > kMaximumRttiBaseClasses ||
			hierarchy->value.baseClassArrayRva == 0)
		{
			result.hierarchyUnavailable =
				"invalid bounded RTTI class hierarchy metadata";
			return result;
		}
		if ((hierarchy->value.attributes & 0x4u) != 0)
		{
			result.hierarchyUnavailable =
				"ambiguous RTTI class hierarchy";
			return result;
		}

		std::array<std::uint32_t, kMaximumRttiBaseClasses> descriptorRvas{};
		const auto descriptorArraySize =
			hierarchy->value.baseClassCount * sizeof(std::uint32_t);
		auto descriptorArrayAddress = module_rva(
			*image,
			hierarchy->value.baseClassArrayRva,
			descriptorArraySize,
			"RTTI base-class array");
		if (!descriptorArrayAddress)
		{
			result.hierarchyUnavailable =
				"invalid RTTI base-class array";
			return result;
		}
		auto descriptorArray = read_bytes(
			*descriptorArrayAddress,
			std::span<std::byte>{
				reinterpret_cast<std::byte*>(descriptorRvas.data()),
				descriptorArraySize });
		if (!descriptorArray)
		{
			result.hierarchyUnavailable =
				descriptorArray.error().code == Capture::ErrorCode::kBudgetExceeded ?
				"RTTI base-class array analysis budget exhausted" :
				"RTTI base-class array unreadable";
			return result;
		}

		result.bases.reserve(hierarchy->value.baseClassCount);
		bool sawVirtualBase{};
		for (std::size_t index = 0;
			 index < hierarchy->value.baseClassCount;
			 ++index)
		{
			if (descriptorRvas[index] == 0)
			{
				result.bases.clear();
				result.hierarchyUnavailable =
					"RTTI base-class descriptor has a null RVA";
				return result;
			}
			auto descriptorAddress = module_rva(
				*image,
				descriptorRvas[index],
				sizeof(BaseClassDescriptor),
				"RTTI base-class descriptor");
			if (!descriptorAddress)
			{
				result.bases.clear();
				result.hierarchyUnavailable =
					"invalid RTTI base-class descriptor";
				return result;
			}
			auto descriptor = read_pod<BaseClassDescriptor>(*descriptorAddress);
			if (!descriptor)
			{
				result.bases.clear();
				result.hierarchyUnavailable =
					descriptor.error().code == Capture::ErrorCode::kBudgetExceeded ?
					"RTTI base-class descriptor analysis budget exhausted" :
					"RTTI base-class descriptor unreadable";
				return result;
			}
			if (descriptor->value.typeDescriptorRva == 0 ||
				descriptor->value.containedBaseCount >=
					hierarchy->value.baseClassCount - index ||
				(descriptor->value.attributes & ~0x7Fu) != 0)
			{
				result.bases.clear();
				result.hierarchyUnavailable =
					"invalid RTTI base-class descriptor metadata";
				return result;
			}
			if (index == 0 &&
				(descriptor->value.typeDescriptorRva != locator->value.typeDescriptorRva ||
					descriptor->value.pmd.memberDisplacement != 0 ||
					descriptor->value.pmd.vbtableDisplacement != -1))
			{
				result.bases.clear();
				result.hierarchyUnavailable = "RTTI hierarchy root does not match the complete object";
				return result;
			}
			auto baseName =
				read_type_name(descriptor->value.typeDescriptorRva);
			if (!baseName)
			{
				result.bases.clear();
				result.hierarchyUnavailable =
					baseName.error().code == Capture::ErrorCode::kBudgetExceeded ?
					"RTTI base type-name analysis budget exhausted" :
					"RTTI base type descriptor unreadable";
				return result;
			}

			RttiBase base{ .decoratedName = std::move(*baseName) };
			const auto& pmd = descriptor->value.pmd;
			const auto isAmbiguous =
				(descriptor->value.attributes & 0x2u) != 0;
			const auto isVirtual =
				(descriptor->value.attributes & 0x10u) != 0;
			const auto hasVirtualPmd = pmd.vbtableDisplacement >= 0;
			if (isVirtual != hasVirtualPmd)
			{
				result.bases.clear();
				result.hierarchyUnavailable =
					"inconsistent virtual-base PMD metadata";
				return result;
			}
			if (isVirtual)
			{
				sawVirtualBase = true;
				base.unavailableReason = isAmbiguous ?
					"ambiguous RTTI base" :
					"virtual RTTI base layout unsupported";
			}
			else if (pmd.vbtableDisplacement != -1 ||
				pmd.memberDisplacement < 0 ||
				static_cast<std::uint64_t>(pmd.memberDisplacement) >
					kMaximumBaseDisplacement)
			{
				result.bases.clear();
				result.hierarchyUnavailable =
					"invalid non-virtual PMD metadata";
				return result;
			}
			else if (isAmbiguous)
				base.unavailableReason = "ambiguous RTTI base";
			else
			{
				auto baseAddress = TargetAddress(result.completeObject).add(
					static_cast<std::uint64_t>(pmd.memberDisplacement));
				if (!baseAddress)
				{
					result.bases.clear();
					result.hierarchyUnavailable =
						"RTTI base-object address overflow";
					return result;
				}
				base.address = baseAddress->value();
			}
			result.bases.push_back(std::move(base));
		}
		if (((hierarchy->value.attributes & 0x2u) != 0) != sawVirtualBase)
		{
			result.bases.clear();
			result.hierarchyUnavailable =
				"inconsistent virtual-inheritance hierarchy metadata";
		}
		return result;
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
			auto dataAddress = TargetAddress(current).add(sizeof(StringPoolEntry));
			if (!dataAddress)
				return "<unavailable: BSStringPoolEntry data address overflow>";
			auto value = read_c_string(
				*dataAddress,
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
		if (m_profile == RuntimeProfile::kUnsupported)
			return "RuntimeFields=<unsupported profile: generic RTTI and raw memory only>";
		if (!a_rtti.hierarchyUnavailable.empty())
			return fmt::format(
				"RuntimeFields=<unavailable: {}>",
				a_rtti.hierarchyUnavailable);

		auto read_at = [this]<class T>(
			TargetAddress a_base,
			std::uint64_t a_offset)
			-> std::expected<Capture::ReadObjectResult<T>, Capture::Error>
		{
			auto address = a_base.add(a_offset);
			if (!address)
				return std::unexpected(address.error());
			return read_pod<T>(*address);
		};
		auto find_base = [&a_rtti](
			std::string_view a_name,
			std::string& a_reason) -> const RttiBase*
		{
			const RttiBase* found{};
			for (const auto& base : a_rtti.bases)
			{
				if (base.decoratedName != a_name)
					continue;
				if (found)
				{
					a_reason = "ambiguous duplicate RTTI base";
					return nullptr;
				}
				found = std::addressof(base);
			}
			if (found && !found->unavailableReason.empty())
			{
				a_reason = found->unavailableReason;
				return nullptr;
			}
			return found;
		};
		auto has_type = [&a_rtti](std::string_view a_name)
		{
			for (const auto& base : a_rtti.bases)
				if (base.decoratedName == a_name)
					return true;
			return false;
		};

		std::string reason;
		const auto* formBase = find_base(kTesForm, reason);
		if (formBase)
		{
			struct FormScalars
			{
				std::uint32_t flags{};
				std::uint32_t formId{};
				std::uint16_t inGameFlags{};
				std::uint8_t formType{};
				std::uint8_t padding{};
			};
			auto values = read_at.operator()<FormScalars>(
				TargetAddress(formBase->address),
				0x10);
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
		else if (!reason.empty())
			fields.push_back(fmt::format(
				"TESForm fields=<unavailable: {}>",
				reason));

		reason.clear();
		const auto* objectNetBase = find_base(kNiObjectNet, reason);
		if (objectNetBase)
		{
			auto nameAddress = TargetAddress(objectNetBase->address).add(0x10);
			if (!nameAddress)
				fields.push_back("Name=<unavailable: address overflow>");
			else
			{
				const auto name = read_fixed_string(*nameAddress);
				fields.push_back(name.empty() ?
					"Name=<empty>" :
					fmt::format("Name=\"{}\"", name));
			}
		}
		else if (!reason.empty())
			fields.push_back(fmt::format(
				"NiObjectNET fields=<unavailable: {}>",
				reason));

		reason.clear();
		const auto* avObjectBase = find_base(kNiAvObject, reason);
		if (avObjectBase)
		{
			auto parent = read_at.operator()<std::uint64_t>(
				TargetAddress(avObjectBase->address),
				0x28);
			auto flags = read_at.operator()<std::uint64_t>(
				TargetAddress(avObjectBase->address),
				0x108);
			fields.push_back(parent ?
				fmt::format("Parent=0x{:016X}", parent->value) :
				"Parent=<unavailable>");
			fields.push_back(flags ?
				fmt::format("Flags=0x{:016X}", flags->value) :
				"Flags=<unavailable>");
			fields.push_back(unavailable("TESObjectREFR for 3D"));
		}
		else if (!reason.empty())
			fields.push_back(fmt::format(
				"NiAVObject fields=<unavailable: {}>",
				reason));

		reason.clear();
		const auto* refrBase = find_base(kTesObjectRefr, reason);
		if (refrBase)
		{
			auto parentCell = read_at.operator()<std::uint64_t>(
				TargetAddress(refrBase->address),
				0xB8);
			auto base = read_at.operator()<std::uint64_t>(
				TargetAddress(refrBase->address),
				0xE0);
			fields.push_back(parentCell ?
				fmt::format("ParentCell=0x{:016X}", parentCell->value) :
				"ParentCell=<unavailable>");
			fields.push_back(base ?
				fmt::format("BaseForm=0x{:016X}", base->value) :
				"BaseForm=<unavailable>");
			fields.push_back(
				"LeveledBase=<unavailable: ExtraDataList container decoding not verified for this runtime>");
		}
		else if (!reason.empty())
			fields.push_back(fmt::format(
				"TESObjectREFR fields=<unavailable: {}>",
				reason));

		reason.clear();
		const auto* questBase = find_base(kTesQuest, reason);
		if (questBase)
		{
			auto stage = read_at.operator()<std::uint16_t>(
				TargetAddress(questBase->address),
				0x2B4);
			auto alreadyRun = read_at.operator()<std::uint8_t>(
				TargetAddress(questBase->address),
				0x2B6);
			fields.push_back(stage ?
				fmt::format("CurrentStage={}", stage->value) :
				"CurrentStage=<unavailable>");
			fields.push_back(alreadyRun ?
				fmt::format("AlreadyRun={}", alreadyRun->value != 0) :
				"AlreadyRun=<unavailable>");
		}
		else if (!reason.empty())
			fields.push_back(fmt::format(
				"TESQuest fields=<unavailable: {}>",
				reason));

		if (has_type(kCodeTasklet))
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
		if (has_type(kNiStream))
		{
			fields.push_back(
				"FixedArrays/Header=<unavailable: NiStream layout not verified for captured runtime>");
			fields.push_back(unavailable("Virtual resource stream name"));
		}
		if (has_type(kNativeFunctionBase))
			fields.push_back(
				"Object/Function/State=<unavailable: direct Papyrus layout not verified; virtual getters forbidden>");
		if (has_type(kObjectTypeInfo))
			fields.push_back(
				"Name/DocString=<unavailable: direct ObjectTypeInfo layout not verified>");
		if (has_type(kBsShaderProperty))
			fields.push_back(
				"ShaderName/Flags/Extra=<unavailable: direct shader layout not verified; virtual getters forbidden>");
		if (has_type(kTesFullName))
			fields.push_back(
				"FullNameStorage=<unavailable: localized-string subobject offset not verified>");

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
			return found->second;
		(void)a_label;
		if (m_diagnostics.objects >= m_budgets.maximumObjects)
		{
			m_diagnostics.objectBudgetExceeded = true;
			return "<unavailable: object budget>";
		}

		std::string result;
		auto rtti = decode_rtti(a_address);
		const auto objectAddress = rtti ?
			TargetAddress(rtti->completeObject) :
			a_address;
		if (!begin_object(objectAddress) &&
			(!rtti || !m_fieldResults.contains(rtti->completeObject)))
			return "<unavailable: object budget or cycle>";
		if (rtti)
		{
			result = fmt::format(
				"({}*) 0x{:016X}",
				display_type_name(rtti->decoratedName),
				a_address.value());
			auto fields = m_fieldResults.find(rtti->completeObject);
			if (fields == m_fieldResults.end())
				fields = m_fieldResults.emplace(
					rtti->completeObject,
					decode_known_fields(a_address, *rtti, 0)).first;
			if (!fields->second.empty())
				result += fmt::format(" [{}]", fields->second);
		}
		else if (rtti.error().code == Capture::ErrorCode::kBudgetExceeded)
			result = fmt::format(
				"(void*) 0x{:016X} [RTTI=<unavailable: analysis budget exhausted>]",
				a_address.value());
		else if (rtti.error().code == Capture::ErrorCode::kUnavailable)
			result = fmt::format(
				"(void*) 0x{:016X} [RTTI=<unavailable: {}>]",
				a_address.value(),
				rtti.error().message);
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
