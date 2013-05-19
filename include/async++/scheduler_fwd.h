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

// Scheduler interface
class task_run_handle;
class scheduler {
public:
	// Schedule a task for execution.
	virtual void schedule(task_run_handle t) = 0;
};

// Run a task in a thread pool. This scheduler will wait for all tasks to finish
// at program exit.
LIBASYNC_EXPORT scheduler& threadpool_scheduler();

// Run a task directly
LIBASYNC_EXPORT scheduler& inline_scheduler();

// Run a task in a separate thread. Note that this scheduler does not wait for
// threads to finish at process exit. You must ensure that all threads finish
// before ending the process.
LIBASYNC_EXPORT scheduler& thread_scheduler();

// Wait handler function prototype
class task_wait_handle;
typedef void (*wait_handler)(task_wait_handle t);

// Set a wait handler to control what a task does when it has "free time", which
// is when it is waiting for another task to complete. The wait handler can do
// other work, but should return when it detects that the task has completed.
// The previously installed handler is returned.
LIBASYNC_EXPORT wait_handler set_thread_wait_handler(wait_handler w);

namespace detail {

// Reference counted pointer to task data
struct task_base;
typedef ref_count_ptr<task_base> task_ptr;

// Helper function to schedule a task using a scheduler
void schedule_task(scheduler& sched, task_ptr t);

// Wait for the given task to finish. This will call the wait handler currently
// active for this thread, which causes the thread to sleep by default.
LIBASYNC_EXPORT void wait_for_task(task_base* wait_task);

} // namespace detail
} // namespace async
