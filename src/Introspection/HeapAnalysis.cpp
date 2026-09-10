#include "Introspection/HeapAnalysis.h"

namespace Crash::Introspection::Heap
{
	std::optional<HeapInfo> analyze_heap_pointer(const void*)
	{
		// Compatibility seam only. HeapLock/HeapWalk are intentionally forbidden
		// during failure analysis; a memory-region classification cannot prove
		// heap ownership.
		return std::nullopt;
	}

	std::string format_heap_info(const HeapInfo&)
	{
		return "unavailable: heap ownership inspection is retired";
	}
}
