#pragma once

#include "Capture/SavedContextWalker.h"
#include <vector>

struct _EXCEPTION_RECORD;
struct _CONTEXT;

namespace Crash
{
	namespace Modules
	{
		class Module;
	}
	namespace PDB
	{
		class SymbolResolver;
	}

	class Callstack
	{
	public:
		// A supplied fault context retains unwind status and per-frame provenance.
		Callstack(const ::_EXCEPTION_RECORD& a_except, const ::_CONTEXT* a_context = nullptr);

		void print(
			spdlog::logger& a_log,
			std::span<const std::unique_ptr<Modules::Module>> a_modules,
			PDB::SymbolResolver& a_symbols) const;

		// Get the throw location for C++ exceptions (frame after KERNELBASE/VCRUNTIME)
		// Returns empty string if not found
		[[nodiscard]] std::string get_throw_location(
			std::span<const std::unique_ptr<Modules::Module>> a_modules,
			PDB::SymbolResolver& a_symbols) const;

		[[nodiscard]] std::vector<std::string> get_frame_info_strings(
			std::span<const std::unique_ptr<Modules::Module>> a_modules,
			PDB::SymbolResolver& a_symbols,
			std::size_t a_max_frames = 50) const;

		[[nodiscard]] std::vector<const void*> get_frame_addresses(std::size_t a_max_frames = 500) const;
		[[nodiscard]] std::span<const Capture::SavedContextFrame> get_saved_frames() const noexcept;
		void print_capture_status(spdlog::logger& a_log) const;

	private:
		[[nodiscard]] static std::string get_size_string(std::size_t a_size);

		[[nodiscard]] std::string get_format(std::size_t a_nameWidth) const;

		void print_probable_callstack(
			spdlog::logger& a_log,
			std::span<const std::unique_ptr<Modules::Module>> a_modules,
			PDB::SymbolResolver& a_symbols) const;

		void print_raw_callstack(spdlog::logger& a_log) const;

		std::vector<boost::stacktrace::frame> _capturedFrames;
		std::span<const boost::stacktrace::frame> _frames;
		Capture::SavedContextWalk _savedWalk;
		bool _contextProvided{};
		bool _usingSavedFrames{};
	};

	[[nodiscard]] std::filesystem::path GetF4SELogDirectory();
	[[nodiscard]] std::filesystem::path GetStartupLogPath();
	[[nodiscard]] std::filesystem::path GetCrashLogDirectory();

	bool Install();
}
