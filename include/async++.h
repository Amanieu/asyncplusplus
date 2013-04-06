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
#define ASYNCXX_H_

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// SSE intrinsics for _mm_pause
#if defined(__SSE__) || _M_IX86_FP > 0
#include <xmmintrin.h>
#endif

// Export declaration to make symbols visible for dll/so
#ifdef LIBASYNC_STATIC
# define LIBASYNC_EXPORT
#else
# ifdef _WIN32
#  ifdef LIBASYNC_BUILD
#   define LIBASYNC_EXPORT __declspec(dllexport)
#  else
#   define LIBASYNC_EXPORT __declspec(dllimport)
#  endif
# else
#  define LIBASYNC_EXPORT __attribute__((visibility("default")))
# endif
#endif

// Set this to override the default scheduler for newly created tasks. The
// original can still be accessed through async::default_scheduler().
#ifndef LIBASYNC_DEFAULT_SCHEDULER
# define LIBASYNC_DEFAULT_SCHEDULER async::default_scheduler()
#endif

// Force symbol visibility to hidden unless explicity exported
#ifdef __GNUC__
#pragma GCC visibility push(hidden)
#endif

// Some forward declarations
namespace async {

template<typename Result> class task;
template<typename Result> class shared_task;
template<typename Result> class event_task;

// Exception thrown by cancel_current_task()
struct LIBASYNC_EXPORT task_canceled {};

} // namespace async

#include "async++/spinlock.h"
#include "async++/traits.h"
#include "async++/ref_count.h"
#include "async++/scheduler_fwd.h"
#include "async++/task_base.h"
#include "async++/scheduler.h"
#include "async++/task.h"
#include "async++/when_all_any.h"
#include "async++/cancel.h"

#ifdef __GNUC__
#pragma GCC visibility pop
#endif

#endif
