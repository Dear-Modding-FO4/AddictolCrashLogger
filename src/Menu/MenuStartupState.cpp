#include "MenuStartupState.h"

#include <mutex>

namespace CrashUI
{
	namespace
	{
		std::mutex s_mutex;
		std::shared_ptr<const StartupSnapshot> s_snapshot =
			std::make_shared<const StartupSnapshot>();
	}

	void PublishStartupSnapshot(StartupSnapshot a_snapshot)
	{
		const std::scoped_lock lock{ s_mutex };
		s_snapshot = std::make_shared<const StartupSnapshot>(std::move(a_snapshot));
	}

	std::shared_ptr<const StartupSnapshot> GetStartupSnapshot()
	{
		const std::scoped_lock lock{ s_mutex };
		return s_snapshot;
	}
}
