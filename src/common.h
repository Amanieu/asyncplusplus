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

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <thread>
#include <type_traits>
#include <vector>

#include <async++.h>

// For posix_memalign/_aligned_malloc
#ifdef _WIN32
# include <malloc.h>
# ifdef __MINGW32__
#  define _aligned_malloc __mingw_aligned_malloc
#  define _aligned_free __mingw_aligned_free
# endif
#else
# include <stdlib.h>
#endif

// thread_local keyword support
#ifdef __clang__
# if __has_feature(cxx_thread_local)
#  define HAVE_THREAD_LOCAL
# endif
#elif !defined(__INTEL_COMPILER) && __GNUC__ * 100 + __GNUC_MINOR__ >= 408
# define HAVE_THREAD_LOCAL
#endif

// For compilers that don't support thread_local, use __thread/declspec(thread)
// which have the same semantics but doesn't support dynamic initialization/destruction.
#ifndef HAVE_THREAD_LOCAL
# ifdef _MSC_VER
#  define thread_local __declspec(thread)
# else
#  define thread_local __thread
# endif
#endif

// MSVC deadlocks when joining a thread from a static destructor. Use a
// workaround in that case to avoid the deadlock.
#ifdef _MSC_VER
#define BROKEN_JOIN_IN_DESTRUCTOR
#endif

// Force symbol visibility to hidden unless explicity exported
#if defined(__GNUC__) && !defined(_WIN32)
#pragma GCC visibility push(hidden)
#endif

// Include other internal headers
#include "task_wait_event.h"
#include "fifo_queue.h"
#include "work_steal_queue.h"
