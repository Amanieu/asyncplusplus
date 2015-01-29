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
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

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

// Support compiling without exceptions
#ifndef LIBASYNC_NO_EXCEPTIONS
# ifdef __clang__
#  if !__has_feature(cxx_exceptions)
#   define LIBASYNC_NO_EXCEPTIONS
#  endif
# elif defined(__GNUC__) && !defined(__EXCEPTIONS)
#  define LIBASYNC_NO_EXCEPTIONS
# endif
#endif
#ifdef LIBASYNC_NO_EXCEPTIONS
# define LIBASYNC_THROW(...) std::abort()
# define LIBASYNC_RETHROW() do {} while (false)
# define LIBASYNC_TRY if (true)
# define LIBASYNC_CATCH(...) else if (false)
#else
# define LIBASYNC_THROW(...) throw __VA_ARGS__
# define LIBASYNC_RETHROW() throw
# define LIBASYNC_TRY try
# define LIBASYNC_CATCH(...) catch (__VA_ARGS__)
#endif

// Annotate move constructors and move assignment with noexcept to allow objects
// to be moved if they are in containers. Compilers which don't support noexcept
// will usually move regardless.
#ifdef __GNUC__
#define LIBASYNC_NOEXCEPT noexcept
#else
#define LIBASYNC_NOEXCEPT
#endif

// Cacheline alignment to avoid false sharing between different threads
#define LIBASYNC_CACHELINE_SIZE 64
#ifdef __GNUC__
# define LIBASYNC_CACHELINE_ALIGN __attribute__((aligned(LIBASYNC_CACHELINE_SIZE)))
#elif defined(_MSC_VER)
# define LIBASYNC_CACHELINE_ALIGN __declspec(align(LIBASYNC_CACHELINE_SIZE))
#else
# define LIBASYNC_CACHELINE_ALIGN alignas(LIBASYNC_CACHELINE_SIZE)
#endif

// Set this to override the default scheduler for newly created tasks.
// async::threadpool_scheduler() is used by default.
#ifndef LIBASYNC_DEFAULT_SCHEDULER
# define LIBASYNC_DEFAULT_SCHEDULER async::default_scheduler()
#endif

// Force symbol visibility to hidden unless explicity exported
#if defined(__GNUC__) && !defined(_WIN32)
#pragma GCC visibility push(hidden)
#endif

// Some forward declarations
namespace async {

template<typename Result>
class task;
template<typename Result>
class shared_task;
template<typename Result>
class event_task;

} // namespace async

// Include sub-headers
#include "async++/traits.h"
#include "async++/aligned_alloc.h"
#include "async++/ref_count.h"
#include "async++/scheduler_fwd.h"
#include "async++/task_base.h"
#include "async++/scheduler.h"
#include "async++/task.h"
#include "async++/when_all_any.h"
#include "async++/cancel.h"
#include "async++/range.h"
#include "async++/partitioner.h"
#include "async++/parallel_invoke.h"
#include "async++/parallel_for.h"
#include "async++/parallel_reduce.h"
#include "async++/fifo_queue.h"
#include "async++/work_steal_queue.h"

#if defined(__GNUC__) && !defined(_WIN32)
#pragma GCC visibility pop
#endif

#endif
