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

// Task handle used in scheduler, acts as a unique_ptr to a task object
class task_handle {
	detail::task_ptr handle;

	// Allow access from schedule_task
	template<typename Sched> friend void detail::schedule_task(Sched& sched, detail::task_ptr t);

public:
	task_handle() = default;
	task_handle(const task_handle&) = delete;
	task_handle(task_handle&&) = default;
	task_handle& operator=(const task_handle&) = delete;
	task_handle& operator=(task_handle&&) = default;

	explicit operator bool() const
	{
		return static_cast<bool>(handle);
	}

	// Run the task and release the handle
	void run()
	{
		handle->execute();
		handle = nullptr;
	}
};

// Scheduler interface
class scheduler {
public:
	// Schedule a task for execution. Failure can be indicated by throwing, but
	// then the task must not be executed.
	virtual void schedule(task_handle t) = 0;
};

namespace detail {

// Scheduler implementations
class default_scheduler_impl: public scheduler {
public:
	default_scheduler_impl();
	~default_scheduler_impl();
	LIBASYNC_EXPORT virtual void schedule(task_handle t) override final;
};
class inline_scheduler_impl: public scheduler {
public:
	virtual void schedule(task_handle t) override final
	{
		t.run();
	}
};
class thread_scheduler_impl: public scheduler {
public:
	virtual void schedule(task_handle t) override final
	{
		std::thread([](task_handle t) {
			t.run();
		}, std::move(t));
	}
};

// Schedule a task for execution using its scheduler
template<typename Sched> void schedule_task(Sched& sched, task_ptr t)
{
	task_handle handle;
	handle.handle = std::move(t);
	sched.schedule(std::move(handle));
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

} // namespace async
