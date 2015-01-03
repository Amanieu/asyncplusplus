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

// when_all shared state for ranges
template<typename T>
struct when_all_state_range: public ref_count_base<when_all_state_range<T>> {
	typedef std::vector<T> task_type;
	event_task<task_type> event;
	task_type results;

	when_all_state_range(std::size_t count)
		: ref_count_base<when_all_state_range<T>>(count), results(count) {}

	// When all references are dropped, signal the event
	~when_all_state_range()
	{
		event.set(std::move(results));
	}

	template<typename U>
	void set(std::size_t i, U&& u)
	{
		results[i] = std::forward<U>(u);
	}

	static task<task_type> empty_range()
	{
		return async::make_task(task_type());
	}
};
template<>
struct when_all_state_range<void>: public ref_count_base<when_all_state_range<void>> {
	typedef void task_type;
	event_task<void> event;

	when_all_state_range(std::size_t count)
		: ref_count_base(count) {}

	// When all references are dropped, signal the event
	~when_all_state_range()
	{
		event.set();
	}

	void set(std::size_t, fake_void) {}

	static task<task_type> empty_range()
	{
		return make_task();
	}
};

// when_all shared state for varidic arguments
template<typename Tuple>
struct when_all_state_variadic: public ref_count_base<when_all_state_variadic<Tuple>> {
	event_task<Tuple> event;
	Tuple results;

	when_all_state_variadic()
		: ref_count_base<when_all_state_variadic<Tuple>>(std::tuple_size<Tuple>::value) {}

	// When all references are dropped, signal the event
	~when_all_state_variadic()
	{
		// Catch any potential exceptions from a move constructor
		LIBASYNC_TRY {
			event.set(std::move(results));
		} LIBASYNC_CATCH(...) {
			event.set_exception(std::current_exception());
		}
	}
};

// when_any shared state
template<typename T>
struct when_any_state: public ref_count_base<when_any_state<T>> {
	typedef std::pair<std::size_t, T> task_type;
	event_task<task_type> event;

	when_any_state(std::size_t count)
		: ref_count_base<when_any_state<T>>(count) {}

	template<typename U>
	void set(std::size_t i, U&& u)
	{
		event.set(std::make_pair(i, std::forward<U>(u)));
	}
};
template<>
struct when_any_state<void>: public ref_count_base<when_any_state<void>> {
	typedef std::size_t task_type;
	event_task<task_type> event;

	when_any_state(std::size_t count)
		: ref_count_base(count) {}

	void set(std::size_t i, fake_void)
	{
		event.set(i);
	}
};

// Internal implementation of when_all for variadic arguments
template<std::size_t index, typename State>
void when_all_variadic(when_all_state_variadic<State>*) {}
template<std::size_t index, typename State, typename First, typename... T>
void when_all_variadic(when_all_state_variadic<State>* state_ptr, First&& first, T&&... tasks)
{
	// Add a continuation to the task
	LIBASYNC_TRY {
		first.then(inline_scheduler(), [state_ptr](typename std::decay<First>::type t) {
			detail::ref_count_ptr<when_all_state_variadic<State>> state(state_ptr);
			LIBASYNC_TRY {
				if (detail::get_internal_task(t)->state.load(std::memory_order_relaxed) == detail::task_state::completed)
					std::get<index>(state->results) = detail::get_internal_task(t)->get_result(t);
				else
					state->event.set_exception(detail::get_internal_task(t)->except);
			} LIBASYNC_CATCH(...) {
				// If the assignment of the result threw, propagate the exception
				state->event.set_exception(std::current_exception());
			}
		});
	} LIBASYNC_CATCH(...) {
		// Make sure we don't leak memory if then() throws
		state_ptr->release(sizeof...(T) + 1);
		LIBASYNC_RETHROW();
	}

	// Add continuations to rest of tasks
	detail::when_all_variadic<index + 1>(state_ptr, std::forward<T>(tasks)...);
}

// Internal implementation of when_any for variadic arguments
template<std::size_t index, typename State>
void when_any_variadic(when_any_state<State>*) {}
template<std::size_t index, typename State, typename First, typename... T>
void when_any_variadic(when_any_state<State>* state_ptr, First&& first, T&&... tasks)
{
	static_assert(std::is_same<State, typename std::decay<First>::type::result_type>::value, "All tasks given to when_any must have the same result type");

	// Add a continuation to the task
	LIBASYNC_TRY {
		first.then(inline_scheduler(), [state_ptr](typename std::decay<First>::type t) {
			detail::ref_count_ptr<when_any_state<State>> state(state_ptr);
			LIBASYNC_TRY {
				if (detail::get_internal_task(t)->state.load(std::memory_order_relaxed) == detail::task_state::completed)
					state->set(index, detail::get_internal_task(t)->get_result(t));
				else
					state->event.set_exception(detail::get_internal_task(t)->except);
			} LIBASYNC_CATCH(...) {
				// If the copy/move constructor of the result threw, propagate the exception
				state->event.set_exception(std::current_exception());
			}
		});
	} LIBASYNC_CATCH(...) {
		// Make sure we don't leak memory if then() throws
		state_ptr->release(sizeof...(T) + 1);
		LIBASYNC_RETHROW();
	}

	// Add continuations to rest of tasks
	detail::when_any_variadic<index + 1>(state_ptr, std::forward<T>(tasks)...);
}

} // namespace detail

// Alias for fake_void, used in variadic when_all
typedef detail::fake_void void_;

// Combine a set of tasks into one task which is signaled when all specified tasks finish
template<typename Iter>
task<typename detail::when_all_state_range<typename std::iterator_traits<Iter>::value_type::result_type>::task_type> when_all(Iter begin, Iter end)
{
	typedef typename std::iterator_traits<Iter>::value_type task_type;
	typedef typename task_type::result_type result_type;

	// Handle empty range
	if (begin == end)
		return detail::when_all_state_range<result_type>::empty_range();

	// Create shared state
	auto state_ptr = new detail::when_all_state_range<result_type>(std::distance(begin, end));
	auto out = state_ptr->event.get_task();

	// Add a continuation to each task to add its result to the shared state
	// Last task sets the event result
	for (std::size_t i = 0; begin != end; i++, ++begin) {
		LIBASYNC_TRY {
			(*begin).then(inline_scheduler(), [state_ptr, i](task_type t) {
				detail::ref_count_ptr<detail::when_all_state_range<result_type>> state(state_ptr);
				LIBASYNC_TRY {
					if (detail::get_internal_task(t)->state.load(std::memory_order_relaxed) == detail::task_state::completed)
						state->set(i, detail::get_internal_task(t)->get_result(t));
					else
						state->event.set_exception(detail::get_internal_task(t)->except);
				} LIBASYNC_CATCH(...) {
					// If the assignment of the result threw, propagate the exception
					state->event.set_exception(std::current_exception());
				}
			});
		} LIBASYNC_CATCH(...) {
			// Make sure we don't leak memory if then() throws
			state_ptr->release(std::distance(begin, end));
			LIBASYNC_RETHROW();
		}
	}

	return out;
}

// Combine a set of tasks into one task which is signaled when one of the tasks finishes
template<typename Iter>
task<typename detail::when_any_state<typename std::iterator_traits<Iter>::value_type::result_type>::task_type> when_any(Iter begin, Iter end)
{
	typedef typename std::iterator_traits<Iter>::value_type task_type;
	typedef typename task_type::result_type result_type;

#ifndef NDEBUG
	// Handle empty range
	if (begin == end)
		LIBASYNC_THROW(std::invalid_argument("when_any called with empty range"));
#endif

	// Create shared state
	auto* state_ptr = new detail::when_any_state<result_type>(std::distance(begin, end));
	auto out = state_ptr->event.get_task();

	// Add a continuation to each task to set the event. First one wins.
	for (std::size_t i = 0; begin != end; i++, ++begin) {
		LIBASYNC_TRY {
			(*begin).then(inline_scheduler(), [state_ptr, i](task_type t) {
				detail::ref_count_ptr<detail::when_any_state<result_type>> state(state_ptr);
				LIBASYNC_TRY {
					if (detail::get_internal_task(t)->state.load(std::memory_order_relaxed) == detail::task_state::completed)
						state->set(i, detail::get_internal_task(t)->get_result(t));
					else
						state->event.set_exception(detail::get_internal_task(t)->except);
				} LIBASYNC_CATCH(...) {
					// If the copy/move constructor of the result threw, propagate the exception
					state->event.set_exception(std::current_exception());
				}
			});
		} LIBASYNC_CATCH(...) {
			// Make sure we don't leak memory if then() throws
			state_ptr->release(std::distance(begin, end));
			LIBASYNC_RETHROW();
		}
	}

	return out;
}

// when_all wrapper accepting ranges
template<typename T>
decltype(async::when_all(std::begin(std::declval<T>()), std::end(std::declval<T>()))) when_all(T&& tasks)
{
	return async::when_all(std::begin(std::forward<T>(tasks)), std::end(std::forward<T>(tasks)));
}

// when_any wrapper accepting ranges
template<typename T>
decltype(async::when_any(std::begin(std::declval<T>()), std::end(std::declval<T>()))) when_any(T&& tasks)
{
	return async::when_any(std::begin(std::forward<T>(tasks)), std::end(std::forward<T>(tasks)));
}

// when_all with variadic arguments
template<typename First, typename... T>
task<std::tuple<typename detail::void_to_fake_void<typename std::decay<First>::type::result_type>::type, typename detail::void_to_fake_void<typename std::decay<T>::type::result_type>::type...>> when_all(First&& first, T&&... tasks)
{
	typedef std::tuple<typename detail::void_to_fake_void<typename std::decay<First>::type::result_type>::type, typename detail::void_to_fake_void<typename std::decay<T>::type::result_type>::type...> result_type;

	// Create shared state
	auto state = new detail::when_all_state_variadic<result_type>;
	auto out = state->event.get_task();

	// Add continuations to the tasks
	detail::when_all_variadic<0>(state, std::forward<First>(first), std::forward<T>(tasks)...);

	return out;
}

// when_any with variadic arguments
template<typename First, typename... T>
task<typename detail::when_any_state<typename std::decay<First>::type::result_type>::task_type> when_any(First&& first, T&&... tasks)
{
	typedef typename std::decay<First>::type::result_type result_type;

	// Create shared state
	auto state = new detail::when_any_state<result_type>(sizeof...(tasks) + 1);
	auto out = state->event.get_task();

	// Add continuations to the tasks
	detail::when_any_variadic<0>(state, std::forward<First>(first), std::forward<T>(tasks)...);

	return out;
}

} // namespace async
