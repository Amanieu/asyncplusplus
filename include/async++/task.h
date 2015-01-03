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

// Common code for task and shared_task
template<typename Result>
class basic_task {
protected:
	// Reference counted internal task object
	detail::task_ptr internal_task;

	// Real result type, with void turned into fake_void
	typedef typename void_to_fake_void<Result>::type internal_result;

	// Type-specific task object
	typedef task_result<internal_result> internal_task_type;

	// Friend access
	template<typename T>
	friend class basic_task;
	template<typename T>
	friend typename T::internal_task_type* get_internal_task(const T& t);

	// Common code for get()
	void get_internal() const
	{
#ifndef NDEBUG
		// Catch use of uninitialized task objects
		if (!internal_task)
			LIBASYNC_THROW(std::invalid_argument("Use of empty task object"));
#endif

		// If the task was canceled, throw the associated exception
		internal_task->wait_and_throw();
	}

	// Common code for then()
	template<typename Func, typename Parent>
	typename continuation_traits<Parent, Func>::task_type then_internal(scheduler& sched, Func&& f, Parent&& parent) const
	{
#ifndef NDEBUG
		// Catch use of uninitialized task objects
		if (!internal_task)
			LIBASYNC_THROW(std::invalid_argument("Use of empty task object"));
#endif

		// Save a copy of internal_task because it might get moved into exec_func
		task_base* my_internal = internal_task.get();

		// Create continuation
		typedef typename continuation_traits<Parent, Func>::task_type::internal_result cont_internal_result;
		typedef continuation_exec_func<typename std::decay<Parent>::type, cont_internal_result, typename std::decay<Func>::type, continuation_traits<Parent, Func>::is_value_cont::value, is_task<typename continuation_traits<Parent, Func>::result_type>::value> exec_func;
		typename continuation_traits<Parent, Func>::task_type cont;
		cont.internal_task = task_ptr(new task_func<exec_func, cont_internal_result>(std::forward<Func>(f), std::forward<Parent>(parent)));

		// Set continuation parameters
		cont.internal_task->sched = &sched;
		cont.internal_task->always_cont = !continuation_traits<Parent, Func>::is_value_cont::value;

		// Add the continuation to this task
		// Avoid an expensive ref-count modification since the task isn't shared yet
		cont.internal_task->add_ref_unlocked();
		my_internal->add_continuation(task_ptr(cont.internal_task.get()));

		return cont;
	}

public:
	// Task result type
	typedef Result result_type;

	// Check if this task is not empty
	bool valid() const
	{
		return internal_task != nullptr;
	}

	// Query whether the task has finished executing
	bool ready() const
	{
		return internal_task->ready();
	}

	// Wait for the task to complete
	void wait() const
	{
#ifndef NDEBUG
		// Catch use of uninitialized task objects
		if (!internal_task)
			LIBASYNC_THROW(std::invalid_argument("Use of empty task object"));
#endif

		internal_task->wait();
	}
};

// Common code for event_task specializations
template<typename Result>
class basic_event {
protected:
	// Reference counted internal task object
	detail::task_ptr internal_task;

	// Real result type, with void turned into fake_void
	typedef typename detail::void_to_fake_void<Result>::type internal_result;

	// Type-specific task object
	typedef detail::task_result<internal_result> internal_task_type;

	// Common code for set()
	template<typename T>
	bool set_internal(T&& result) const
	{
#ifndef NDEBUG
		// Catch use of uninitialized task objects
		if (!internal_task)
			LIBASYNC_THROW(std::invalid_argument("Use of empty event_task object"));
#endif

		// Only allow setting the value once
		detail::task_state expected = detail::task_state::pending;
		if (!internal_task->state.compare_exchange_strong(expected, detail::task_state::locked, std::memory_order_acquire, std::memory_order_relaxed))
			return false;

		LIBASYNC_TRY {
			// Store the result and finish
			static_cast<internal_task_type*>(internal_task.get())->set_result(std::forward<T>(result));
			internal_task->finish();
		} LIBASYNC_CATCH(...) {
			// If the copy/move constructor of the result threw, save the exception.
			// We could also return the exception to the caller, but this would
			// cause race conditions.
			internal_task->cancel_base(std::current_exception());
		}
		return true;
	}

	// Movable but not copyable
	basic_event(const basic_event&);
	basic_event& operator=(const basic_event&);

public:
	basic_event(basic_event&& other) LIBASYNC_NOEXCEPT
		: internal_task(std::move(other.internal_task))
	{
		other.internal_task = nullptr;
	}
	basic_event& operator=(basic_event&& other) LIBASYNC_NOEXCEPT
	{
		std::swap(internal_task, other.internal_task);
		return *this;
	}

	// Main constructor
	basic_event()
		: internal_task(new internal_task_type) {}

	// Cancel events if they are destroyed before they are set
	~basic_event()
	{
		// This has no effect if a result is already set
		if (internal_task)
			cancel();
	}

	// Get a task linked to this event
	task<Result> get_task() const
	{
#ifndef NDEBUG
		// Catch use of uninitialized task objects
		if (!internal_task)
			LIBASYNC_THROW(std::invalid_argument("Use of empty event_task object"));
#endif

		task<Result> out;
		out.internal_task = internal_task;
		return out;
	}

	// Cancel the event with an exception and cancel continuations
	bool set_exception(std::exception_ptr except) const
	{
#ifndef NDEBUG
		// Catch use of uninitialized task objects
		if (!internal_task)
			LIBASYNC_THROW(std::invalid_argument("Use of empty event_task object"));
#endif

		// Only allow setting the value once
		detail::task_state expected = detail::task_state::pending;
		if (!internal_task->state.compare_exchange_strong(expected, detail::task_state::locked, std::memory_order_acquire, std::memory_order_relaxed))
			return false;

		// Cancel the task
		internal_task->cancel_base(std::move(except));
		return true;
	}

	// Cancel the event as if with cancel_current_task
	bool cancel() const
	{
		return set_exception(nullptr);
	}
};

} // namespace detail

template<typename Result>
class task: public detail::basic_task<Result> {
	// Friend access for make_task, spawn and event_task::get_task
	template<typename T>
	friend task<typename std::decay<T>::type> make_task(T&& value);
	friend task<void> make_task();
	template<typename Func>
	friend task<typename detail::remove_task<typename std::result_of<Func()>::type>::type> spawn(scheduler& sched, Func&& f);
	friend class detail::basic_event<Result>;

	// Movable but not copyable
	task(const task&);
	task& operator=(const task&);

public:
	task() {}
	task(task&& other) LIBASYNC_NOEXCEPT
		: detail::basic_task<Result>(std::move(other)) {}
	task& operator=(task&& other) LIBASYNC_NOEXCEPT
	{
		detail::basic_task<Result>::operator=(std::move(other));
		return *this;
	}

	// Get the result of the task
	Result get()
	{
		this->get_internal();
		detail::task_ptr my_internal = std::move(this->internal_task);
		return detail::fake_void_to_void(static_cast<typename task::internal_task_type*>(my_internal.get())->get_result(*this));
	}

	// Add a continuation to the task
	template<typename Func>
	typename detail::continuation_traits<task, Func>::task_type then(scheduler& sched, Func&& f)
	{
		auto result = this->then_internal(sched, std::forward<Func>(f), std::move(*this));
		this->internal_task = nullptr;
		return result;
	}
	template<typename Func>
	typename detail::continuation_traits<task, Func>::task_type then(Func&& f)
	{
		return then(LIBASYNC_DEFAULT_SCHEDULER, std::forward<Func>(f));
	}

	// Create a shared_task from this task
	shared_task<Result> share()
	{
		shared_task<Result> out;
		out.internal_task = std::move(this->internal_task);
		return out;
	}
};

template<typename Result>
class shared_task: public detail::basic_task<Result> {
	// Friend access for task::share
	friend class task<Result>;

	// get() return value: const Result& -or- void
	typedef typename std::conditional<
		std::is_void<Result>::value,
		void,
		typename std::add_lvalue_reference<
			typename std::add_const<Result>::type
		>::type
	>::type get_result;

public:
	// Movable and copyable
	shared_task() {}

	// Get the result of the task
	get_result get() const
	{
		this->get_internal();
		return detail::fake_void_to_void(detail::get_internal_task(*this)->get_result(*this));
	}

	// Add a continuation to the task
	template<typename Func>
	typename detail::continuation_traits<shared_task, Func>::task_type then(scheduler& sched, Func&& f) const
	{
		return this->then_internal(sched, std::forward<Func>(f), *this);
	}
	template<typename Func>
	typename detail::continuation_traits<shared_task, Func>::task_type then(Func&& f) const
	{
		return then(LIBASYNC_DEFAULT_SCHEDULER, std::forward<Func>(f));
	}
};

// Special task type which can be triggered manually rather than when a function executes.
template<typename Result>
class event_task: public detail::basic_event<Result> {
	// Movable but not copyable
	event_task(const event_task&);
	event_task& operator=(const event_task& other);

public:
	event_task() {}
	event_task(event_task&& other) LIBASYNC_NOEXCEPT
		: detail::basic_event<Result>(std::move(other)) {}
	event_task& operator=(event_task&& other) LIBASYNC_NOEXCEPT
	{
		detail::basic_event<Result>::operator=(std::move(other));
		return *this;
	}

	// Set the result of the task, mark it as completed and run its continuations
	bool set(const Result& result) const
	{
		return this->set_internal(result);
	}
	bool set(Result&& result) const
	{
		return this->set_internal(std::move(result));
	}
};

// Specialization for references
template<typename Result>
class event_task<Result&>: public detail::basic_event<Result&> {
	// Movable but not copyable
	event_task(const event_task&);
	event_task& operator=(const event_task& other);

public:
	event_task() {}
	event_task(event_task&& other) LIBASYNC_NOEXCEPT
		: detail::basic_event<Result&>(std::move(other)) {}
	event_task& operator=(event_task&& other) LIBASYNC_NOEXCEPT
	{
		detail::basic_event<Result&>::operator=(std::move(other));
		return *this;
	}

	// Set the result of the task, mark it as completed and run its continuations
	bool set(Result& result) const
	{
		return this->set_internal(result);
	}
};

// Specialization for void
template<>
class event_task<void>: public detail::basic_event<void> {
	// Movable but not copyable
	event_task(const event_task&);
	event_task& operator=(const event_task& other);

public:
	event_task() {}
	event_task(event_task&& other) LIBASYNC_NOEXCEPT
		: detail::basic_event<void>(std::move(other)) {}
	event_task& operator=(event_task&& other) LIBASYNC_NOEXCEPT
	{
		detail::basic_event<void>::operator=(std::move(other));
		return *this;
	}

	// Set the result of the task, mark it as completed and run its continuations
	bool set()
	{
		return this->set_internal(detail::fake_void());
	}
};

// Task type returned by local_spawn()
template<typename Func>
class LIBASYNC_CACHELINE_ALIGN local_task {
	// Make sure the function type is callable
	static_assert(detail::is_callable<Func()>::value, "Invalid function type passed to local_spawn()");

	// Task result type
	typedef typename detail::remove_task<decltype(std::declval<Func>()())>::type result_type;
	typedef typename detail::void_to_fake_void<result_type>::type internal_result;

	// Task execution function type
	typedef detail::root_exec_func<internal_result, typename std::decay<Func>::type, detail::is_task<decltype(std::declval<Func>()())>::value> exec_func;

	// Task object embedded directly. The ref-count is initialized to 1 so it
	// will never be freed using delete, only in destructor.
	detail::task_func<exec_func, internal_result> internal_task;

	// Friend access for local_spawn
	template<typename F>
	friend local_task<F> local_spawn(scheduler& sched, F&& f);
	template<typename F>
	friend local_task<F> local_spawn(F&& f);

	// Constructor, used by local_spawn
	local_task(scheduler& sched, Func&& f)
		: internal_task(std::forward<Func>(f))
	{
		// Avoid an expensive ref-count modification since the task isn't shared yet
		internal_task.add_ref_unlocked();
		detail::schedule_task(sched, detail::task_ptr(&internal_task));
	}

	// Non-movable and non-copyable
	local_task(const local_task&);
	local_task(local_task &&);
	local_task& operator=(const local_task&);
	local_task& operator=(local_task&&);

public:
	// Wait for the task to complete when destroying
	~local_task()
	{
		wait();

		// Now spin until the reference count to drops to 1, since other threads
		// may still have a reference to the task.
		while (internal_task.ref_count.load(std::memory_order_relaxed) != 1)
			detail::spinlock::spin_pause();
		std::atomic_thread_fence(std::memory_order_acquire);
	}

	// Query whether the task has finished executing
	bool ready() const
	{
		return internal_task.ready();
	}

	// Wait for the task to complete
	void wait()
	{
		internal_task.wait();
	}

	// Get the result of the task
	result_type get()
	{
		internal_task.wait_and_throw();
		return detail::fake_void_to_void(internal_task.get_result(task<result_type>()));
	}
};

// Spawn a function asynchronously
// Using result_of instead of decltype here because Intel C++ gets confused by the previous friend declaration
template<typename Func>
task<typename detail::remove_task<typename std::result_of<Func()>::type>::type> spawn(scheduler& sched, Func&& f)
{
	// Make sure the function type is callable
	static_assert(detail::is_callable<Func()>::value, "Invalid function type passed to spawn()");

	// Create task
	typedef typename detail::void_to_fake_void<typename detail::remove_task<decltype(std::declval<Func>()())>::type>::type internal_result;
	typedef detail::root_exec_func<internal_result, typename std::decay<Func>::type, detail::is_task<decltype(std::declval<Func>()())>::value> exec_func;
	task<typename detail::remove_task<decltype(std::declval<Func>()())>::type> out;
	out.internal_task = detail::task_ptr(new detail::task_func<exec_func, internal_result>(std::forward<Func>(f)));

	// Avoid an expensive ref-count modification since the task isn't shared yet
	out.internal_task->add_ref_unlocked();
	detail::schedule_task(sched, detail::task_ptr(out.internal_task.get()));

	return out;
}
template<typename Func>
decltype(async::spawn(LIBASYNC_DEFAULT_SCHEDULER, std::declval<Func>())) spawn(Func&& f)
{
	return async::spawn(LIBASYNC_DEFAULT_SCHEDULER, std::forward<Func>(f));
}

// Create a completed task containing a value
template<typename T>
task<typename std::decay<T>::type> make_task(T&& value)
{
	task<typename std::decay<T>::type> out;

	out.internal_task = detail::task_ptr(new detail::task_result<typename std::decay<T>::type>);
	detail::get_internal_task(out)->set_result(std::forward<T>(value));
	out.internal_task->state.store(detail::task_state::completed, std::memory_order_relaxed);

	return out;
}
inline task<void> make_task()
{
	task<void> out;

	out.internal_task = detail::task_ptr(new detail::task_result<detail::fake_void>);
	out.internal_task->state.store(detail::task_state::completed, std::memory_order_relaxed);

	return out;
}

// Spawn a very limited task which is restricted to the current function and
// joins on destruction. Because local_task is not movable, the result must
// be captured in a reference, like this:
// auto&& x = local_spawn(...);
template<typename Func>
#ifdef __GNUC__
__attribute__((warn_unused_result))
#endif
local_task<Func> local_spawn(scheduler& sched, Func&& f)
{
	// Since local_task is not movable, we construct it in-place and let the
	// caller extend the lifetime of the returned object using a reference.
	return {sched, std::forward<Func>(f)};
}
template<typename Func>
#ifdef __GNUC__
__attribute__((warn_unused_result))
#endif
local_task<Func> local_spawn(Func&& f)
{
	return {LIBASYNC_DEFAULT_SCHEDULER, std::forward<Func>(f)};
}

} // namespace async
