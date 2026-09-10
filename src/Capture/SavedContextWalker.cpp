#include "Capture/SavedContextWalker.h"

#include <limits>
#include <set>

namespace Capture
{
	namespace
	{
		[[nodiscard]] bool checked_add(
			std::uint64_t a_left,
			std::uint64_t a_right,
			std::uint64_t& a_result) noexcept
		{
			if (a_right > std::numeric_limits<std::uint64_t>::max() - a_left)
				return false;
			a_result = a_left + a_right;
			return true;
		}

		[[nodiscard]] bool read_qword(
			std::uint64_t a_address,
			std::uint64_t& a_value) noexcept
		{
			SIZE_T read{};
			return ::ReadProcessMemory(
					   ::GetCurrentProcess(),
					   reinterpret_cast<const void*>(a_address),
					   std::addressof(a_value),
					   sizeof(a_value),
					   std::addressof(read)) &&
			       read == sizeof(a_value);
		}

		[[nodiscard]] bool inside_stack(
			std::uint64_t a_address,
			std::uint64_t a_stackLimit,
			std::uint64_t a_stackBase,
			std::size_t a_size = 1) noexcept
		{
			std::uint64_t end{};
			return a_stackLimit < a_stackBase &&
			       a_address >= a_stackLimit &&
			       checked_add(a_address, a_size, end) &&
			       end <= a_stackBase;
		}

		[[nodiscard]] bool plausible_code_address(std::uint64_t a_address) noexcept
		{
			if (a_address < 0x10000)
				return false;
			MEMORY_BASIC_INFORMATION information{};
			if (::VirtualQuery(
					reinterpret_cast<const void*>(a_address),
					std::addressof(information),
					sizeof(information)) != sizeof(information) ||
				information.State != MEM_COMMIT ||
				(information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
				return false;
			switch (information.Protect & 0xFF)
			{
			case PAGE_EXECUTE:
			case PAGE_EXECUTE_READ:
			case PAGE_EXECUTE_READWRITE:
			case PAGE_EXECUTE_WRITECOPY:
				return true;
			default:
				return false;
			}
		}

		[[nodiscard]] bool virtual_unwind_once(
			CONTEXT& a_context,
			bool& a_usedMetadata,
			bool& a_leafAssumption) noexcept
		{
			a_usedMetadata = false;
			a_leafAssumption = false;
			__try
			{
				DWORD64 imageBase{};
				const auto* function = ::RtlLookupFunctionEntry(
					a_context.Rip,
					std::addressof(imageBase),
					nullptr);
				if (function)
				{
					if (function->BeginAddress >= function->EndAddress ||
						a_context.Rip < imageBase + function->BeginAddress ||
						a_context.Rip >= imageBase + function->EndAddress)
						return false;
					PVOID handlerData{};
					DWORD64 establisherFrame{};
					KNONVOLATILE_CONTEXT_POINTERS pointers{};
					::RtlVirtualUnwind(
						UNW_FLAG_NHANDLER,
						imageBase,
						a_context.Rip,
						const_cast<PRUNTIME_FUNCTION>(function),
						std::addressof(a_context),
						std::addressof(handlerData),
						std::addressof(establisherFrame),
						std::addressof(pointers));
					a_usedMetadata = true;
					return true;
				}

				std::uint64_t returnAddress{};
				if (!read_qword(a_context.Rsp, returnAddress))
					return false;
				a_context.Rsp += sizeof(std::uint64_t);
				a_context.Rip = returnAddress;
				a_leafAssumption = true;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}
	}

	SavedContextWalk walk_saved_context(
		const CONTEXT& a_savedContext,
		std::uint64_t a_stackLimit,
		std::uint64_t a_stackBase,
		std::size_t a_maxFrames,
		bool a_allowNearNullReseed)
	{
		SavedContextWalk result;
		if (a_maxFrames == 0 ||
			a_savedContext.Rip == 0 ||
			!inside_stack(
				a_savedContext.Rsp,
				a_stackLimit,
				a_stackBase,
				sizeof(std::uint64_t)))
		{
			result.detail = "saved RIP/RSP or captured TEB stack bounds are invalid";
			return result;
		}

		CONTEXT cursor = a_savedContext;
		if (a_allowNearNullReseed && cursor.Rip < 0x10000)
		{
			result.nullReseedAttempted = true;
			constexpr std::size_t kMaximumReseedSlots = 32;
			for (std::size_t slot = 0; slot < kMaximumReseedSlots; ++slot)
			{
				std::uint64_t address{};
				const auto slotAddress = cursor.Rsp + slot * sizeof(std::uint64_t);
				if (!inside_stack(slotAddress, a_stackLimit, a_stackBase, sizeof(address)) ||
					!read_qword(slotAddress, address))
					break;
				if (plausible_code_address(address))
				{
					cursor.Rip = address;
					cursor.Rsp = slotAddress + sizeof(std::uint64_t);
					result.frames.push_back(SavedContextFrame{
						.programCounter = address,
						.stackPointer = cursor.Rsp,
						.provenance = FrameProvenance::kNullReseed,
						.sourceSlot = slotAddress
					});
					result.nullReseeded = true;
					break;
				}
			}
		}
		else
		{
			result.frames.push_back(SavedContextFrame{
				.programCounter = cursor.Rip,
				.stackPointer = cursor.Rsp,
				.provenance = FrameProvenance::kContextInstruction
			});
		}

		if (result.frames.empty())
		{
			result.status = SavedWalkStatus::kPartialUnreadableStack;
			result.detail = "near-null execute context had no verified stack reseed candidate";
			return result;
		}

		std::set<std::pair<std::uint64_t, std::uint64_t>> visited;
		visited.emplace(cursor.Rip, cursor.Rsp);
		while (result.frames.size() < a_maxFrames)
		{
			const auto previousSp = cursor.Rsp;
			bool metadata{};
			bool leaf{};
			if (!virtual_unwind_once(cursor, metadata, leaf))
			{
				result.status = SavedWalkStatus::kPartialInvalidMetadata;
				result.detail = "RtlVirtualUnwind or bounded leaf read failed";
				return result;
			}
			if (cursor.Rip == 0)
			{
				result.status = SavedWalkStatus::kComplete;
				result.detail = "saved-context unwind reached a null return address";
				return result;
			}
			if (cursor.Rsp <= previousSp)
			{
				result.status = SavedWalkStatus::kPartialBounds;
				result.detail = "saved-context unwind did not advance RSP monotonically";
				return result;
			}
			if (!inside_stack(cursor.Rsp, a_stackLimit, a_stackBase))
			{
				result.status = SavedWalkStatus::kPartialBounds;
				result.detail = "saved-context unwind left captured TEB stack bounds";
				return result;
			}
			if (!visited.emplace(cursor.Rip, cursor.Rsp).second)
			{
				result.status = SavedWalkStatus::kPartialLoop;
				result.detail = "saved-context unwind repeated a (PC, SP) pair";
				return result;
			}
			result.frames.push_back(SavedContextFrame{
				.programCounter = cursor.Rip,
				.stackPointer = cursor.Rsp,
				.provenance = metadata ?
					FrameProvenance::kUnwindMetadata :
					FrameProvenance::kLeafAssumption
			});
		}

		result.status = SavedWalkStatus::kFrameLimit;
		result.detail = "saved-context unwind reached the requested frame limit";
		return result;
	}

	bool current_thread_stack_bounds(
		std::uint64_t& a_stackLimit,
		std::uint64_t& a_stackBase) noexcept
	{
		a_stackLimit = 0;
		a_stackBase = 0;
		__try
		{
			const auto* tib = reinterpret_cast<const NT_TIB*>(::NtCurrentTeb());
			a_stackLimit = reinterpret_cast<std::uint64_t>(tib->StackLimit);
			a_stackBase = reinterpret_cast<std::uint64_t>(tib->StackBase);
			return a_stackLimit < a_stackBase;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
}
