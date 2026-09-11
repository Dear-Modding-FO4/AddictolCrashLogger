#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace CrashUI
{
	enum class DiagnosticTone { kNormal, kGood, kWarning, kError };
	enum class ProbePhase { kIdle, kQueued, kRunning, kFinished, kCancelled };

	struct SymbolProbeRequest
	{
		std::string gameVersion;
		std::string loggerVersion;
		std::string activeCacheDirectory;
		bool loggerEnabled{};
	};

	struct DiagnosticField
	{
		std::string label;
		std::string value;
		DiagnosticTone tone{ DiagnosticTone::kNormal };
	};

	struct SymbolProbeSnapshot
	{
		std::uint64_t generation{};
		ProbePhase phase{ ProbePhase::kIdle };
		bool workerBusy{};
		bool pending{};
		SymbolProbeRequest request;
		std::string observedAt;
		std::vector<DiagnosticField> fields;
		std::string compactSummary;
		std::string error;
	};

	class SymbolDiagnostics
	{
	public:
		static SymbolDiagnostics& GetSingleton();
		void RequestProbe(SymbolProbeRequest a_request);
		void CancelProbe();
		[[nodiscard]] std::shared_ptr<const SymbolProbeSnapshot> Snapshot() const;

	private:
		SymbolDiagnostics();
		void Worker();
		[[nodiscard]] bool IsCurrent(std::uint64_t a_generation) const;

		struct Request
		{
			std::uint64_t generation{};
			SymbolProbeRequest values;
		};
		mutable std::mutex m_mutex;
		std::condition_variable m_condition;
		std::optional<Request> m_pending;
		std::shared_ptr<const SymbolProbeSnapshot> m_snapshot;
		std::shared_ptr<const SymbolProbeSnapshot> m_allocationFailure;
		std::shared_ptr<const SymbolProbeSnapshot> m_cancelledIdle;
		std::uint64_t m_generation{};
		bool m_busy{};
	};
}
