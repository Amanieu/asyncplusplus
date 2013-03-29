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

// Continuation vector optimized for single continuations. Only supports a
// minimal set of operations.
class continuation_vector {
public:
	task_ptr* begin()
	{
		if (count > 1)
			return data.vector_data.get();
		else
			return &data.inline_data;
	}
	task_ptr* end()
	{
		return begin() + count;
	}

	void push_back(task_ptr t)
	{
		// First try to insert the continuation inline
		if (count == 0) {
			data.inline_data = std::move(t);
			count = 1;
		}

		// Check if we need to go from an inline continuation to a vector
		else if (count == 1) {
			std::unique_ptr<task_ptr[]> ptr(new task_ptr[2]);
			ptr[0] = std::move(data.inline_data);
			ptr[1] = std::move(t);
			data.inline_data.~task_ptr();
			new(&data.vector_data) std::unique_ptr<task_ptr[]>(std::move(ptr));
			count = 2;
		}

		// Check if the vector needs to be grown (size is a power of 2)
		else if ((count & (count - 1)) == 0) {
			std::unique_ptr<task_ptr[]> ptr(new task_ptr[count * 2]);
			std::move(data.vector_data.get(), data.vector_data.get() + count, ptr.get());
			ptr[count++] = std::move(t);
			data.vector_data = std::move(ptr);
		}
	}

	void clear()
	{
		if (count > 1) {
			data.vector_data.~unique_ptr();
			new(&data.inline_data) task_ptr;
		} else
			data.inline_data = nullptr;
		count = 0;
	}

	size_t size() const
	{
		return count;
	}

	~continuation_vector()
	{
		if (count > 1)
			data.vector_data.~unique_ptr();
		else
			data.inline_data.~task_ptr();
	}

private:
	size_t count{0};

	union data_union {
		data_union(): inline_data() {}

		// Destruction is handled by the parent class
		~data_union() {}

		// Inline continuation used for common case (only one continuation)
		task_ptr inline_data;

		// Vector of continuations. The capacity is the lowest power of 2
		// which is >= count.
		std::unique_ptr<task_ptr[]> vector_data;
	} data;
};

// Type-generic base task object
struct task_base: public ref_count_base<task_base> {
	// Task state
	std::atomic<task_state> state{task_state::TASK_PENDING};

	// Whether this task should be run even if the parent was canceled
	bool always_cont;

	// Vector of continuations and lock protecting it
	spinlock lock;
	continuation_vector continuations;

	// Scheduler to be used to schedule this task
	scheduler* sched;

	// Exception associated with the task if it was canceled
	std::exception_ptr except;

	// Virtual destructor so result gets destroyed properly
	virtual ~task_base() {}

	// Execution function called by the task scheduler. A default implementation
	// is provided for event_task.
	virtual void execute() {}

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
	template<bool cancel> void run_continuations()
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

	// Cancel the task with an exception. This function is virtual so that
	// the associated function object can be freed (it is not needed anymore).
	virtual void cancel(std::exception_ptr cancel_exception)
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
		}
		std::atomic_thread_fence(std::memory_order_acquire);
		return s;
	}

	// Wait and throw the exception if the task was canceled
	void wait_and_throw()
	{
		if (wait() == task_state::TASK_CANCELED) {
			if (except)
				std::rethrow_exception(except);
			else
				throw task_canceled();
		}
	}
};

// Result type-specific task object
template<typename Result> struct task_result: public task_base {
	typename std::aligned_storage<sizeof(Result), std::alignment_of<Result>::value>::type result;

	template<typename T> void set_result(T&& t)
	{
		new(&result) Result(std::forward<T>(t));
	}

	// Return a result using an lvalue or rvalue reference depending on the task
	// type. The task parameter is not used, it is just there for overload resolution.
	template<typename T> Result&& get_result(const task<T>&)
	{
		return std::move(*reinterpret_cast<Result*>(&result));
	}
	template<typename T> const Result& get_result(const shared_task<T>&)
	{
		return *reinterpret_cast<Result*>(&result);
	}

	virtual ~task_result()
	{
		// Result is only present if the task completed successfully
		if (state.load(std::memory_order_relaxed) == task_state::TASK_COMPLETED)
			reinterpret_cast<Result*>(&result)->~Result();
	}
};

// Specialization for references
template<typename Result> struct task_result<Result&>: public task_base {
	// Store as pointer internally
	Result* result;

	void set_result(Result& obj)
	{
		result = std::addressof(obj);
	}

	template<typename T> Result& get_result(const task<T>&)
	{
		return *result;
	}
	template<typename T> Result& get_result(const shared_task<T>&)
	{
		return *result;
	}
};

// Specialization for void
template<> struct task_result<fake_void>: public task_base {
	void set_result(fake_void) {}

	// Get the result as fake_void so that it can be passed to set_result and
	// continuations
	template<typename T> fake_void get_result(const task<T>&)
	{
		return fake_void();
	}
	template<typename T> fake_void get_result(const shared_task<T>&)
	{
		return fake_void();
	}
};

// Class to hold a function object and initialize/destroy it
template<typename Func, typename = void> struct func_holder {
	typename std::aligned_storage<sizeof(Func), std::alignment_of<Func>::value>::type func;

	Func& get_func()
	{
		return *reinterpret_cast<Func*>(&func);
	}
	void init_func(Func&& f)
	{
		new(&func) Func(std::move(f));
	}
	void destroy_func()
	{
		get_func().~Func();
	}
};

// Specialization for empty function objects
template<typename Func> struct func_holder<Func, typename std::enable_if<std::is_empty<Func>::value>::type> {
	Func& get_func()
	{
		return *reinterpret_cast<Func*>(this);
	}
	void init_func(Func&& f)
	{
		new(this) Func(std::move(f));
	}
	void destroy_func()
	{
		get_func().~Func();
	}
};

// Task object with an associated function object
// Using private inheritance so empty Func doesn't take up space
template<typename Func, typename Result> struct task_func: public task_result<Result>, private func_holder<Func> {
	explicit task_func(Func&& f)
	{
		this->init_func(std::move(f));
	}

	// Execution function called by the scheduler
	virtual void execute() override final
	{
		try {
			// Dispatch to execution function
			this->get_func()(this);

			// If we successfully ran, destroy the function object so that it
			// can release any references (shared_ptr) it holds. Behaviour is
			// undefined if the destructor throws.
			this->destroy_func();
		} catch (task_canceled) {
			// Optimize task_canceled by encoding it as a null exception_ptr
			cancel(nullptr);
		} catch (...) {
			cancel(std::current_exception());
		}
	}

	// Destroy the function when being canceled
	virtual void cancel(std::exception_ptr cancel_exception) override final
	{
		this->destroy_func();
		task_base::cancel(std::move(cancel_exception));
	}
};

// Helper function to access the internal_task member of a task object, which
// avoids us having to specify half of the functions in the detail namespace
// as friend. Also, internal_task is downcast to the appropriate task_result<>.
template<typename Task> typename Task::internal_task_type* get_internal_task(const Task& t)
{
	return static_cast<typename Task::internal_task_type*>(t.internal_task.get());
}

// Common code for task unwrapping
template<typename Result, typename Func, typename Child> struct unwrapped_func {
	task_ptr parent_task;
	unwrapped_func(task_ptr t): parent_task(std::move(t)) {}
	void operator()(Child child_task) const
	{
		// Forward completion state and result to parent task
		try {
			if (get_internal_task(child_task)->state.load(std::memory_order_relaxed) == task_state::TASK_COMPLETED) {
				static_cast<task_result<Result>*>(parent_task.get())->set_result(get_internal_task(child_task)->get_result(child_task));
				parent_task->finish();
			} else
				static_cast<task_func<Func, Result>*>(parent_task.get())->cancel(get_internal_task(child_task)->except);
		} catch (...) {
			// If the copy/move constructor of the result threw, propagate the exception
			static_cast<task_func<Func, Result>*>(parent_task.get())->cancel(std::current_exception());
		}
	}
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
template<typename Result, typename Func, bool Unwrap> struct root_exec_func: private Func {
	template<typename F> root_exec_func(F&& f): Func(std::forward<F>(f)) {}
	void operator()(task_base* t)
	{
		static_cast<task_result<Result>*>(t)->set_result(invoke_fake_void(std::move(*static_cast<Func*>(this))));
		t->finish();
	}
};
template<typename Result, typename Func> struct root_exec_func<Result, Func, true>: private Func {
	template<typename F> root_exec_func(F&& f): Func(std::forward<F>(f)) {}
	void operator()(task_base* t)
	{
		unwrapped_finish<Result, root_exec_func>(t, std::move(std::move(*static_cast<Func*>(this)))());
	}
};

// Execution functions for continuation tasks:
// - With and without task unwraping
// - For value-based and task-based continuations
template<typename Parent, typename Result, typename Func, bool ValueCont, bool Unwrap> struct continuation_exec_func: private Func {
	template<typename F, typename P> continuation_exec_func(F&& f, P&& p): Func(std::forward<F>(f)), parent(std::forward<P>(p)) {}
	void operator()(task_base* t)
	{
		static_cast<task_result<Result>*>(t)->set_result(invoke_fake_void(std::move(*static_cast<Func*>(this)), std::move(this->parent)));
		t->finish();
	}
	Parent parent;
};
template<typename Parent, typename Result, typename Func> struct continuation_exec_func<Parent, Result, Func, true, false>: private Func {
	template<typename F, typename P> continuation_exec_func(F&& f, P&& p): Func(std::forward<F>(f)), parent(std::forward<P>(p)) {}
	void operator()(task_base* t)
	{
		auto&& result = get_internal_task(parent)->get_result(parent);
		static_cast<task_result<Result>*>(t)->set_result(invoke_fake_void(std::move(*static_cast<Func*>(this)), std::forward<decltype(result)>(result)));
		t->finish();
	}
	Parent parent;
};
template<typename Parent, typename Result, typename Func> struct continuation_exec_func<Parent, Result, Func, false, true>: private Func {
	template<typename F, typename P> continuation_exec_func(F&& f, P&& p): Func(std::forward<F>(f)), parent(std::forward<P>(p)) {}
	void operator()(task_base* t)
	{
		typedef typename detail::void_to_fake_void<typename Parent::result_type>::type internal_result;
		unwrapped_finish<internal_result, continuation_exec_func>(t, invoke_fake_void(std::move(*static_cast<Func*>(this)), std::move(parent)));
	}
	Parent parent;
};
template<typename Parent, typename Result, typename Func> struct continuation_exec_func<Parent, Result, Func, true, true>: private Func {
	template<typename F, typename P> continuation_exec_func(F&& f, P&& p): Func(std::forward<F>(f)), parent(std::forward<P>(p)) {}
	void operator()(task_base* t)
	{
		auto&& result = get_internal_task(parent)->get_result(parent);
		typedef typename detail::void_to_fake_void<typename Parent::result_type>::type internal_result;
		unwrapped_finish<internal_result, continuation_exec_func>(t, invoke_fake_void(std::move(*static_cast<Func*>(this)), std::forward<decltype(result)>(result)));
	}
	Parent parent;
};

} // namespace detail
} // namespace async
