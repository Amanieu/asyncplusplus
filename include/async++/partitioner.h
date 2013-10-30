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

// Partitioners are essentially ranges with an extra split() function. The
// split() function returns a partitioner containing a range to be executed in a
// child task and modifies the parent partitioner's range to represent the rest
// of the original range. If the range cannot be split any more then split()
// should return an empty range.

// Detect whether a range is a partitioner
template<typename T, typename = void>
struct is_partitioner: public std::false_type {};
template<typename T>
struct is_partitioner<T, decltype((void)std::declval<T>().split())>: public std::true_type {};

// Automatically determine a grain size for a sequence length
inline std::size_t auto_grain_size(std::size_t dist)
{
	// Determine the grain size automatically using a heuristic
	std::size_t num_threads = std::thread::hardware_concurrency();
	if (num_threads == 0)
		num_threads = 1;
	std::size_t grain = dist / (8 * num_threads);
	if (grain < 1)
		grain = 1;
	if (grain > 2048)
		grain = 2048;
	return grain;
}

template<typename Iter>
class static_partitioner_impl {
public:
	static_partitioner_impl(Iter begin, Iter end, std::size_t grain)
		: iter_begin(begin), iter_end(end), grain(grain) {}
	Iter begin() const
	{
		return iter_begin;
	}
	Iter end() const
	{
		return iter_end;
	}
	static_partitioner_impl split()
	{
		// Don't split if below grain size
		std::size_t length = std::distance(iter_begin, iter_end);
		static_partitioner_impl out(iter_begin, iter_begin, grain);
		if (length <= grain)
			return out;

		// Split our range in half
		std::advance(iter_begin, length / 2);
		out.iter_end = iter_begin;
		return out;
	}

private:
	Iter iter_begin, iter_end;
	std::size_t grain;
};

template<typename Iter>
class auto_partitioner_impl {
public:
	// thread_id is initialized to "no thread" and will be set on first split
	auto_partitioner_impl(Iter begin, Iter end, std::size_t grain)
		: iter_begin(begin), iter_end(end), grain(grain) {}
	Iter begin() const
	{
		return iter_begin;
	}
	Iter end() const
	{
		return iter_end;
	}
	auto_partitioner_impl split()
	{
		// Don't split if below grain size
		std::size_t length = std::distance(iter_begin, iter_end);
		auto_partitioner_impl out(iter_begin, iter_begin, grain);
		if (length <= grain)
			return out;

		// Check if we are in a different thread than we were before
		std::thread::id current_thread = std::this_thread::get_id();
		if (current_thread != last_thread)
			num_threads = std::thread::hardware_concurrency();

		// If we only have one thread, don't split
		if (num_threads <= 1)
			return out;

		// Split our range in half
		std::advance(iter_begin, length / 2);
		out.iter_end = iter_begin;
		out.last_thread = current_thread;
		last_thread = current_thread;
		out.num_threads = num_threads / 2;
		num_threads -= out.num_threads;
		return out;
	}

private:
	Iter iter_begin, iter_end;
	std::size_t grain;
	std::size_t num_threads;
	std::thread::id last_thread;
};

} // namespace detail

// A simple partitioner which splits until a grain size is reached. If a grain
// size is not specified, one is chosen automatically.
template<typename Range>
detail::static_partitioner_impl<decltype(std::begin(std::declval<Range>()))> static_partitioner(Range&& range, std::size_t grain)
{
	return {std::begin(range), std::end(range), grain};
}
template<typename Range>
detail::static_partitioner_impl<decltype(std::begin(std::declval<Range>()))> static_partitioner(Range&& range)
{
	std::size_t grain = detail::auto_grain_size(std::distance(std::begin(range), std::end(range)));
	return {std::begin(range), std::end(range), grain};
}

// A more advanced partitioner which initially divides the range into one chunk
// for each available thread. The range is split further if a chunk gets stolen
// by a different thread.
template<typename Range>
detail::auto_partitioner_impl<decltype(std::begin(std::declval<Range>()))> auto_partitioner(Range&& range)
{
	std::size_t grain = detail::auto_grain_size(std::distance(std::begin(range), std::end(range)));
	return {std::begin(range), std::end(range), grain};
}

// Wrap a range in a partitioner. If the input is already a partitioner then it
// is returned unchanged. This allows parallel algorithms to accept both ranges
// and partitioners as parameters.
template<typename Partitioner, typename std::enable_if<detail::is_partitioner<typename std::decay<Partitioner>::type>::value, int>::type = 0>
Partitioner&& to_partitioner(Partitioner&& partitioner)
{
	return std::forward<Partitioner>(partitioner);
}
template<typename Range, typename std::enable_if<!detail::is_partitioner<typename std::decay<Range>::type>::value, int>::type = 0>
decltype(async::auto_partitioner(std::declval<Range>())) to_partitioner(Range&& range)
{
	return async::auto_partitioner(std::forward<Range>(range));
}

} // namespace async
