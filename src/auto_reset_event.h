// Copyright (c) 2013 Amanieu d'Antras
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifdef __linux__
# include <unistd.h>
# include <sys/syscall.h>
# include <linux/futex.h>
#elif defined(_WIN32)
# define NOMINMAX
# include <windows.h>
#else
# include <mutex>
# include <condition_variable>
#endif

namespace async {
namespace detail {

// Windows-style auto-reset event, essentially a semaphore with a max
// value of 1.
// This implementation makes one assumption:
// - Only the thread owning the event calls reset() and wait()
// - Therefore there can only be at most one thread waiting on an event
#ifdef __linux__
// Linux-specific implementation using futex
class auto_reset_event {
	// Valid values:
	// 1 = set
	// 0 = not set
	// -1 = not set and sleeping thread
	std::atomic<int> futex_val{0};

public:
	void wait()
	{
		// If futex_val goes below 0, sleep
		if (futex_val.fetch_sub(1, std::memory_order_relaxed) <= 0) {
			int ret;
			do {
				// Possible results:
				// - Success => we were woken up, so return
				// - EWOULDBLOCK => futex_val is not -1 anymore, so return
				// - EINTR => spurious wakeup, try again
				ret = syscall(SYS_futex, reinterpret_cast<int*>(&futex_val), FUTEX_WAIT_PRIVATE, -1, nullptr);
			} while (ret == -1 && errno == EINTR);
		} else
			std::atomic_thread_fence(std::memory_order_acquire);
	}

	void reset()
	{
		futex_val.store(0, std::memory_order_relaxed);
	}

	void signal()
	{
		// Increment futex_val, but don't go above 1
		int val = futex_val.load(std::memory_order_relaxed);
		do {
			if (val > 0)
				return;
		} while (!futex_val.compare_exchange_weak(val, val + 1, std::memory_order_release, std::memory_order_relaxed));

		// Wake up a sleeping thread if futex_val was negative
		if (val < 0)
			syscall(SYS_futex, reinterpret_cast<int*>(&futex_val), FUTEX_WAKE_PRIVATE, 1);
	}
};
#elif defined(_WIN32)
// Windows-specific implementation using CreateEvent
class auto_reset_event {
	HANDLE event;

public:
	auto_reset_event()
	{
		event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	}
	~auto_reset_event()
	{
		CloseHandle(event);
	}

	void wait()
	{
		WaitForSingleObject(event, INFINITE);
	}

	void reset()
	{
		ResetEvent(event);
	}

	void signal()
	{
		SetEvent(event);
	}
};
#else
// Generic implementation using std::mutex and std::condition_variable
class auto_reset_event {
	std::mutex m;
	std::condition_variable c;
	bool signaled{false};

public:
	void wait()
	{
		std::unique_lock<std::mutex> lock(m);
		while (!signaled)
			c.wait(lock);
		signaled = false;
	}

	void reset()
	{
		signaled = false;
	}

	void signal()
	{
		std::lock_guard<std::mutex> lock(m);
		signaled = true;
		c.notify_one();
	}
};
#endif

} // namespace detail
} // namespace async
