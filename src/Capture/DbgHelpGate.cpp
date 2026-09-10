#include "Capture/DbgHelpGate.h"

namespace Capture
{
	std::mutex& DbgHelpGate::mutex()
	{
		static std::mutex instance;
		return instance;
	}

	std::unique_lock<std::mutex> DbgHelpGate::lock()
	{
		return std::unique_lock{ mutex() };
	}

	std::unique_lock<std::mutex> DbgHelpGate::try_lock()
	{
		return std::unique_lock{ mutex(), std::try_to_lock };
	}
}
