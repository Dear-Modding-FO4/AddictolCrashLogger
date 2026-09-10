#pragma once

#include "Capture/ProcessSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Crash::Introspection::ReadOnly
{
	class TargetAddress
	{
	public:
		constexpr TargetAddress() = default;
		explicit constexpr TargetAddress(std::uint64_t a_value) : m_value(a_value) {}

		[[nodiscard]] constexpr std::uint64_t value() const noexcept { return m_value; }
		[[nodiscard]] std::expected<TargetAddress, Capture::Error> add(
			std::uint64_t a_offset) const noexcept;

	private:
		std::uint64_t m_value{};
	};

	struct ModuleRange
	{
		std::uint64_t base{};
		std::uint64_t size{};
		std::string name;
		std::string path;
	};

	class MemoryReader
	{
	public:
		virtual ~MemoryReader() = default;
		[[nodiscard]] virtual std::expected<Capture::ReadResult, Capture::Error> read(
			TargetAddress a_address,
			std::span<std::byte> a_destination) const = 0;
	};

	class LiveMemoryReader final : public MemoryReader
	{
	public:
		[[nodiscard]] std::expected<Capture::ReadResult, Capture::Error> read(
			TargetAddress a_address,
			std::span<std::byte> a_destination) const override;
	};

	class SnapshotMemoryReader final : public MemoryReader
	{
	public:
		explicit SnapshotMemoryReader(Capture::SnapshotOperation& a_operation) :
			m_operation(a_operation)
		{}

		[[nodiscard]] std::expected<Capture::ReadResult, Capture::Error> read(
			TargetAddress a_address,
			std::span<std::byte> a_destination) const override;

	private:
		Capture::SnapshotOperation& m_operation;
	};

	enum class RuntimeProfile
	{
		kOldGen_1_10_163,
		kNextGen_1_10_984,
		kAnniversary_1_11_191,
		kAnniversary_1_11_221,
		kAnniversary_1_11_240,
		kUnsupported
	};

	struct AnalysisBudgets
	{
		std::size_t maximumReads{ 4096 };
		std::size_t maximumBytes{ 512u * 1024u };
		std::size_t maximumObjects{ 512 };
		std::size_t maximumDepth{ 8 };
		std::size_t maximumStringBytes{ 512 };
	};

	struct AnalysisDiagnostics
	{
		std::size_t reads{};
		std::size_t bytes{};
		std::size_t objects{};
		std::size_t weakReads{};
		std::size_t unavailableFields{};
		bool readBudgetExceeded{};
		bool byteBudgetExceeded{};
		bool objectBudgetExceeded{};
	};

	class AnalysisSession
	{
	public:
		AnalysisSession(
			const MemoryReader& a_reader,
			std::span<const ModuleRange> a_modules,
			RuntimeProfile a_profile,
			AnalysisBudgets a_budgets = {});

		[[nodiscard]] std::vector<std::string> analyze(
			std::span<const std::uint64_t> a_addresses,
			std::span<const std::string> a_labels = {});
		[[nodiscard]] std::string analyze_one(
			TargetAddress a_address,
			std::string_view a_label = {});

		[[nodiscard]] const AnalysisDiagnostics& diagnostics() const noexcept;
		[[nodiscard]] RuntimeProfile profile() const noexcept;
		[[nodiscard]] const ModuleRange* module_for(TargetAddress a_address) const noexcept;

		template <class T>
			requires(
				std::is_trivially_copyable_v<T> &&
				std::is_standard_layout_v<T> &&
				!std::is_polymorphic_v<T>)
		[[nodiscard]] std::expected<Capture::ReadObjectResult<T>, Capture::Error> read_pod(
			TargetAddress a_address)
		{
			T value{};
			auto read = read_bytes(
				a_address,
				std::span<std::byte>{
					reinterpret_cast<std::byte*>(std::addressof(value)),
					sizeof(value) });
			if (!read)
				return std::unexpected(read.error());
			return Capture::ReadObjectResult<T>{
				.value = value,
				.provenance = read->provenance
			};
		}

		[[nodiscard]] std::expected<Capture::ReadResult, Capture::Error> read_bytes(
			TargetAddress a_address,
			std::span<std::byte> a_destination);
		[[nodiscard]] std::expected<std::string, Capture::Error> read_c_string(
			TargetAddress a_address,
			std::size_t a_maximum = 0);

	private:
		struct RttiBase
		{
			std::string decoratedName;
			std::uint64_t address{};
			std::string unavailableReason;
		};

		struct RttiResult
		{
			std::string decoratedName;
			std::uint64_t completeObject{};
			std::uint32_t baseOffset{};
			std::vector<RttiBase> bases;
			std::string hierarchyUnavailable;
		};

		[[nodiscard]] std::expected<RttiResult, Capture::Error> decode_rtti(
			TargetAddress a_address);
		[[nodiscard]] std::string decode_known_fields(
			TargetAddress a_address,
			const RttiResult& a_rtti,
			std::size_t a_depth);
		[[nodiscard]] std::string read_fixed_string(
			TargetAddress a_fixedStringAddress);
		[[nodiscard]] bool begin_object(TargetAddress a_address);
		[[nodiscard]] std::string unavailable(std::string_view a_field);

		const MemoryReader& m_reader;
		std::span<const ModuleRange> m_modules;
		RuntimeProfile m_profile{ RuntimeProfile::kUnsupported };
		AnalysisBudgets m_budgets;
		AnalysisDiagnostics m_diagnostics;
		std::unordered_set<std::uint64_t> m_visited;
		std::unordered_map<std::uint64_t, std::string> m_results;
		std::unordered_map<std::uint64_t, std::string> m_fieldResults;
	};

	[[nodiscard]] RuntimeProfile runtime_profile(
		std::uint16_t a_major,
		std::uint16_t a_minor,
		std::uint16_t a_build,
		std::uint16_t a_revision) noexcept;

	[[nodiscard]] std::vector<ModuleRange> snapshot_module_ranges(
		const Capture::SnapshotOperation& a_operation);
}
