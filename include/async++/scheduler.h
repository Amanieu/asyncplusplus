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

// Task handle used by a wait handler
class task_wait_handle;

// Wait handler function prototype
typedef void (*wait_handler)(task_wait_handle t);

// Set a wait handler to control what a task does when it has "free time", which
// is when it is waiting for another task to complete. The wait handler can do
// other work, but should return when it detects that the task has completed.
// The previously installed handler is returned.
LIBASYNC_EXPORT wait_handler set_thread_wait_handler(wait_handler w);

// Task handle used in scheduler, acts as a unique_ptr to a task object
class task_run_handle {
	detail::task_ptr handle;

	// Allow construction in schedule_task()
	template<typename Sched> friend void detail::schedule_task(Sched& sched, detail::task_ptr t);
	explicit task_run_handle(detail::task_ptr t)
		: handle(std::move(t)) {}

public:
	task_run_handle(const task_run_handle&) = delete;
	task_run_handle(task_run_handle&&) = default;
	task_run_handle& operator=(const task_run_handle&) = delete;
	task_run_handle& operator=(task_run_handle&&) = default;

	// Run the task and release the handle
	void run()
	{
		handle->dispatch(handle.get(), detail::dispatch_op::execute);
		handle = nullptr;
	}

	// Run the task but run the given wait handler when waiting for a task,
	// instead of just sleeping.
	void run_with_wait_handler(wait_handler handler)
	{
		wait_handler old = set_thread_wait_handler(handler);
		run();
		set_thread_wait_handler(old);
	}

	// Conversion to and from void pointer. This allows the task handle to be
	// sent through C APIs which don't preserve types.
	void* to_void_ptr()
	{
		return handle.release();
	}
	static task_run_handle from_void_ptr(void* ptr)
	{
		return task_run_handle(detail::task_ptr(static_cast<detail::task_base*>(ptr)));
	}
};

// Scheduler interface
class scheduler {
public:
	// Schedule a task for execution. Failure can be indicated by throwing, but
	// then the task must not be executed.
	virtual void schedule(task_run_handle t) = 0;
};

namespace detail {

// Scheduler implementations
class threadpool_scheduler_impl: public scheduler {
public:
	threadpool_scheduler_impl();
	~threadpool_scheduler_impl();
	LIBASYNC_EXPORT virtual void schedule(task_run_handle t) override final;
};
class inline_scheduler_impl: public scheduler {
public:
	virtual void schedule(task_run_handle t) override final
	{
		t.run();
	}
};
class thread_scheduler_impl: public scheduler {
public:
	virtual void schedule(task_run_handle t) override final
	{
		std::thread([](task_run_handle t) {
			t.run();
		}, std::move(t));
	}
};

// Schedule a task for execution using its scheduler
template<typename Sched> void schedule_task(Sched& sched, task_ptr t)
{
	sched.schedule(task_run_handle(std::move(t)));
}

} // namespace detail

inline detail::inline_scheduler_impl& inline_scheduler()
{
	static detail::inline_scheduler_impl sched;
	return sched;
}

inline detail::thread_scheduler_impl& thread_scheduler()
{
	static detail::thread_scheduler_impl sched;
	return sched;
}

class task_wait_handle {
	detail::task_base* handle;

	// Allow construction in wait_for_task()
	friend void detail::wait_for_task(detail::task_base* t);
	task_wait_handle(detail::task_base* t)
		: handle(t) {}

	// Execution function for use by wait handlers
	template<typename Func> struct wait_exec_func: private detail::func_base<Func> {
		template<typename F> explicit wait_exec_func(F&& f): detail::func_base<Func>(std::forward<F>(f)) {}
		void operator()(detail::task_base*)
		{
			// Just call the function directly, all this wrapper does is remove
			// the task_base* parameter.
			this->get_func()();
		}
	};

	// Non-copyable and non-movable, it can only be used in a wait handler
	task_wait_handle(const task_wait_handle& other)
		: handle(other.handle) {}
	task_wait_handle& operator=(const task_wait_handle&) = delete;

public:
	// Check if the task has finished executing
	bool ready() const
	{
		if (handle->state.load(std::memory_order_relaxed) >= detail::task_state::TASK_COMPLETED) {
			std::atomic_thread_fence(std::memory_order_acquire);
			return true;
		} else
			return false;
	}

	// Queue a function to be executed when the task has finished executing.
	template<typename Func> void on_finish(Func&& func)
	{
		detail::task_ptr cont(new detail::task_func<wait_exec_func<typename std::decay<Func>::type>, detail::fake_void>(std::forward<Func>(func)));
		cont->sched = &inline_scheduler();
		cont->always_cont = true;
		handle->add_continuation(std::move(cont));
	}
};

} // namespace async
