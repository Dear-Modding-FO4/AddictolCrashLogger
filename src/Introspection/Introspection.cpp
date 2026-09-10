#include "Introspection/Introspection.h"

#include "Modules/ModuleHandler.h"

namespace Crash::Introspection
{
	namespace
	{
		thread_local std::unordered_set<std::uint64_t> s_recentObjects;

		[[nodiscard]] std::vector<ReadOnly::ModuleRange> module_ranges(
			std::span<const module_pointer> a_modules)
		{
			std::vector<ReadOnly::ModuleRange> ranges;
			ranges.reserve(a_modules.size());
			for (const auto& module : a_modules)
				ranges.push_back(ReadOnly::ModuleRange{
					.base = module->address(),
					.size = module->size(),
					.name = std::string(module->name()),
					.path = std::string(module->path())
				});
			return ranges;
		}
	}

	const Modules::Module* get_module_for_pointer(
		const void* a_ptr,
		std::span<const module_pointer> a_modules) noexcept
	{
		for (const auto& module : a_modules)
			if (module && module->in_range(a_ptr))
				return module.get();
		return nullptr;
	}

	void reset_analysis_state() noexcept
	{
		s_recentObjects.clear();
	}

	std::vector<std::string> analyze_data(
		std::span<const std::size_t> a_data,
		std::span<const module_pointer> a_modules,
		std::function<std::string(size_t)> a_label_generator)
	{
		const auto ranges = module_ranges(a_modules);
		const auto version =
			REX::FModule::GetExecutingModule().GetFileVersion();
		ReadOnly::LiveMemoryReader reader;
		ReadOnly::AnalysisSession session(
			reader,
			ranges,
			ReadOnly::runtime_profile(
				version[0],
				version[1],
				version[2],
				version[3]));
		return analyze_data(
			a_data,
			session,
			std::move(a_label_generator));
	}

	std::vector<std::string> analyze_data(
		std::span<const std::size_t> a_data,
		ReadOnly::AnalysisSession& a_session,
		std::function<std::string(size_t)> a_label_generator)
	{
		std::vector<std::uint64_t> addresses(a_data.begin(), a_data.end());
		std::vector<std::string> labels;
		if (a_label_generator)
		{
			labels.reserve(a_data.size());
			for (std::size_t index = 0; index < a_data.size(); ++index)
				labels.push_back(a_label_generator(index));
		}
		auto results = a_session.analyze(addresses, labels);
		for (std::size_t index = 0; index < results.size(); ++index)
			if (!results[index].empty() &&
				!results[index].starts_with("(void*)") &&
				results[index].find("*) 0x") != std::string::npos)
				s_recentObjects.insert(addresses[index]);
		return results;
	}

	void backfill_void_pointers(
		std::vector<std::string>& a_results,
		std::span<const std::size_t> a_addresses)
	{
		assert(a_results.size() == a_addresses.size());
		// A single explicit AnalysisSession owns deduplication. Cross-session
		// backfill is intentionally retired to avoid stale address reuse.
	}

	bool was_introspected(const void* a_ptr) noexcept
	{
		return s_recentObjects.contains(
			reinterpret_cast<std::uint64_t>(a_ptr));
	}
}
