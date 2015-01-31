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

#include "common.h"

namespace async {
namespace detail {

void* aligned_alloc(std::size_t size, std::size_t align)
{
#ifdef _WIN32
	void* ptr = _aligned_malloc(size, align);
	if (!ptr)
		LIBASYNC_THROW(std::bad_alloc());
	return ptr;
#else
	void* result;
	if (posix_memalign(&result, align, size))
		LIBASYNC_THROW(std::bad_alloc());
	else
		return result;
#endif
}

void aligned_free(void* addr)
{
#ifdef _WIN32
	_aligned_free(addr);
#else
	free(addr);
#endif
}

// Wait for a task to complete (for threads outside thread pool)
static void generic_wait_handler(task_wait_handle wait_task)
{
	// Create an event to wait on
	task_wait_event thread_event;

	// Create a continuation for the task we are waiting for
	wait_task.on_finish([&thread_event] {
		// Just signal the thread event
		thread_event.signal(EVENT_TASK_FINISHED);
	});

	// Wait for the event to be set
	thread_event.wait();
}

// Wait handler function, per-thread, defaults to generic version
static thread_local wait_handler thread_wait_handler = generic_wait_handler;

// Wait for a task to complete
void wait_for_task(task_base* wait_task)
{
	// Dispatch to the current thread's wait handler
	thread_wait_handler(task_wait_handle(wait_task));
}

// Singleton wrapper class
#if defined(__GNUC__) && !defined(__INTEL_COMPILER)
// C++11 guarantees thread safety for static initialization
template<typename T>
class singleton {
public:
	static T& get_instance()
	{
		static T instance;
		return instance;
	}
};
#else
// Intel and MSVC don't support thread-safe static initialization, so emulate it
template<typename T>
class singleton {
	std::mutex lock;
	std::atomic<bool> init_flag;
	typename std::aligned_storage<sizeof(T), std::alignment_of<T>::value>::type storage;

	static singleton instance;

	// Use a destructor instead of atexit() because the latter does not work
	// properly when the singleton is in a library that is unloaded.
	~singleton()
	{
		if (init_flag.load(std::memory_order_acquire))
			reinterpret_cast<T*>(&storage)->~T();
	}

public:
	static T& get_instance()
	{
		T* ptr = reinterpret_cast<T*>(&instance.storage);
		if (!instance.init_flag.load(std::memory_order_acquire)) {
			std::lock_guard<std::mutex> locked(instance.lock);
			if (!instance.init_flag.load(std::memory_order_relaxed)) {
				new(ptr) T;
				instance.init_flag.store(true, std::memory_order_release);
			}
		}
		return *ptr;
	}
};

template<typename T> singleton<T> singleton<T>::instance;
#endif

// The default scheduler is just a thread pool which can be configured
// using environment variables.
class default_scheduler_impl: public threadpool_scheduler {
	std::size_t get_num_threads()
	{
		// Get the requested number of threads from the environment
		// If that fails, use the number of CPUs in the system
		const char *s = std::getenv("LIBASYNC_NUM_THREADS");
		std::size_t num_threads;
		if (s)
			num_threads = std::strtoul(s, nullptr, 10);
		else
			num_threads = hardware_concurrency();

		// Make sure the thread count is reasonable
		if (num_threads < 1)
			num_threads = 1;
		return num_threads;
	}

public:
	default_scheduler_impl()
		: threadpool_scheduler(get_num_threads()) {}
};

threadpool_scheduler& internal_default_scheduler()
{
	return singleton<default_scheduler_impl>::get_instance();
}

// Thread scheduler implementation
void thread_scheduler_impl::schedule(task_run_handle t)
{
	// A shared_ptr is used here because not all implementations of
	// std::thread support move-only objects.
	std::thread([](const std::shared_ptr<task_run_handle>& t) {
		t->run();
	}, std::make_shared<task_run_handle>(std::move(t))).detach();
}

} // namespace detail

// FIFO scheduler implementation
struct fifo_scheduler::internal_data {
	detail::fifo_queue queue;
	std::mutex lock;
};
fifo_scheduler::fifo_scheduler()
	: impl(new internal_data) {}
fifo_scheduler::~fifo_scheduler() {}
void fifo_scheduler::schedule(task_run_handle t)
{
	std::lock_guard<std::mutex> locked(impl->lock);
	impl->queue.push(std::move(t));
}
bool fifo_scheduler::try_run_one_task()
{
	task_run_handle t;
	{
		std::lock_guard<std::mutex> locked(impl->lock);
		t = impl->queue.pop();
	}
	if (t) {
		t.run();
		return true;
	}
	return false;
}
void fifo_scheduler::run_all_tasks()
{
	while (try_run_one_task()) {}
}

std::size_t hardware_concurrency()
{
	// Cache the value because calculating it may be expensive
	static std::size_t value = std::thread::hardware_concurrency();

	// Always return at least 1 core
	return value == 0 ? 1 : value;
}

wait_handler set_thread_wait_handler(wait_handler handler)
{
	wait_handler old = detail::thread_wait_handler;
	detail::thread_wait_handler = handler;
	return old;
}

} // namespace async

#if defined(__GNUC__) && !defined(_WIN32)
#pragma GCC visibility pop
#endif
