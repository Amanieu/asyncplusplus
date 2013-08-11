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

// Automatically determine a grain size for a sequence length
inline size_t auto_grain_size(size_t dist)
{
	// Determine the grain size automatically using a heuristic
	return std::max<std::size_t>(std::min<std::size_t>(2048, dist / (8 * std::thread::hardware_concurrency())), 1);
}

} // namespace detail

// Run a function for each element in a range
template<typename Range, typename Func>
void parallel_for(scheduler& sched, Range&& range, std::size_t grain, const Func& func)
{
	// If the length is less than the grain size, run the loop inline
	auto begin = std::begin(range);
	auto end = std::end(range);
	std::size_t length = std::distance(begin, end);
	if (length <= grain) {
		std::for_each(begin, end, func);
		return;
	}

	// Split the range at its midpoint
	auto mid = begin;
	std::advance(mid, length / 2);

	// Run the function over each half in parallel
	auto&& t = async::local_spawn(sched, [&sched, mid, end, grain, &func] {
		async::parallel_for(sched, async::make_range(mid, end), grain, func);
	});
	async::parallel_for(sched, async::make_range(begin, mid), grain, func);
	t.get();
}

// Overload with implicit grain size
template<typename Range, typename Func>
void parallel_for(scheduler& sched, Range&& range, const Func& func)
{
	std::size_t grain = detail::auto_grain_size(std::distance(std::begin(range), std::end(range)));
	async::parallel_for(sched, range, grain, func);
}

// Overloads with default scheduler
template<typename Range, typename Func>
void parallel_for(Range&& range, std::size_t grain, const Func& func)
{
	async::parallel_for(LIBASYNC_DEFAULT_SCHEDULER, range, grain, func);
}
template<typename Range, typename Func>
void parallel_for(Range&& range, const Func& func)
{
	async::parallel_for(LIBASYNC_DEFAULT_SCHEDULER, range, func);
}

// Overloads with std::initializer_list
template<typename T, typename Func>
void parallel_for(scheduler& sched, std::initializer_list<T> range, std::size_t grain, const Func& func)
{
	async::parallel_for(sched, async::make_range(range.begin(), range.end()), grain, func);
}
template<typename T, typename Func>
void parallel_for(scheduler& sched, std::initializer_list<T> range, const Func& func)
{
	async::parallel_for(sched, async::make_range(range.begin(), range.end()), func);
}
template<typename T, typename Func>
void parallel_for(std::initializer_list<T> range, std::size_t grain, const Func& func)
{
	async::parallel_for(async::make_range(range.begin(), range.end()), grain, func);
}
template<typename T, typename Func>
void parallel_for(std::initializer_list<T> range, const Func& func)
{
	async::parallel_for(async::make_range(range.begin(), range.end()), func);
}

} // namespace async
