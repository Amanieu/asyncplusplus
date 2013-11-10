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

#ifndef ASYNCXX_H_
# error "Do not include this header directly, include <async++.h> instead."
#endif

namespace async {
namespace detail {

// Spinlock with same interface as std::mutex
class spinlock {
	std::atomic<bool> locked;

	// Non-copyable and non-movable
	spinlock(const spinlock&);
	spinlock(spinlock&&);
	spinlock& operator=(const spinlock&);
	spinlock& operator=(spinlock&&);

public:
	spinlock()
		: locked(false) {}

	void lock()
	{
		while (!try_lock()) {
			// If direct locking fails then spin using load() only
			// before trying again. This saves bus traffic since the
			// spinlock is already in the cache.
			while (locked.load(std::memory_order_relaxed))
				spin_pause();
		}
	}

	bool try_lock()
	{
		bool expected = false;
		return locked.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed);
	}

	void unlock()
	{
		locked.store(false, std::memory_order_release);
	}

	// Low-level access to atomic variable
	std::atomic<bool>& get_atomic()
	{
		return locked;
	}

	// Pause for use in spinloops. On hyperthreaded CPUs, this yields to the other
	// hardware thread. Otherwise it is simply a no-op.
	static void spin_pause()
	{
#if defined(__SSE__) || _M_IX86_FP > 0
		_mm_pause();
#endif
	}
};

} // namespace detail
} // namespace async
