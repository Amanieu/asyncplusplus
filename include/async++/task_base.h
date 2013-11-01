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
enum class task_state: unsigned char {
	TASK_PENDING, // Task has completed yet
	TASK_LOCKED, // Task is locked (used by event_task to prevent double set)
	TASK_COMPLETED, // Task has finished execution and a result is available
	TASK_CANCELED // Task has been canceled and an exception is available
};

// Operations for dispatch function
enum class dispatch_op {
	execute,
	cancel_hook,
	destroy
};

// Continuation vector optimized for single continuations. Only supports a
// minimal set of operations.
class continuation_vector {
	std::size_t count;

	struct data_union {
		data_union()
		{
			new(&inline_data()) task_ptr;
		}

		// Destruction is handled by the parent class
		~data_union() {}

		std::aligned_storage<sizeof(task_ptr), std::alignment_of<task_ptr>::value>::type data;

		// Inline continuation used for common case (only one continuation)
		task_ptr& inline_data()
		{
			return *reinterpret_cast<task_ptr*>(&data);
		}

		// Vector of continuations. The capacity is the lowest power of 2
		// which is >= count.
		std::unique_ptr<task_ptr[]>& vector_data()
		{
			return *reinterpret_cast<std::unique_ptr<task_ptr[]>*>(&data);
		}
	} data;

public:
	continuation_vector()
		: count(0) {}

	task_ptr* begin()
	{
		if (count > 1)
			return data.vector_data().get();
		else
			return &data.inline_data();
	}
	task_ptr* end()
	{
		return begin() + count;
	}

	void push_back(task_ptr t)
	{
		// First try to insert the continuation inline
		if (count == 0) {
			data.inline_data() = std::move(t);
			count = 1;
		}

		// Check if we need to go from an inline continuation to a vector
		else if (count == 1) {
			std::unique_ptr<task_ptr[]> ptr(new task_ptr[2]);
			ptr[0] = std::move(data.inline_data());
			ptr[1] = std::move(t);
			data.inline_data().~task_ptr();
			new(&data.vector_data()) std::unique_ptr<task_ptr[]>(std::move(ptr));
			count = 2;
		}

		// Check if the vector needs to be grown (size is a power of 2)
		else if ((count & (count - 1)) == 0) {
			std::unique_ptr<task_ptr[]> ptr(new task_ptr[count * 2]);
			std::move(data.vector_data().get(), data.vector_data().get() + count, ptr.get());
			ptr[count++] = std::move(t);
			data.vector_data() = std::move(ptr);
		}
	}

	void clear()
	{
		if (count > 1) {
			data.vector_data().~unique_ptr();
			new(&data.inline_data()) task_ptr;
		} else
			data.inline_data() = nullptr;
		count = 0;
	}

	std::size_t size() const
	{
		return count;
	}

	~continuation_vector()
	{
		if (count > 1)
			data.vector_data().~unique_ptr();
		else
			data.inline_data().~task_ptr();
	}
};

// Type-generic base task object
struct LIBASYNC_CACHELINE_ALIGN task_base: public ref_count_base<task_base> {
	// Task state
	std::atomic<task_state> state;

	// Whether this task should be run even if the parent was canceled
	bool always_cont;

	// Vector of continuations and lock protecting it
	spinlock lock;
	continuation_vector continuations;

	// Scheduler to be used to schedule this task
	scheduler* sched;

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
		: state(task_state::TASK_PENDING) {}

	// Destroy task function and result in destructor
	~task_base()
	{
		dispatch(this, dispatch_op::destroy);
	}

	// Run a single continuation
	void run_continuation(task_ptr&& cont, bool cancel)
	{
		// Handle continuations that run even if the parent task is canceled
		if (!cancel || cont->always_cont) {
			scheduler& s = *cont->sched;
			schedule_task(s, std::move(cont));
		} else
			cont->cancel(except);
	}

	// Run all of the task's continuations after it has completed or canceled.
	// The list of continuations is then emptied.
	template<bool cancel>
	void run_continuations()
	{
		// Wait for any threads which may be adding a continuation. The lock is
		// not needed afterwards because any future continuations are run
		// directly instead of being added to the continuation list.
		{
			std::lock_guard<spinlock> locked(lock);
		}

		// Early exit for common case of zero continuations
		if (continuations.size() == 0)
			return;

		for (auto& i: continuations)
			run_continuation(std::move(i), cancel);
		continuations.clear();
	}

	// Add a continuation to this task
	void add_continuation(task_ptr cont)
	{
		// Check for task completion
		task_state current_state = state.load(std::memory_order_relaxed);
		if (current_state < task_state::TASK_COMPLETED) {
			std::lock_guard<spinlock> locked(lock);

			// If the task has not finished yet, add the continuation to it
			current_state = state.load(std::memory_order_relaxed);
			if (current_state < task_state::TASK_COMPLETED) {
				continuations.push_back(cont);
				return;
			}
		}

		// Otherwise run the continuation directly
		std::atomic_thread_fence(std::memory_order_acquire);
		run_continuation(std::move(cont), current_state == task_state::TASK_CANCELED);
	}

	// Cancel the task with an exception
	void cancel(std::exception_ptr cancel_exception)
	{
		// Destroy the function object in the task before cancelling
		dispatch(this, dispatch_op::cancel_hook);
		cancel_base(std::move(cancel_exception));
	}

	// Cancel function to be used for tasks which don't have an associated
	// function object (event_task).
	void cancel_base(std::exception_ptr cancel_exception)
	{
		except = std::move(cancel_exception);
		state.store(task_state::TASK_CANCELED, std::memory_order_release);
		run_continuations<true>();
	}

	// Finish the task after it has been executed and the result set
	void finish()
	{
		state.store(task_state::TASK_COMPLETED, std::memory_order_release);
		run_continuations<false>();
	}

	// Wait for the task to finish executing
	task_state wait()
	{
		task_state s = state.load(std::memory_order_relaxed);
		if (s < task_state::TASK_COMPLETED) {
			wait_for_task(this);
			s = state.load(std::memory_order_relaxed);
		} else
			std::atomic_thread_fence(std::memory_order_acquire);
		return s;
	}

	// Wait and throw the exception if the task was canceled
	void wait_and_throw()
	{
		if (wait() == task_state::TASK_CANCELED) {
#ifdef LIBASYNC_NO_EXCEPTIONS
			std::abort();
#else
			if (except)
				std::rethrow_exception(except);
			else
				throw task_canceled();
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
			if (current_task->state.load(std::memory_order_relaxed) == task_state::TASK_COMPLETED)
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
struct task_func: public task_result<Result>, private func_holder<Func> {
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

				// If we successfully ran, destroy the function object so that it
				// can release any references (shared_ptr) it holds.
				current_task->destroy_func();
			} LIBASYNC_CATCH(task_canceled) {
				// Optimize task_canceled by encoding it as a null exception_ptr
				current_task->cancel(nullptr);
			} LIBASYNC_CATCH(...) {
				current_task->cancel(std::current_exception());
			}
			break;

		case dispatch_op::cancel_hook:
			// Destroy the function object when cancelling since it won't be
			// used anymore.
			current_task->destroy_func();
			break;

		case dispatch_op::destroy:
			// If the task hasn't completed yet, destroy the function object.
			if (current_task->state.load(std::memory_order_relaxed) < task_state::TASK_COMPLETED)
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

// Helper function to access the internal_task member of a task object, which
// avoids us having to specify half of the functions in the detail namespace
// as friend. Also, internal_task is downcast to the appropriate task_result<>.
template<typename Task>
typename Task::internal_task_type* get_internal_task(const Task& t)
{
	return static_cast<typename Task::internal_task_type*>(t.internal_task.get());
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
			if (get_internal_task(child_task)->state.load(std::memory_order_relaxed) == task_state::TASK_COMPLETED) {
				static_cast<task_result<Result>*>(parent_task.get())->set_result(get_internal_task(child_task)->get_result(child_task));
				parent_task->finish();
			} else
				static_cast<task_func<Func, Result>*>(parent_task.get())->cancel(get_internal_task(child_task)->except);
		} LIBASYNC_CATCH(...) {
			// If the copy/move constructor of the result threw, propagate the exception
			static_cast<task_func<Func, Result>*>(parent_task.get())->cancel(std::current_exception());
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
		unwrapped_finish<Result, root_exec_func>(t, std::move(std::move(this->get_func()))());
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
		typedef typename detail::void_to_fake_void<typename Parent::result_type>::type internal_result;
		unwrapped_finish<internal_result, continuation_exec_func>(t, invoke_fake_void(std::move(this->get_func()), std::move(parent)));
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
		typedef typename detail::void_to_fake_void<typename Parent::result_type>::type internal_result;
		unwrapped_finish<internal_result, continuation_exec_func>(t, invoke_fake_void(std::move(this->get_func()), std::forward<decltype(result)>(result)));
	}
	Parent parent;
};

} // namespace detail
} // namespace async
