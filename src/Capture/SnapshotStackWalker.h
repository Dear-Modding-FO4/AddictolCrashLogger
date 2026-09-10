#pragma once

#include "Capture/ProcessSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Capture
{
	enum class WalkStatus
	{
		kComplete,
		kFrameLimit,
		kPartial,
		kUnsupported,
		kInvalidInput
	};

	struct StackFrame
	{
		std::uint64_t programCounter{};
		std::uint64_t stackPointer{};
		std::uint64_t framePointer{};
	};

	struct WalkInstrumentation
	{
		std::size_t cloneMemoryReads{};
		std::size_t localMetadataReads{};
		std::size_t weakMetadataReads{};
		std::size_t ownedMetadataBytes{};
		std::size_t functionTableLookups{};
		std::size_t moduleBaseLookups{};
		std::size_t rejectedWeakReads{};
		std::size_t failedMemoryReads{};
		std::size_t sourceDomainViolations{};
		std::uint64_t lastFailedAddress{};
		std::uint32_t lastFailedSize{};
		ErrorCode lastReadError{ ErrorCode::kUnreadable };
		DWORD stackWalkError{};
		std::uint64_t failedFrameProgramCounter{};
		std::uint64_t failedFrameStackPointer{};
	};

	struct StackWalkResult
	{
		WalkStatus status{ WalkStatus::kUnsupported };
		std::string detail;
		std::vector<StackFrame> frames;
		WalkInstrumentation instrumentation;
	};

	class SnapshotStackWalker
	{
	public:
		[[nodiscard]] StackWalkResult walk(
			const ProcessSnapshot& a_snapshot,
			const CapturedThread& a_thread,
			std::size_t a_maxFrames) const;
	};
}
