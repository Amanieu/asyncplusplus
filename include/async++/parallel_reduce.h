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

// Default map function which simply passes its parameter through unmodified
struct default_map {
	template<typename T> T&& operator()(T&& x) const
	{
		return std::forward<T>(x);
	}
};

} // namespace detail

// Run a function for each element in a range and then reduce the results of that function to a single value
template<typename Range, typename Result, typename MapFunc, typename ReduceFunc>
Result parallel_reduce(scheduler& sched, Range&& range, const Result& init, std::size_t grain, const MapFunc& map, const ReduceFunc& reduce)
{
	// If the length is less than the grain size, run the loop inline
	auto begin = std::begin(range);
	auto end = std::end(range);
	std::size_t length = std::distance(begin, end);
	if (length <= grain) {
		Result out(init);
		for (auto&& i: range)
			out = reduce(out, map(i));
		return out;
	}

	// Split the range at its midpoint
	auto mid = begin;
	std::advance(mid, length / 2);

	// Run the function over each half in parallel
	auto&& t = async::local_spawn(sched, [&sched, mid, end, &init, grain, &map, &reduce] {
		return async::parallel_reduce(sched, async::make_range(mid, end), init, grain, map, reduce);
	});
	return reduce(async::parallel_reduce(sched, async::make_range(begin, mid), init, grain, map, reduce), t.get());
}

// Overload with implicit grain size
template<typename Range, typename Result, typename MapFunc, typename ReduceFunc>
Result parallel_reduce(scheduler& sched, Range&& range, const Result& init, const MapFunc& map, const ReduceFunc& reduce)
{
	std::size_t grain = detail::auto_grain_size(std::distance(std::begin(range), std::end(range)));
	return async::parallel_reduce(sched, range, init, grain, map, reduce);
}

// Overloads with identity map function
template<typename Range, typename Result, typename ReduceFunc>
Result parallel_reduce(scheduler& sched, Range&& range, const Result& init, std::size_t grain, const ReduceFunc& reduce)
{
	return async::parallel_reduce(sched, range, init, grain, detail::default_map(), reduce);
}
template<typename Range, typename Result, typename ReduceFunc>
Result parallel_reduce(scheduler& sched, Range&& range, const Result& init, const ReduceFunc& reduce)
{
	return async::parallel_reduce(sched, range, init, detail::default_map(), reduce);
}

// Overloads with default scheduler
template<typename Range, typename Result, typename MapFunc, typename ReduceFunc>
Result parallel_reduce(Range&& range, const Result& init, std::size_t grain, const MapFunc& map, const ReduceFunc& reduce)
{
	return async::parallel_reduce(LIBASYNC_DEFAULT_SCHEDULER, range, init, grain, map, reduce);
}
template<typename Range, typename Result, typename MapFunc, typename ReduceFunc>
Result parallel_reduce(Range&& range, const Result& init, const MapFunc& map, const ReduceFunc& reduce)
{
	return async::parallel_reduce(LIBASYNC_DEFAULT_SCHEDULER, range, init, map, reduce);
}
template<typename Range, typename Result, typename ReduceFunc>
Result parallel_reduce(Range&& range, const Result& init, std::size_t grain, const ReduceFunc& reduce)
{
	return async::parallel_reduce(LIBASYNC_DEFAULT_SCHEDULER, range, init, grain, reduce);
}
template<typename Range, typename Result, typename ReduceFunc>
Result parallel_reduce(Range&& range, const Result& init, const ReduceFunc& reduce)
{
	return async::parallel_reduce(LIBASYNC_DEFAULT_SCHEDULER, range, init, reduce);
}

// Overloads with std::initializer_list
template<typename T, typename Result, typename MapFunc, typename ReduceFunc>
Result parallel_reduce(scheduler& sched, std::initializer_list<T> range, const Result& init, std::size_t grain, const MapFunc& map, const ReduceFunc& reduce)
{
	return async::parallel_reduce(sched, async::make_range(range.begin(), range.end()), init, grain, map, reduce);
}
template<typename T, typename Result, typename MapFunc, typename ReduceFunc>
Result parallel_reduce(scheduler& sched, std::initializer_list<T> range, const Result& init, const MapFunc& map, const ReduceFunc& reduce)
{
	return async::parallel_reduce(sched, async::make_range(range.begin(), range.end()), init, map, reduce);
}
template<typename T, typename Result, typename ReduceFunc>
Result parallel_reduce(scheduler& sched, std::initializer_list<T> range, const Result& init, std::size_t grain, const ReduceFunc& reduce)
{
	return async::parallel_reduce(sched, async::make_range(range.begin(), range.end()), init, grain, reduce);
}
template<typename T, typename Result, typename ReduceFunc>
Result parallel_reduce(scheduler& sched, std::initializer_list<T> range, const Result& init, const ReduceFunc& reduce)
{
	return async::parallel_reduce(sched, async::make_range(range.begin(), range.end()), init, reduce);
}
template<typename T, typename Result, typename MapFunc, typename ReduceFunc>
Result parallel_reduce(std::initializer_list<T> range, const Result& init, std::size_t grain, const MapFunc& map, const ReduceFunc& reduce)
{
	return async::parallel_reduce(async::make_range(range.begin(), range.end()), init, grain, map, reduce);
}
template<typename T, typename Result, typename MapFunc, typename ReduceFunc>
Result parallel_reduce(std::initializer_list<T> range, const Result& init, const MapFunc& map, const ReduceFunc& reduce)
{
	return async::parallel_reduce(async::make_range(range.begin(), range.end()), init, map, reduce);
}
template<typename T, typename Result, typename ReduceFunc>
Result parallel_reduce(std::initializer_list<T> range, const Result& init, std::size_t grain, const ReduceFunc& reduce)
{
	return async::parallel_reduce(async::make_range(range.begin(), range.end()), init, grain, reduce);
}
template<typename T, typename Result, typename ReduceFunc>
Result parallel_reduce(std::initializer_list<T> range, const Result& init, const ReduceFunc& reduce)
{
	return async::parallel_reduce(async::make_range(range.begin(), range.end()), init, reduce);
}

} // namespace async
