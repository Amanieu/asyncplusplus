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

// Queue used to hold tasks from outside the thread pool, in FIFO order
class fifo_queue {
	std::size_t length;
	std::unique_ptr<void*[]> items;
	std::size_t head, tail;

public:
	fifo_queue()
		: length(32), items(new void*[32]), head(0), tail(0) {}

	// Push a task to the end of the queue
	void push(task_run_handle t)
	{
		// Resize queue if it is full
		if (head == ((tail + 1) & (length - 1))) {
			length *= 2;
			std::unique_ptr<void*[]> ptr(new void*[length]);
			for (std::size_t i = 0; i < tail - head; i++)
				ptr[i] = items[(i + head) & (length - 1)];
			items = std::move(ptr);
		}

		// Push the item
		items[tail] = t.to_void_ptr();
		tail = (tail + 1) & (length - 1);
	}

	// Pop a task from the front of the queue
	void* pop()
	{
		// See if an item is available
		if (head == tail)
			return nullptr;
		else {
			void* task = items[head];
			head = (head + 1) & (length - 1);
			return task;
		}
	}
};

} // namespace detail
} // namespace async
