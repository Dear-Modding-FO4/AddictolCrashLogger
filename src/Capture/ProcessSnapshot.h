#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <DbgHelp.h>
#include <processsnapshot.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace Capture
{
	enum class ErrorCode
	{
		kUnavailable,
		kCaptureFailed,
		kQueryFailed,
		kWalkFailed,
		kInvalidAddress,
		kAddressOverflow,
		kUnreadable,
		kLifetimeExpired,
		kInvalidImage,
		kInvalidMetadata,
		kBudgetExceeded,
		kAllocationFailed,
		kPartialRead,
		kIoFailed,
		kDumpFailed,
		kReleaseFailed
	};

	struct Error
	{
		ErrorCode code{};
		DWORD systemError{};
		std::string message;
		std::size_t bytesCompleted{};
	};

	class PssApi
	{
	public:
		using CaptureSnapshot = DWORD(WINAPI*)(HANDLE, PSS_CAPTURE_FLAGS, DWORD, HPSS*);
		using FreeSnapshot = DWORD(WINAPI*)(HANDLE, HPSS);
		using QuerySnapshot = DWORD(WINAPI*)(HPSS, PSS_QUERY_INFORMATION_CLASS, void*, DWORD);
		using WalkSnapshot = DWORD(WINAPI*)(HPSS, PSS_WALK_INFORMATION_CLASS, HPSSWALK, void*, DWORD);
		using WalkMarkerCreate = DWORD(WINAPI*)(const PSS_ALLOCATOR*, HPSSWALK*);
		using WalkMarkerFree = DWORD(WINAPI*)(HPSSWALK);

		[[nodiscard]] static std::expected<PssApi, Error> load();
		[[nodiscard]] static std::expected<PssApi, Error> resolve(HMODULE a_module);

		[[nodiscard]] DWORD capture_snapshot(
			HANDLE a_process,
			PSS_CAPTURE_FLAGS a_flags,
			DWORD a_contextFlags,
			HPSS* a_snapshot) const;
		[[nodiscard]] DWORD free_snapshot(HANDLE a_process, HPSS a_snapshot) const;
		[[nodiscard]] DWORD query_snapshot(
			HPSS a_snapshot,
			PSS_QUERY_INFORMATION_CLASS a_informationClass,
			void* a_buffer,
			DWORD a_bufferLength) const;
		[[nodiscard]] DWORD walk_snapshot(
			HPSS a_snapshot,
			PSS_WALK_INFORMATION_CLASS a_informationClass,
			HPSSWALK a_marker,
			void* a_buffer,
			DWORD a_bufferLength) const;
		[[nodiscard]] DWORD create_walk_marker(HPSSWALK* a_marker) const;
		[[nodiscard]] DWORD free_walk_marker(HPSSWALK a_marker) const;

	private:
		PssApi() = default;

		CaptureSnapshot m_capture{};
		FreeSnapshot m_free{};
		QuerySnapshot m_query{};
		WalkSnapshot m_walk{};
		WalkMarkerCreate m_markerCreate{};
		WalkMarkerFree m_markerFree{};

	};

	enum class MemoryProvenance
	{
		kPrivateFrozen,
		kOwnedAtAnalysisFromSharedImage,
		kSharedImageWeak,
		kSharedMappedWeak,
		kUnknownWeak
	};

	struct MemoryRegion
	{
		std::uint64_t base{};
		std::uint64_t allocationBase{};
		std::uint64_t size{};
		DWORD allocationProtect{};
		DWORD state{};
		DWORD protect{};
		DWORD type{};
		DWORD imageSize{};
		std::wstring mappedFileName;

		[[nodiscard]] MemoryProvenance provenance() const noexcept;
	};

	struct CapturedThread
	{
		DWORD processId{};
		DWORD threadId{};
		std::uint64_t tebAddress{};
		std::uint64_t stackLimit{};
		std::uint64_t stackBase{};
		bool hasStackBounds{};
		CONTEXT context{};
		std::vector<std::byte> contextBytes;
	};

	struct CapturedSegment
	{
		std::uint64_t base{};
		std::vector<std::byte> bytes;
		MemoryProvenance sourceProvenance{ MemoryProvenance::kUnknownWeak };
	};

	struct ModuleImage
	{
		std::uint64_t base{};
		std::uint64_t size{};
		std::wstring path;
		bool pathIsNtDevice{};
		std::vector<IMAGE_SECTION_HEADER> sections;
		std::vector<RUNTIME_FUNCTION> runtimeFunctions;
		MemoryProvenance metadataProvenance{ MemoryProvenance::kUnknownWeak };
		bool metadataPartial{};
		std::string metadataStatus;

		[[nodiscard]] bool contains(std::uint64_t a_address) const noexcept;
		[[nodiscard]] const RUNTIME_FUNCTION* find_runtime_function(std::uint64_t a_address) const noexcept;
	};

	struct ReadResult
	{
		std::size_t bytesRead{};
		MemoryProvenance provenance{ MemoryProvenance::kUnknownWeak };
	};

	template <class T>
	struct ReadObjectResult
	{
		T value{};
		MemoryProvenance provenance{ MemoryProvenance::kUnknownWeak };
	};

	struct SnapshotState;
	class SnapshotOperation;

	class SnapshotReader
	{
	public:
		SnapshotReader() = default;

		[[nodiscard]] std::expected<ReadResult, Error> read(
			std::uint64_t a_address,
			std::span<std::byte> a_destination) const;

		template <class T>
			requires(
				std::is_trivially_copyable_v<T> &&
				!std::is_polymorphic_v<T> &&
				std::is_standard_layout_v<T>)
		[[nodiscard]] std::expected<ReadObjectResult<T>, Error> read_object(std::uint64_t a_address) const
		{
			T value{};
			auto result = read(
				a_address,
				std::span<std::byte>{ reinterpret_cast<std::byte*>(std::addressof(value)), sizeof(T) });
			if (!result)
				return std::unexpected(result.error());
			return ReadObjectResult<T>{
				.value = value,
				.provenance = result->provenance
			};
		}

		[[nodiscard]] std::expected<SnapshotOperation, Error> begin_operation(
			std::size_t a_ownedByteBudget = 8u * 1024u * 1024u) const;

	private:
		explicit SnapshotReader(std::weak_ptr<SnapshotState> a_state) :
			m_state(std::move(a_state))
		{}

		std::weak_ptr<SnapshotState> m_state;

		friend class ProcessSnapshot;
		friend class SnapshotStackWalker;
	};

	class SnapshotOperation
	{
	public:
		SnapshotOperation() = default;
		~SnapshotOperation();
		SnapshotOperation(SnapshotOperation&&) noexcept;
		SnapshotOperation& operator=(SnapshotOperation&&) noexcept;
		SnapshotOperation(const SnapshotOperation&) = delete;
		SnapshotOperation& operator=(const SnapshotOperation&) = delete;

		[[nodiscard]] std::expected<ReadResult, Error> read(
			std::uint64_t a_address,
			std::span<std::byte> a_destination) const;
		[[nodiscard]] std::expected<ReadResult, Error> read_owned_page_cached(
			std::uint64_t a_address,
			std::span<std::byte> a_destination);
		[[nodiscard]] const std::vector<MemoryRegion>& regions() const noexcept;
		[[nodiscard]] const std::vector<CapturedThread>& threads() const noexcept;
		[[nodiscard]] const std::vector<ModuleImage>& modules() const noexcept;
		[[nodiscard]] std::size_t owned_bytes() const noexcept;
		[[nodiscard]] std::size_t owned_byte_budget() const noexcept;

	private:
		struct Cache;
		SnapshotOperation(std::shared_ptr<SnapshotState> a_state, std::size_t a_ownedByteBudget);

		std::shared_ptr<SnapshotState> m_state;
		std::unique_ptr<Cache> m_cache;

		friend class SnapshotReader;
		friend class SnapshotStackWalker;
	};

	struct CaptureOptions
	{
		bool captureHandleData{};
	};

	struct CaptureDiagnostics
	{
		std::uint32_t activeSnapshots{};
		std::uint32_t activeWalkMarkers{};
		std::uint32_t snapshotReleaseFailures{};
		std::uint32_t walkMarkerReleaseFailures{};
		DWORD lastSnapshotReleaseError{};
		DWORD lastWalkMarkerReleaseError{};
	};

	class ProcessSnapshot
	{
	public:
		ProcessSnapshot() = default;
		~ProcessSnapshot() = default;
		ProcessSnapshot(ProcessSnapshot&&) noexcept = default;
		ProcessSnapshot& operator=(ProcessSnapshot&&) noexcept = default;
		ProcessSnapshot(const ProcessSnapshot&) = delete;
		ProcessSnapshot& operator=(const ProcessSnapshot&) = delete;

		[[nodiscard]] static std::expected<ProcessSnapshot, Error> capture(
			const PssApi& a_api,
			const CaptureOptions& a_options = {});

		[[nodiscard]] explicit operator bool() const noexcept;
		[[nodiscard]] SnapshotReader reader() const noexcept;
		[[nodiscard]] const std::vector<MemoryRegion>& regions() const noexcept;
		[[nodiscard]] const std::vector<CapturedThread>& threads() const noexcept;
		[[nodiscard]] const std::vector<ModuleImage>& modules() const noexcept;
		[[nodiscard]] DWORD process_id() const noexcept;
		[[nodiscard]] bool captured_handle_data() const noexcept;

		[[nodiscard]] std::expected<void, Error> write_minidump(
			const std::filesystem::path& a_path,
			MINIDUMP_TYPE a_type) const;

		void reset() noexcept;

	private:
		explicit ProcessSnapshot(std::shared_ptr<SnapshotState> a_state) :
			m_state(std::move(a_state))
		{}

		std::shared_ptr<SnapshotState> m_state;

		friend class SnapshotStackWalker;
	};

	[[nodiscard]] CaptureDiagnostics capture_diagnostics() noexcept;
}
