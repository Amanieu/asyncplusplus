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

// Work-stealing queue, contains a list of tasks belonging to a single thread.
// A thread accessing is own queue will use the tail, whereas a thread stealing
// from another thread's queue will use the head. Since tasks tend to split
// themselves into smaller tasks, this allows larger chunks of work to be
// stolen.
class work_steal_queue {
	std::size_t length;
	std::unique_ptr<void*[]> items;
	spinlock lock;
	std::atomic<std::size_t> atomic_head{0}, atomic_tail{0};

public:
	work_steal_queue()
		: length(32), items(new void*[32]) {}

	// Push a task to the tail of this thread's queue
	void push(void* t)
	{
		std::size_t tail = atomic_tail.load(std::memory_order_relaxed);

		// Check if we have space to insert an element at the tail
		if (tail == length) {
			// Lock the queue
			std::lock_guard<spinlock> locked(lock);
			std::size_t head = atomic_head.load(std::memory_order_relaxed);

			// Resize the queue if it is more than 75% full
			if (head <= length / 4) {
				length *= 2;
				std::unique_ptr<void*[]> ptr(new void*[length]);
				std::copy(items.get() + head, items.get() + tail, ptr.get());
				items = std::move(ptr);
			} else {
				// Simply shift the items to free up space at the end
				std::copy(items.get() + head, items.get() + tail, items.get());
			}
			tail -= head;
			atomic_head.store(0, std::memory_order_relaxed);
		}

		// Now add the task
		items[tail] = t;
		atomic_tail.store(tail + 1, std::memory_order_release);
	}

	// Pop a task from the tail of this thread's queue
	void* pop()
	{
		std::size_t tail = atomic_tail.load(std::memory_order_relaxed);

		// Early exit if queue is empty
		if (atomic_head.load(std::memory_order_relaxed) >= tail)
			return nullptr;

		// Make sure tail is stored before we read head
		atomic_tail.store(--tail, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_seq_cst);

		// Race to the queue
		if (atomic_head.load(std::memory_order_relaxed) <= tail)
			return items[tail];

		// There is a concurrent steal or no items, lock the queue and try again
		std::lock_guard<spinlock> locked(lock);

		// Check if the item is still available
		if (atomic_head.load(std::memory_order_relaxed) <= tail)
			return items[tail];

		// Otherwise restore the tail and fail
		atomic_tail.store(tail + 1, std::memory_order_relaxed);
		return nullptr;
	}

	// Steal a task from the head of this thread's queue
	void* steal()
	{
		// Lock the queue to prevent concurrent steals
		std::lock_guard<spinlock> locked(lock);

		// Make sure head is stored before we read tail
		std::size_t head = atomic_head.load(std::memory_order_relaxed);
		atomic_head.store(head + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_seq_cst);

		// Check if there is a task to steal
		if (head < atomic_tail.load(std::memory_order_relaxed)) {
			// Need acquire fence to synchronise with concurrent push
			std::atomic_thread_fence(std::memory_order_acquire);
			return items[head];
		}

		// Otherwise restore the head and fail
		atomic_head.store(head, std::memory_order_relaxed);
		return nullptr;
	}
};

} // namespace detail
} // namespace async
