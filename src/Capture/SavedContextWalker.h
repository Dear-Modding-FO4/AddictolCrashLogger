#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Capture
{
	enum class FrameProvenance
	{
		kContextInstruction,
		kUnwindMetadata,
		kLeafAssumption,
		kNullReseed,
		kStackScanSlot
	};

	struct SavedContextFrame
	{
		std::uint64_t programCounter{};
		std::uint64_t stackPointer{};
		FrameProvenance provenance{ FrameProvenance::kContextInstruction };
		std::uint64_t sourceSlot{};
	};

	enum class SavedWalkStatus
	{
		kComplete,
		kFrameLimit,
		kPartialInvalidContext,
		kPartialUnreadableStack,
		kPartialInvalidMetadata,
		kPartialLoop,
		kPartialBounds
	};

	struct SavedContextWalk
	{
		SavedWalkStatus status{ SavedWalkStatus::kPartialInvalidContext };
		std::string detail;
		std::vector<SavedContextFrame> frames;
		bool nullReseedAttempted{};
		bool nullReseeded{};
	};

	[[nodiscard]] std::string_view frame_provenance_name(FrameProvenance a_provenance) noexcept;
	[[nodiscard]] std::string_view saved_walk_status_name(SavedWalkStatus a_status) noexcept;

	[[nodiscard]] SavedContextWalk walk_saved_context(
		const CONTEXT& a_savedContext,
		std::uint64_t a_stackLimit,
		std::uint64_t a_stackBase,
		std::size_t a_maxFrames = 128,
		bool a_allowNearNullReseed = false);

	[[nodiscard]] bool current_thread_stack_bounds(
		std::uint64_t& a_stackLimit,
		std::uint64_t& a_stackBase) noexcept;
}
