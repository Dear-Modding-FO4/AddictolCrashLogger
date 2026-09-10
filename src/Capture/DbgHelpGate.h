#pragma once

#include <mutex>

namespace Capture
{
	class DbgHelpGate
	{
	public:
		[[nodiscard]] static std::unique_lock<std::mutex> lock();
		[[nodiscard]] static std::unique_lock<std::mutex> try_lock();

	private:
		[[nodiscard]] static std::mutex& mutex();
	};
}
