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

namespace async {
namespace detail {

// Chase-Lev work stealing deque
//
// Dynamic Circular Work-Stealing Deque
// http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.170.1097&rep=rep1&type=pdf
//
// Correct and Efﬁcient Work-Stealing for Weak Memory Models
// http://www.di.ens.fr/~zappa/readings/ppopp13.pdf
class work_steal_queue {
	// Circular array of atomic<void*>
	class circular_array {
	public:
		circular_array(std::size_t n)
			: items(n) {}

		std::size_t size() const
		{
			return items.size();
		}

		void* get(std::size_t index)
		{
			return items[index & (size() - 1)].load(std::memory_order_relaxed);
		}

		void put(std::size_t index, void* x)
		{
			items[index & (size() - 1)].store(x, std::memory_order_relaxed);
		}

		// Growing the array returns a new circular_array object and keeps a
		// linked list of all previous arrays. This is done because other threads
		// could still be accessing elements from the smaller arrays.
		circular_array* grow(std::size_t top, std::size_t bottom)
		{
			circular_array* new_array = new circular_array(size() * 2);
			new_array->previous.reset(this);
			for (std::size_t i = top; i != bottom; i++)
				new_array->put(i, get(i));
			return new_array;
		}

	private:
		aligned_array<std::atomic<void*>, LIBASYNC_CACHELINE_SIZE> items;
		std::unique_ptr<circular_array> previous;
	};

	std::atomic<circular_array*> array;
	std::atomic<std::size_t> top, bottom;

	// Convert a 2's complement unsigned value to a signed value. We need to do
	// this because (b - t) may not always be positive.
	static std::ptrdiff_t to_signed(std::size_t x)
	{
		// Unsigned to signed conversion is implementation-defined if the value
		// doesn't fit, so we convert manually.
		static_assert(static_cast<std::size_t>(PTRDIFF_MAX) + 1 == static_cast<std::size_t>(PTRDIFF_MIN), "Wrong integer wrapping behavior");
		if (x > static_cast<std::size_t>(PTRDIFF_MAX))
			return static_cast<std::ptrdiff_t>(x - PTRDIFF_MIN) + PTRDIFF_MIN;
		else
			return static_cast<std::ptrdiff_t>(x);
	}

public:
	// Initialize the indices to 1 so that popping an empty queue does not cause
	// an integer overflow when decrementing.
	work_steal_queue()
		: array(new circular_array(32)), top(1), bottom(1) {}
	~work_steal_queue()
	{
		delete array.load(std::memory_order_relaxed);
	}

	// Push a task to the bottom of this thread's queue
	void push(task_run_handle x)
	{
		std::size_t b = bottom.load(std::memory_order_relaxed);
		std::size_t t = top.load(std::memory_order_relaxed);
		circular_array* a = array.load(std::memory_order_relaxed);

		// Grow the array if it is full
		if (to_signed(b - t) >= to_signed(a->size())) {
			a = a->grow(t, b);
			array.store(a, std::memory_order_release);
		} else
			std::atomic_thread_fence(std::memory_order_acquire);

		// Note that we only convert to void* here in case grow throws due to
		// lack of memory.
		a->put(b, x.to_void_ptr());
		bottom.store(b + 1, std::memory_order_release);
	}

	// Pop a task from the bottom of this thread's queue
	void* pop()
	{
		std::size_t b = bottom.load(std::memory_order_relaxed);

		// Early exit if queue is empty
		std::size_t t = top.load(std::memory_order_relaxed);
		if (to_signed(b - t) <= 0)
			return nullptr;

		// Make sure bottom is stored before top is read
		bottom.store(--b, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		t = top.load(std::memory_order_relaxed);

		// If the queue is empty, restore bottom and exit
		if (to_signed(b - t) < 0) {
			bottom.store(b + 1, std::memory_order_relaxed);
			return nullptr;
		}

		// Fetch the element from the queue
		circular_array* a = array.load(std::memory_order_relaxed);
		void* x = a->get(b);

		// If this was the last element in the queue, check for races
		if (b == t) {
			if (!top.compare_exchange_strong(t, t + 1, std::memory_order_relaxed, std::memory_order_relaxed))
				x = nullptr;
			bottom.store(b + 1, std::memory_order_relaxed);
		}
		return x;
	}

	// Steal a task from the top of this thread's queue
	void* steal()
	{
		// Loop while the compare_exchange fails. This is still lock-free because
		// a fail means that another thread has sucessfully stolen a task.
		while (true) {
			// Make sure top is read before bottom
			std::size_t t = top.load(std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			std::size_t b = bottom.load(std::memory_order_relaxed);

			// Exit if the queue is empty
			if (to_signed(b - t) <= 0)
				return nullptr;

			// Fetch the element from the queue
			std::atomic_thread_fence(std::memory_order_acquire);
			circular_array* a = array.load(std::memory_order_consume);
			void* x = a->get(t);

			// Attempt to increment top
			if (top.compare_exchange_weak(t, t + 1, std::memory_order_release, std::memory_order_relaxed))
				return x;
		}
	}
};

} // namespace detail
} // namespace async
