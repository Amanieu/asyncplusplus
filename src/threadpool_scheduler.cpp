// Copyright (c) 2015 Amanieu d'Antras
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

#include "internal.h"

// For GetProcAddress and GetModuleHandle
#ifdef _WIN32
#include <windows.h>
#endif

namespace async {
namespace detail {

// Per-thread data, aligned to cachelines to avoid false sharing
struct LIBASYNC_CACHELINE_ALIGN thread_data_t {
	work_steal_queue queue;
	std::minstd_rand rng;
	task_wait_event event;
	std::thread handle;
};

// Internal data used by threadpool_scheduler
struct threadpool_data {
	threadpool_data(std::size_t num_threads)
		: thread_data(num_threads), shutdown(false), num_waiters(0), waiters(new task_wait_event*[num_threads]) {}

	// Mutex protecting everything except thread_data
	std::mutex lock;

	// Array of per-thread data
	aligned_array<thread_data_t> thread_data;

	// Global queue for tasks from outside the pool
	fifo_queue public_queue;

	// Shutdown request indicator
	bool shutdown;

	// List of threads waiting for tasks to run. num_waiters needs to be atomic
	// because it is sometimes read outside the mutex.
	std::atomic<std::size_t> num_waiters;
	std::unique_ptr<task_wait_event*[]> waiters;

#ifdef BROKEN_JOIN_IN_DESTRUCTOR
	// Shutdown complete event, used instead of thread::join()
	std::size_t shutdown_num_threads;
	std::condition_variable shutdown_complete_event;
#endif
};

// Thread pool this thread belongs to, or null if not in pool
static THREAD_LOCAL threadpool_data* owning_threadpool = nullptr;

// Current thread's index in the pool
static THREAD_LOCAL std::size_t thread_id;

// Try to steal a task from another thread's queue
static task_run_handle steal_task(threadpool_data* impl)
{
	// Make a list of victim thread ids and shuffle it
	std::vector<std::size_t> victims(impl->thread_data.size());
	std::iota(victims.begin(), victims.end(), 0);
	std::shuffle(victims.begin(), victims.end(), impl->thread_data[thread_id].rng);

	// Try to steal from another thread
	for (std::size_t i: victims) {
		// Don't try to steal from ourself
		if (i == thread_id)
			continue;

		if (task_run_handle t = impl->thread_data[i].queue.steal())
			return t;
	}

	// No tasks found, but we might have missed one if it was just added. In
	// practice this doesn't really matter since it will be handled by another
	// thread.
	return task_run_handle();
}

// Main task stealing loop which is used by worker threads when they have
// nothing to do.
static void thread_task_loop(threadpool_data* impl, task_wait_handle wait_task)
{
	// Get our thread's data
	thread_data_t& current_thread = owning_threadpool->thread_data[thread_id];

	// Flag indicating if we have added a continuation to the task
	bool added_continuation = false;

	// Loop while waiting for the task to complete
	while (true) {
		// Check if the task has finished. If we have added a continuation, we
		// need to make sure the event has been signaled.
		if (wait_task && (added_continuation ? current_thread.event.try_wait(wait_type::task_finished) : wait_task.ready()))
			return;

		// Try to get a task from the local queue
		if (task_run_handle t = current_thread.queue.pop()) {
			t.run();
			continue;
		}

		// Stealing loop
		while (true) {
			// Try to steal a task
			if (task_run_handle t = steal_task(impl)) {
				t.run();
				break;
			}

			// Try to fetch from the public queue
			std::unique_lock<std::mutex> locked(impl->lock);
			if (task_run_handle t = impl->public_queue.pop()) {
				// Don't hold the lock while running the task
				locked.unlock();
				t.run();
				break;
			}

			// If shutting down and we don't have a task to wait for, return.
			if (!wait_task && impl->shutdown) {
#ifdef BROKEN_JOIN_IN_DESTRUCTOR
				// Notify once all worker threads have exited
				if (--impl->shutdown_num_threads == 0)
					impl->shutdown_complete_event.notify_one();
#endif
				return;
			}

			// No tasks found, so sleep until something happens.
			// If a continuation has not been added yet, add it.
			if (wait_task && !added_continuation) {
				// Create a continuation for the task we are waiting for
				task_wait_event& event = current_thread.event;
				wait_task.on_finish([&event] {
					// Signal the thread's event
					event.signal(wait_type::task_finished);
				});
				added_continuation = true;
			}

			// Add our thread to the list of waiting threads
			size_t num_waiters_val = impl->num_waiters.load(std::memory_order_relaxed);
			impl->waiters[num_waiters_val] = &current_thread.event;
			impl->num_waiters.store(num_waiters_val + 1, std::memory_order_relaxed);

			// Wait for our event to be signaled when a task is scheduled or
			// the task we are waiting for has completed.
			locked.unlock();
			int events = current_thread.event.wait();
			locked.lock();

			// Remove our thread from the list of waiting threads
			num_waiters_val = impl->num_waiters.load(std::memory_order_relaxed);
			for (std::size_t i = 0; i < num_waiters_val; i++) {
				if (impl->waiters[i] == &current_thread.event) {
					if (i != num_waiters_val - 1)
						std::swap(impl->waiters[i], impl->waiters[num_waiters_val - 1]);
					impl->num_waiters.store(num_waiters_val - 1, std::memory_order_relaxed);
					break;
				}
			}

			// Check again if the task has finished
			if (wait_task && (events & wait_type::task_finished))
				return;
		}
	}
}

// Wait for a task to complete (for worker threads inside thread pool)
static void threadpool_wait_handler(task_wait_handle wait_task)
{
	thread_task_loop(owning_threadpool, wait_task);
}

// Worker thread main loop
static void worker_thread(threadpool_data* impl, std::size_t id)
{
	// Save the thread id and owning threadpool
	owning_threadpool = impl;
	thread_id = id;

	// Set the wait handler so threads from the pool do useful work while
	// waiting for another task to finish.
	set_thread_wait_handler(threadpool_wait_handler);

	// Seed the random number generator with our id. This gives each thread a
	// different steal order.
	impl->thread_data[thread_id].rng.seed(static_cast<std::minstd_rand::result_type>(thread_id));

	// Main loop, runs until the shutdown signal is recieved
	thread_task_loop(impl, task_wait_handle());
}

// Recursive function to spawn all worker threads in parallel
static void recursive_spawn_worker_thread(threadpool_data* impl, std::size_t index, std::size_t threads)
{
	// If we are down to one thread, go to the worker main loop
	if (threads == 1)
		worker_thread(impl, index);
	else {
		// Split thread range into 2 sub-ranges
		std::size_t mid = index + threads / 2;

		// Spawn a thread for half of the range
		impl->thread_data[mid].handle = std::thread(recursive_spawn_worker_thread, impl, mid, threads - threads / 2);
#ifdef BROKEN_JOIN_IN_DESTRUCTOR
		impl->thread_data[mid].handle.detach();
#endif

		// Tail-recurse to handle our half of the range
		recursive_spawn_worker_thread(impl, index, threads / 2);
	}
}

} // namespace detail

threadpool_scheduler::threadpool_scheduler(std::size_t num_threads)
	: impl(new detail::threadpool_data(num_threads))
{
	// Start worker threads
	impl->thread_data[0].handle = std::thread(detail::recursive_spawn_worker_thread, impl.get(), 0, num_threads);
#ifdef BROKEN_JOIN_IN_DESTRUCTOR
	impl->thread_data[0].handle.detach();
#endif
}

// Wait for all currently running tasks to finish
threadpool_scheduler::~threadpool_scheduler()
{
#ifdef _WIN32
	// Windows kills all threads except one on process exit before calling
	// global destructors in DLLs. Waiting for dead threads to exit will likely
	// result in deadlocks, so we just exit early if we detect that the process
	// is exiting.
	auto RtlDllShutdownInProgress = reinterpret_cast<BOOLEAN(WINAPI *)()>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlDllShutdownInProgress"));
	if (RtlDllShutdownInProgress && RtlDllShutdownInProgress()) {
# ifndef BROKEN_JOIN_IN_DESTRUCTOR
		// We still need to detach the thread handles otherwise the std::thread
		// destructor will throw an exception.
		for (std::size_t i = 0, threadCount = impl->thread_data.size(); i < threadCount; i++) {
			try {
				impl->thread_data[i].handle.detach();
			} catch (...) {}
		}
# endif
		return;
	}
#endif

	{
		std::unique_lock<std::mutex> locked(impl->lock);

		// Signal shutdown
		impl->shutdown = true;

		// Wake up any sleeping threads
		size_t num_waiters_val = impl->num_waiters.load(std::memory_order_relaxed);
		for (std::size_t i = 0; i < num_waiters_val; i++)
			impl->waiters[i]->signal(detail::wait_type::task_available);
		impl->num_waiters.store(0, std::memory_order_relaxed);

#ifdef BROKEN_JOIN_IN_DESTRUCTOR
		// Wait for the threads to exit
		impl->shutdown_num_threads = impl->thread_data.size();
		impl->shutdown_complete_event.wait(locked);
#endif
	}

#ifndef BROKEN_JOIN_IN_DESTRUCTOR
	// Wait for the threads to exit
	for (std::size_t i = 0, threadCount = impl->thread_data.size(); i < threadCount; i++)
		impl->thread_data[i].handle.join();
#endif
}

// Schedule a task on the thread pool
void threadpool_scheduler::schedule(task_run_handle t)
{
	// Check if we are in the thread pool
	if (detail::owning_threadpool == impl.get()) {
		// Push the task onto our task queue
		impl->thread_data[detail::thread_id].queue.push(std::move(t));

		// If there are no sleeping threads, just return. We check outside the
		// lock to avoid locking overhead in the fast path.
		if (impl->num_waiters.load(std::memory_order_relaxed) == 0)
			return;

		// Get a thread to wake up from the list
		// lock scope
		{
		    std::lock_guard<std::mutex> locked(impl->lock);
		    // Check again if there are waiters
		    size_t num_waiters_val = impl->num_waiters.load(std::memory_order_relaxed);
		    if (num_waiters_val == 0)
			return;
	
			// Pop a thread from the list and wake it up
		    impl->waiters[num_waiters_val - 1]->signal(detail::wait_type::task_available);
		    impl->num_waiters.store(num_waiters_val - 1, std::memory_order_relaxed);
		}
	} else {
		std::lock_guard<std::mutex> locked(impl->lock);

		// Push task onto the public queue
		impl->public_queue.push(std::move(t));

		// Wake up a sleeping thread
		size_t num_waiters_val = impl->num_waiters.load(std::memory_order_relaxed);
		if (num_waiters_val == 0)
			return;
		impl->waiters[num_waiters_val - 1]->signal(detail::wait_type::task_available);
		impl->num_waiters.store(num_waiters_val - 1, std::memory_order_relaxed);
	}
}

} // namespace async

#if defined(__GNUC__) && !defined(_WIN32)
# pragma GCC visibility pop
#endif
