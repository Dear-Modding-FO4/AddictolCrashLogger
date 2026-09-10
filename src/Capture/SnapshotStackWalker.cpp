#include "Capture/SnapshotStackWalker.h"

#include "Capture/DbgHelpGate.h"

#include <limits>
#include <set>
#include <utility>

namespace Capture
{
	namespace
	{
		struct CallbackContext
		{
			SnapshotOperation* operation{};
			HANDLE processToken{};
			HANDLE threadToken{};
			WalkInstrumentation instrumentation;
			bool missingFunctionTable{};
		};

		thread_local CallbackContext* s_callbackContext{};

		[[nodiscard]] const ModuleImage* find_module(
			const std::vector<ModuleImage>& a_modules,
			std::uint64_t a_address) noexcept
		{
			const auto it = std::upper_bound(
				a_modules.begin(),
				a_modules.end(),
				a_address,
				[](std::uint64_t a_value, const ModuleImage& a_module) {
					return a_value < a_module.base;
				});
			if (it == a_modules.begin())
				return nullptr;
			const auto& module = *std::prev(it);
			return module.contains(a_address) ? std::addressof(module) : nullptr;
		}

		BOOL read_snapshot_memory(
			CallbackContext& a_context,
			DWORD64 a_address,
			PVOID a_buffer,
			DWORD a_size,
			LPDWORD a_bytesRead)
		{
			if (a_bytesRead)
				*a_bytesRead = 0;
			if (!a_buffer || !a_bytesRead)
				return FALSE;
			if (a_size == 0)
				return TRUE;

			auto destination = std::span<std::byte>{
				static_cast<std::byte*>(a_buffer),
				static_cast<std::size_t>(a_size)
			};
			if (find_module(a_context.operation->modules(), a_address))
			{
				auto read = a_context.operation->read_owned_page_cached(a_address, destination);
				if (!read)
				{
					++a_context.instrumentation.failedMemoryReads;
					a_context.instrumentation.lastFailedAddress = a_address;
					a_context.instrumentation.lastFailedSize = a_size;
					a_context.instrumentation.lastReadError = read.error().code;
					return FALSE;
				}
				++a_context.instrumentation.localMetadataReads;
				if (read->provenance == MemoryProvenance::kOwnedAtAnalysisFromSharedImage)
					++a_context.instrumentation.weakMetadataReads;
				*a_bytesRead = a_size;
				return TRUE;
			}

			auto read = a_context.operation->read(a_address, destination);
			if (!read)
			{
				++a_context.instrumentation.failedMemoryReads;
				a_context.instrumentation.lastFailedAddress = a_address;
				a_context.instrumentation.lastFailedSize = a_size;
				a_context.instrumentation.lastReadError = read.error().code;
				return FALSE;
			}
			if (read->provenance != MemoryProvenance::kPrivateFrozen)
			{
				++a_context.instrumentation.rejectedWeakReads;
				a_context.instrumentation.lastFailedAddress = a_address;
				a_context.instrumentation.lastFailedSize = a_size;
				return FALSE;
			}

			++a_context.instrumentation.cloneMemoryReads;
			*a_bytesRead = a_size;
			return TRUE;
		}

		BOOL CALLBACK read_memory_callback(
			HANDLE a_process,
			DWORD64 a_address,
			PVOID a_buffer,
			DWORD a_size,
			LPDWORD a_bytesRead)
		{
			auto* context = s_callbackContext;
			if (!context || a_process != context->processToken)
			{
				if (context)
					++context->instrumentation.sourceDomainViolations;
				return FALSE;
			}
			return read_snapshot_memory(*context, a_address, a_buffer, a_size, a_bytesRead);
		}

		PVOID CALLBACK function_table_callback(HANDLE a_process, DWORD64 a_address)
		{
			auto* context = s_callbackContext;
			if (!context || a_process != context->processToken)
			{
				if (context)
					++context->instrumentation.sourceDomainViolations;
				return nullptr;
			}

			++context->instrumentation.functionTableLookups;
			const auto* module = find_module(context->operation->modules(), a_address);
			if (!module)
			{
				context->missingFunctionTable = true;
				return nullptr;
			}
			const auto* function = module->find_runtime_function(a_address);
			if (!function)
				context->missingFunctionTable = true;
			else if (module->metadataProvenance !=
				MemoryProvenance::kPrivateFrozen)
				++context->instrumentation.weakMetadataReads;
			return const_cast<RUNTIME_FUNCTION*>(function);
		}

		DWORD64 CALLBACK module_base_callback(HANDLE a_process, DWORD64 a_address)
		{
			auto* context = s_callbackContext;
			if (!context || a_process != context->processToken)
			{
				if (context)
					++context->instrumentation.sourceDomainViolations;
				return 0;
			}
			++context->instrumentation.moduleBaseLookups;
			const auto* module = find_module(context->operation->modules(), a_address);
			return module ? module->base : 0;
		}

		class CallbackScope
		{
		public:
			explicit CallbackScope(CallbackContext& a_context) :
				m_previous(std::exchange(s_callbackContext, std::addressof(a_context)))
			{}

			~CallbackScope()
			{
				s_callbackContext = m_previous;
			}

		private:
			CallbackContext* m_previous{};
		};
	}

	StackWalkResult SnapshotStackWalker::walk(
		const ProcessSnapshot& a_snapshot,
		const CapturedThread& a_thread,
		std::size_t a_maxFrames) const
	{
		StackWalkResult result;
		if (!a_snapshot || a_maxFrames == 0 || !a_thread.hasStackBounds)
		{
			result.status = WalkStatus::kInvalidInput;
			result.detail = "snapshot, frame limit, or captured TEB stack bounds are invalid";
			return result;
		}
		if (a_thread.context.Rip == 0 ||
			a_thread.context.Rsp < a_thread.stackLimit ||
			a_thread.context.Rsp >= a_thread.stackBase)
		{
			result.status = WalkStatus::kInvalidInput;
			result.detail = "captured instruction or stack pointer is outside valid bounds";
			return result;
		}

		auto operationResult = a_snapshot.reader().begin_operation();
		if (!operationResult)
		{
			result.status = WalkStatus::kUnsupported;
			result.detail = operationResult.error().message;
			return result;
		}
		auto operation = std::move(*operationResult);
		CallbackContext callback{
			.operation = std::addressof(operation),
			.processToken = reinterpret_cast<HANDLE>(
				reinterpret_cast<std::uintptr_t>(this) | std::uintptr_t{ 1 }),
			.threadToken = reinterpret_cast<HANDLE>(
				(reinterpret_cast<std::uintptr_t>(this) ^ a_thread.threadId) | std::uintptr_t{ 1 })
		};
		CONTEXT context = a_thread.context;
		STACKFRAME64 nativeFrame{};
		nativeFrame.AddrPC.Offset = context.Rip;
		nativeFrame.AddrPC.Mode = AddrModeFlat;
		// On x64, StackWalk64's frame cursor is the stack pointer. RBP is not a
		// reliable frame pointer in optimized code.
		nativeFrame.AddrFrame.Offset = context.Rsp;
		nativeFrame.AddrFrame.Mode = AddrModeFlat;
		nativeFrame.AddrStack.Offset = context.Rsp;
		nativeFrame.AddrStack.Mode = AddrModeFlat;

		result.frames.push_back(StackFrame{
			.programCounter = context.Rip,
			.stackPointer = context.Rsp,
			.framePointer = context.Rbp
		});
		std::set<std::pair<std::uint64_t, std::uint64_t>> visited;
		visited.emplace(context.Rip, context.Rsp);

		bool completed = false;
		bool failed = false;
		bool cycle = false;
		bool boundsFailure = false;
		bool skippedInitialEcho = false;
		{
			const auto gate = DbgHelpGate::lock();
			CallbackScope callbackScope(callback);
			while (result.frames.size() < a_maxFrames)
			{
				::SetLastError(ERROR_SUCCESS);
				const auto walked = ::StackWalk64(
					IMAGE_FILE_MACHINE_AMD64,
					callback.processToken,
					callback.threadToken,
					std::addressof(nativeFrame),
					std::addressof(context),
					read_memory_callback,
					function_table_callback,
					module_base_callback,
					nullptr);
				if (!walked)
				{
					callback.instrumentation.stackWalkError = ::GetLastError();
					callback.instrumentation.failedFrameProgramCounter = nativeFrame.AddrPC.Offset;
					callback.instrumentation.failedFrameStackPointer = nativeFrame.AddrStack.Offset;
					failed = true;
					break;
				}

				const auto pc = nativeFrame.AddrPC.Offset;
				// Current DbgHelp builds can leave AddrStack unset for AMD64
				// custom-callback walks while still advancing the context copy.
				const auto sp = nativeFrame.AddrStack.Offset != 0 ?
					nativeFrame.AddrStack.Offset :
					context.Rsp;
				if (pc == 0)
				{
					completed = true;
					break;
				}
				if (sp < a_thread.stackLimit || sp >= a_thread.stackBase)
				{
					boundsFailure = true;
					break;
				}
				if (!skippedInitialEcho &&
					pc == result.frames.front().programCounter &&
					sp == result.frames.front().stackPointer)
				{
					// StackWalk64 reports the seeded AMD64 frame on its first
					// successful call before advancing to callers.
					skippedInitialEcho = true;
					continue;
				}
				if (!visited.emplace(pc, sp).second)
				{
					cycle = true;
					break;
				}

				result.frames.push_back(StackFrame{
					.programCounter = pc,
					.stackPointer = sp,
					.framePointer = nativeFrame.AddrFrame.Offset != 0 ?
						nativeFrame.AddrFrame.Offset :
						context.Rbp
				});
			}
		}

		callback.instrumentation.ownedMetadataBytes = operation.owned_bytes();
		result.instrumentation = callback.instrumentation;
		if (completed)
		{
			result.status = WalkStatus::kComplete;
			result.detail = "walk reached the end of the captured stack";
		}
		else if (result.frames.size() == a_maxFrames)
		{
			result.status = WalkStatus::kFrameLimit;
			result.detail = "walk stopped at the requested frame limit";
		}
		else if (cycle)
		{
			result.status = WalkStatus::kPartial;
			result.detail = "walk stopped after detecting a repeated (PC, SP) pair";
		}
		else if (boundsFailure)
		{
			result.status = WalkStatus::kPartial;
			result.detail = "walk left the captured TEB stack bounds";
		}
		else if (failed && (callback.missingFunctionTable || result.frames.size() == 1))
		{
			result.status = WalkStatus::kUnsupported;
			result.detail = callback.missingFunctionTable ?
				"captured module unwind metadata was unavailable" :
				"StackWalk64 rejected the initial frame despite successful snapshot-only callbacks";
		}
		else
		{
			result.status = WalkStatus::kPartial;
			result.detail = "StackWalk64 could not produce another snapshot-backed frame";
		}
		return result;
	}
}
