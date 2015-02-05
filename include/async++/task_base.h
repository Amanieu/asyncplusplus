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

// Task states
// Intel C++ seems to get confused by enum class, so just use a normal enum.
// We can still access elements using the task_state:: scope.
enum task_state: unsigned char {
	pending, // Task has not completed yet
	locked, // Task is locked (used by event_task to prevent double set)
	completed, // Task has finished execution and a result is available
	canceled // Task has been canceled and an exception is available
};

// Determine whether a task is in a final state
inline bool is_finished(task_state s)
{
	return s == task_state::completed || s == task_state::canceled;
}

// Operations for dispatch function
enum class dispatch_op {
	execute,
	cancel_hook,
	destroy
};

// Thread-safe vector of task_ptr which is optimized for the common case of
// only having a single continuation.
class continuation_vector {
	// Heap-allocated data for the slow path
	struct vector_data {
		std::vector<task_base*> vector;
		std::mutex lock;
	};

	// Internal data of the vector
	struct internal_data {
		// If this is true then no more changes are allowed
		bool is_locked;

		// Indicates which element of the union is currently active
		bool is_vector;

		union {
			// Fast path: This represents zero (nullptr) or one elements
			task_base* inline_ptr;

			// Slow path: This is used for two or more elements
			vector_data* vector_ptr;
		};
	};

	// All changes to the internal data are atomic
	std::atomic<internal_data> atomic_data;

public:
	// Start unlocked with zero elements in the fast path
	continuation_vector()
		: atomic_data(internal_data{false, false, {}}) {}

	// Free any left over data
	~continuation_vector()
	{
		// Converting task_ptr instead of using remove_ref because task_base
		// isn't defined yet at this point.
		internal_data data = atomic_data.load(std::memory_order_relaxed);
		if (!data.is_vector) {
			// If the data is locked then the inline pointer is already gone
			if (!data.is_locked)
				task_ptr tmp(data.inline_ptr);
		} else {
			for (task_base* i: data.vector_ptr->vector)
				task_ptr tmp(i);
			delete data.vector_ptr;
		}
	}

	// Try adding an element to the vector. This fails and returns false if
	// the vector has been locked. In that case t is not modified.
	bool try_add(task_ptr&& t)
	{
		// Cache to avoid re-allocating vector_data multiple times. This is
		// automatically freed if it is not successfully saved to atomic_data.
		std::unique_ptr<vector_data> vector;

		// Compare-exchange loop on atomic_data
		internal_data data = atomic_data.load(std::memory_order_relaxed);
		internal_data new_data;
		new_data.is_locked = false;
		do {
			// Return immediately if the vector is locked
			if (data.is_locked)
				return false;

			if (!data.is_vector) {
				if (!data.inline_ptr) {
					// Going from 0 to 1 elements
					new_data.inline_ptr = t.get();
					new_data.is_vector = false;
				} else {
					// Going from 1 to 2 elements, allocate a vector_data
					if (!vector)
						vector.reset(new vector_data{{data.inline_ptr, t.get()}, {}});
					new_data.vector_ptr = vector.get();
					new_data.is_vector = true;
				}
			} else {
				// Larger vectors use a mutex, so grab the lock
				std::atomic_thread_fence(std::memory_order_acquire);
				std::lock_guard<std::mutex> locked(data.vector_ptr->lock);

				// We need to check again if the vector has been locked here
				// to avoid a race condition with flush_and_lock
				if (atomic_data.load(std::memory_order_relaxed).is_locked)
					return false;

				// Add the element to the vector and return
				data.vector_ptr->vector.push_back(t.release());
				return true;
			}
		} while (!atomic_data.compare_exchange_weak(data, new_data, std::memory_order_release, std::memory_order_relaxed));

		// If we reach this point then atomic_data was successfully changed.
		// Since the pointers are now saved in the vector, release them from
		// the smart pointers.
		t.release();
		vector.release();
		return true;
	}

	// Lock the vector and flush all elements through the given function
	template<typename Func> void flush_and_lock(Func&& func)
	{
		// Try to lock the vector using a compare-exchange loop
		internal_data data = atomic_data.load(std::memory_order_relaxed);
		internal_data new_data;
		do {
			new_data = data;
			new_data.is_locked = true;
		} while (!atomic_data.compare_exchange_weak(data, new_data, std::memory_order_acquire, std::memory_order_relaxed));

		if (!data.is_vector) {
			// If there is an inline element, just pass it on
			if (data.inline_ptr)
				func(task_ptr(data.inline_ptr));
		} else {
			// If we are using vector_data, lock it and flush all elements
			std::lock_guard<std::mutex> locked(data.vector_ptr->lock);
			for (auto i: data.vector_ptr->vector)
				func(task_ptr(i));

			// Clear the vector to save memory. Note that we don't actually free
			// the vector_data here because other threads may still be using it.
			// This isn't a very significant cost since multiple continuations
			// are relatively rare.
			data.vector_ptr->vector.clear();
		}
	}
};

// Type-generic base task object
struct task_base: public ref_count_base<task_base> {
	// Task state
	std::atomic<task_state> state;

	// Whether this task should be run even if the parent was canceled
	bool always_cont;

	// Vector of continuations
	continuation_vector continuations;

	// Scheduler to be used to schedule this task
	scheduler_ref sched;

	// Exception associated with the task if it was canceled
	std::exception_ptr except;

	// Dispatch function with 3 operations:
	// - Run the task function
	// - Free the task function when canceling
	// - Destroy the task function and result
	void (*dispatch)(task_base*, dispatch_op);

	// Use aligned memory allocation
	static void* operator new(std::size_t size)
	{
		return aligned_alloc(size, LIBASYNC_CACHELINE_SIZE);
	}
	static void operator delete(void* ptr)
	{
		aligned_free(ptr);
	}

	// Initialize task state
	task_base()
		: state(task_state::pending) {}

	// Destroy task function and result in destructor
	~task_base()
	{
		dispatch(this, dispatch_op::destroy);
	}

	// Run a single continuation
	template<typename Sched>
	void run_continuation(Sched& sched, task_ptr&& cont, bool cancel)
	{
		// Handle continuations that run even if the parent task is canceled
		if (!cancel || cont->always_cont) {
			LIBASYNC_TRY {
				detail::schedule_task(sched, std::move(cont));
			} LIBASYNC_CATCH(...) {
				// This is suboptimal, but better than letting the exception leak
				cont->cancel(std::current_exception());
			}
		} else
			cont->cancel(except);
	}

	// Run all of the task's continuations after it has completed or canceled.
	// The list of continuations is then emptied.
	void run_continuations(bool cancel)
	{
		continuations.flush_and_lock([this, cancel](task_ptr t) {
			scheduler_ref sched = t->sched;
			run_continuation(sched, std::move(t), cancel);
		});
	}

	// Add a continuation to this task
	template<typename Sched>
	void add_continuation(Sched& sched, task_ptr cont)
	{
		// Check for task completion
		task_state current_state = state.load(std::memory_order_relaxed);
		if (!is_finished(current_state)) {
			// Try to add the task to the continuation list. This can fail only
			// if the task has just finished, in which case we run it directly.
			if (continuations.try_add(std::move(cont)))
				return;
		}

		// Otherwise run the continuation directly
		std::atomic_thread_fence(std::memory_order_acquire);
		run_continuation(sched, std::move(cont), current_state == task_state::canceled);
	}

	// Cancel the task with an exception
	void cancel(std::exception_ptr cancel_exception)
	{
		// Destroy the function object in the task before canceling
		dispatch(this, dispatch_op::cancel_hook);
		cancel_base(std::move(cancel_exception));
	}

	// Cancel function to be used for tasks which don't have an associated
	// function object (event_task).
	void cancel_base(std::exception_ptr cancel_exception)
	{
		except = std::move(cancel_exception);
		state.store(task_state::canceled, std::memory_order_release);
		run_continuations(true);
	}

	// Finish the task after it has been executed and the result set
	void finish()
	{
		state.store(task_state::completed, std::memory_order_release);
		run_continuations(false);
	}

	// Check whether the task is ready and include an acquire barrier if it is
	bool ready() const
	{
		return is_finished(state.load(std::memory_order_acquire));
	}

	// Wait for the task to finish executing
	task_state wait()
	{
		task_state s = state.load(std::memory_order_acquire);
		if (!is_finished(s)) {
			wait_for_task(this);
			s = state.load(std::memory_order_relaxed);
		}
		return s;
	}

	// Wait and throw the exception if the task was canceled
	void wait_and_throw()
	{
		if (wait() == task_state::canceled) {
#ifdef LIBASYNC_NO_EXCEPTIONS
			std::abort();
#else
			std::rethrow_exception(except);
#endif
		}
	}
};

// Result type-specific task object
template<typename Result>
struct task_result: public task_base {
	typename std::aligned_storage<sizeof(Result), std::alignment_of<Result>::value>::type result;

	// Set the dispatch function
	task_result()
	{
		dispatch = cleanup;
	}

	template<typename T>
	void set_result(T&& t)
	{
		new(&result) Result(std::forward<T>(t));
	}

	// Return a result using an lvalue or rvalue reference depending on the task
	// type. The task parameter is not used, it is just there for overload resolution.
	template<typename T>
	Result&& get_result(const task<T>&)
	{
		return std::move(*reinterpret_cast<Result*>(&result));
	}
	template<typename T>
	const Result& get_result(const shared_task<T>&)
	{
		return *reinterpret_cast<Result*>(&result);
	}

	// Result-specific dispatch function
	static void cleanup(task_base* t, dispatch_op op)
	{
		// Only need to handle destruction here
		if (op == dispatch_op::destroy) {
			// Result is only present if the task completed successfully
			task_result* current_task = static_cast<task_result*>(t);
			if (current_task->state.load(std::memory_order_relaxed) == task_state::completed)
				reinterpret_cast<Result*>(&current_task->result)->~Result();
		}
	}
};

// Specialization for references
template<typename Result>
struct task_result<Result&>: public task_base {
	// Store as pointer internally
	Result* result;

	// Set the dispatch function
	task_result()
	{
		dispatch = cleanup;
	}

	void set_result(Result& obj)
	{
		result = std::addressof(obj);
	}

	template<typename T>
	Result& get_result(const task<T>&)
	{
		return *result;
	}
	template<typename T>
	Result& get_result(const shared_task<T>&)
	{
		return *result;
	}

	// No cleanup required for references
	static void cleanup(task_base*, dispatch_op) {}
};

// Specialization for void
template<>
struct task_result<fake_void>: public task_base {
	void set_result(fake_void) {}

	// Set the dispatch function
	task_result()
	{
		dispatch = cleanup;
	}

	// Get the result as fake_void so that it can be passed to set_result and
	// continuations
	template<typename T>
	fake_void get_result(const task<T>&)
	{
		return fake_void();
	}
	template<typename T>
	fake_void get_result(const shared_task<T>&)
	{
		return fake_void();
	}

	// No cleanup required for void
	static void cleanup(task_base*, dispatch_op) {}
};

// Class to hold a function object, with empty base class optimization
template<typename Func, typename = void>
struct func_base {
	Func func;

	template<typename F>
	explicit func_base(F&& f)
		: func(std::forward<F>(f)) {}
	Func& get_func()
	{
		return func;
	}
};
template<typename Func>
struct func_base<Func, typename std::enable_if<std::is_empty<Func>::value>::type> {
	template<typename F>
	explicit func_base(F&& f)
	{
		new(this) Func(std::forward<F>(f));
	}
	~func_base()
	{
		get_func().~Func();
	}
	Func& get_func()
	{
		return *reinterpret_cast<Func*>(this);
	}
};

// Class to hold a function object and initialize/destroy it at any time
template<typename Func, typename = void>
struct func_holder {
	typename std::aligned_storage<sizeof(Func), std::alignment_of<Func>::value>::type func;

	Func& get_func()
	{
		return *reinterpret_cast<Func*>(&func);
	}
	template<typename... Args>
	void init_func(Args&&... args)
	{
		new(&func) Func(std::forward<Args>(args)...);
	}
	void destroy_func()
	{
		get_func().~Func();
	}
};
template<typename Func>
struct func_holder<Func, typename std::enable_if<std::is_empty<Func>::value>::type> {
	Func& get_func()
	{
		return *reinterpret_cast<Func*>(this);
	}
	template<typename... Args>
	void init_func(Args&&... args)
	{
		new(this) Func(std::forward<Args>(args)...);
	}
	void destroy_func()
	{
		get_func().~Func();
	}
};

// Task object with an associated function object
// Using private inheritance so empty Func doesn't take up space
template<typename Func, typename Result>
struct task_func: public task_result<Result>, func_holder<Func> {
	template<typename... Args>
	explicit task_func(Args&&... args)
	{
		this->init_func(std::forward<Args>(args)...);
		this->dispatch = dispatch_func;
	}

	// Dispatch function
	static void dispatch_func(task_base* t, dispatch_op op)
	{
		task_func* current_task = static_cast<task_func*>(t);
		switch (op) {
		case dispatch_op::execute:
			LIBASYNC_TRY {
				// Dispatch to execution function
				current_task->get_func()(current_task);
			} LIBASYNC_CATCH(...) {
				current_task->cancel(std::current_exception());
			}
			break;

		case dispatch_op::cancel_hook:
			// Destroy the function object when canceling since it won't be
			// used anymore.
			current_task->destroy_func();
			break;

		case dispatch_op::destroy:
			// If the task hasn't completed yet, destroy the function object.
			if (current_task->state.load(std::memory_order_relaxed) == task_state::pending)
				current_task->destroy_func();

			// Then destroy the result
			task_result<Result>::cleanup(t, dispatch_op::destroy);
			break;
		}
	}

	// Overriden cancel which avoid dynamic dispatch overhead
	void cancel(std::exception_ptr cancel_exception)
	{
		this->destroy_func();
		this->cancel_base(std::move(cancel_exception));
	}
};

// Helper functions to access the internal_task member of a task object, which
// avoids us having to specify half of the functions in the detail namespace
// as friend. Also, internal_task is downcast to the appropriate task_result<>.
template<typename Task>
typename Task::internal_task_type* get_internal_task(const Task& t)
{
	return static_cast<typename Task::internal_task_type*>(t.internal_task.get());
}
template<typename Task>
void set_internal_task(Task& t, task_ptr p)
{
	t.internal_task = std::move(p);
}

// Common code for task unwrapping
template<typename Result, typename Func, typename Child>
struct unwrapped_func {
	explicit unwrapped_func(task_ptr t)
		: parent_task(std::move(t)) {}
	void operator()(Child child_task) const
	{
		// Forward completion state and result to parent task
		LIBASYNC_TRY {
			if (get_internal_task(child_task)->state.load(std::memory_order_relaxed) == task_state::completed) {
				static_cast<task_result<Result>*>(parent_task.get())->set_result(get_internal_task(child_task)->get_result(child_task));
				parent_task->finish();
			} else {
				// We don't call the specialized cancel function here because
				// the function of the parent task has already been destroyed.
				parent_task->cancel_base(get_internal_task(child_task)->except);
			}
		} LIBASYNC_CATCH(...) {
			// If the copy/move constructor of the result threw, propagate the exception
			parent_task->cancel_base(std::current_exception());
		}
	}
	task_ptr parent_task;
};
template<typename Result, typename Func, typename Child>
void unwrapped_finish(task_base* parent_base, Child child_task)
{
	// Save a reference to the parent in the continuation
	parent_base->add_ref();
	child_task.then(inline_scheduler(), unwrapped_func<Result, Func, Child>(task_ptr(parent_base)));

	// Destroy the parent task's function since it has been executed. Note that
	// this is after the then() call which can potentially throw.
	static_cast<task_func<Func, Result>*>(parent_base)->destroy_func();
}

// Execution functions for root tasks:
// - With and without task unwraping
template<typename Result, typename Func, bool Unwrap>
struct root_exec_func: private func_base<Func> {
	template<typename F>
	explicit root_exec_func(F&& f)
		: func_base<Func>(std::forward<F>(f)) {}
	void operator()(task_base* t)
	{
		static_cast<task_result<Result>*>(t)->set_result(invoke_fake_void(std::move(this->get_func())));
		static_cast<task_func<root_exec_func, Result>*>(t)->destroy_func();
		t->finish();
	}
};
template<typename Result, typename Func>
struct root_exec_func<Result, Func, true>: private func_base<Func> {
	template<typename F>
	explicit root_exec_func(F&& f)
		: func_base<Func>(std::forward<F>(f)) {}
	void operator()(task_base* t)
	{
		unwrapped_finish<Result, root_exec_func>(t, std::move(this->get_func())());
	}
};

// Execution functions for continuation tasks:
// - With and without task unwraping
// - For value-based and task-based continuations
template<typename Parent, typename Result, typename Func, bool ValueCont, bool Unwrap>
struct continuation_exec_func: private func_base<Func> {
	template<typename F, typename P>
	continuation_exec_func(F&& f, P&& p)
		: func_base<Func>(std::forward<F>(f)), parent(std::forward<P>(p)) {}
	void operator()(task_base* t)
	{
		static_cast<task_result<Result>*>(t)->set_result(invoke_fake_void(std::move(this->get_func()), std::move(this->parent)));
		static_cast<task_func<continuation_exec_func, Result>*>(t)->destroy_func();
		t->finish();
	}
	Parent parent;
};
template<typename Parent, typename Result, typename Func>
struct continuation_exec_func<Parent, Result, Func, true, false>: private func_base<Func> {
	template<typename F, typename P>
	continuation_exec_func(F&& f, P&& p)
		: func_base<Func>(std::forward<F>(f)), parent(std::forward<P>(p)) {}
	void operator()(task_base* t)
	{
		auto&& result = get_internal_task(parent)->get_result(parent);
		static_cast<task_result<Result>*>(t)->set_result(invoke_fake_void(std::move(this->get_func()), std::forward<decltype(result)>(result)));
		static_cast<task_func<continuation_exec_func, Result>*>(t)->destroy_func();
		t->finish();
	}
	Parent parent;
};
template<typename Parent, typename Result, typename Func>
struct continuation_exec_func<Parent, Result, Func, false, true>: private func_base<Func> {
	template<typename F, typename P>
	continuation_exec_func(F&& f, P&& p)
		: func_base<Func>(std::forward<F>(f)), parent(std::forward<P>(p)) {}
	void operator()(task_base* t)
	{
		unwrapped_finish<Result, continuation_exec_func>(t, invoke_fake_void(std::move(this->get_func()), std::move(parent)));
	}
	Parent parent;
};
template<typename Parent, typename Result, typename Func>
struct continuation_exec_func<Parent, Result, Func, true, true>: private func_base<Func> {
	template<typename F, typename P>
	continuation_exec_func(F&& f, P&& p)
		: func_base<Func>(std::forward<F>(f)), parent(std::forward<P>(p)) {}
	void operator()(task_base* t)
	{
		auto&& result = get_internal_task(parent)->get_result(parent);
		unwrapped_finish<Result, continuation_exec_func>(t, invoke_fake_void(std::move(this->get_func()), std::forward<decltype(result)>(result)));
	}
	Parent parent;
};

} // namespace detail
} // namespace async
