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

class task_run_handle;

// Scheduler interface:
// A scheduler is any type that implements this function:
// void schedule(async::task_run_handle t);
// This function should result in t.run() being called at some future point.

namespace detail {

// Detect whether an object is a scheduler
template<typename T, typename = decltype(std::declval<T>().schedule(std::declval<task_run_handle>()))>
two& is_scheduler_helper(int);
template<typename T>
one& is_scheduler_helper(...);
template<typename T>
struct is_scheduler: public std::integral_constant<bool, sizeof(is_scheduler_helper<T>(0)) - 1> {};

// Type-erased reference to a scheduler, which is itself a scheduler
class scheduler_ref {
	// Type-erased data
	void* sched_ptr;
	void (*sched_func)(void* sched, task_run_handle&& t);
	template<typename T>
	static void invoke_sched(void* sched, task_run_handle&& t)
	{
		static_cast<T*>(sched)->schedule(std::move(t));
	}

public:
	// Wrap the given scheduler type
	scheduler_ref() {}
	template<typename T> explicit scheduler_ref(T& sched)
		: sched_ptr(std::addressof(sched)), sched_func(invoke_sched<T>) {}

	// Forward tasks to the wrapped scheduler
	void schedule(task_run_handle&& t)
	{
		sched_func(sched_ptr, std::move(t));
	}
};

// Singleton scheduler classes
class thread_scheduler_impl {
public:
	LIBASYNC_EXPORT static void schedule(task_run_handle t);
};
class inline_scheduler_impl {
public:
	static void schedule(task_run_handle t);
};
class default_scheduler_impl {
public:
	default_scheduler_impl();
	~default_scheduler_impl();
	LIBASYNC_EXPORT static void schedule(task_run_handle t);
};

// Run a task in a thread pool. This scheduler will wait for all tasks to finish
// at program exit.
LIBASYNC_EXPORT default_scheduler_impl& internal_default_scheduler();

// Reference counted pointer to task data
struct task_base;
typedef ref_count_ptr<task_base> task_ptr;

// Helper function to schedule a task using a scheduler
template<typename Sched>
void schedule_task(Sched& sched, task_ptr t);

// Wait for the given task to finish. This will call the wait handler currently
// active for this thread, which causes the thread to sleep by default.
LIBASYNC_EXPORT void wait_for_task(task_base* wait_task);

} // namespace detail

// Run a task in the current thread as soon as it is scheduled
inline detail::inline_scheduler_impl& inline_scheduler()
{
	static detail::inline_scheduler_impl instance;
	return instance;
}

// Run a task in a separate thread. Note that this scheduler does not wait for
// threads to finish at process exit. You must ensure that all threads finish
// before ending the process.
inline detail::thread_scheduler_impl& thread_scheduler()
{
	static detail::thread_scheduler_impl instance;
	return instance;
}

// If LIBASYNC_CUSTOM_DEFAULT_SCHEDULER is defined then async::default_scheduler
// is left undefined and should be defined by the user. Keep in mind that in
// order to work, this function should be declared before including async++.h.
#ifndef LIBASYNC_CUSTOM_DEFAULT_SCHEDULER
inline detail::default_scheduler_impl& default_scheduler()
{
	return detail::internal_default_scheduler();
}
#endif

} // namespace async
