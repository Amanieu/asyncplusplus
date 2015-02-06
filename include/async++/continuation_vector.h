// Copyright (c) 2015 Amanieu d'Antras
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

// Thread-safe vector of task_ptr which is optimized for the common case of
// only having a single continuation.
class continuation_vector {
	// Heap-allocated data for the slow path
	struct vector_data {
		std::vector<task_base*> vector;
		std::mutex lock;
	};

	// Internal data of the vector
	struct internal_data {
		// If this is true then no more changes are allowed
		bool is_locked;

		// Indicates which element of the union is currently active
		bool is_vector;

		union {
			// Fast path: This represents zero (nullptr) or one elements
			task_base* inline_ptr;

			// Slow path: This is used for two or more elements
			vector_data* vector_ptr;
		};
	};

	// On x86_64, some compilers will not use CMPXCHG16B because some early AMD
	// processors do not implement that instruction. Since these are quite rare,
	// we force the use of CMPXCHG16B unless the user has explicitly asked
	// otherwise using LIBASYNC_NO_CMPXCHG16B.
#if !defined(LIBASYNC_NO_CMPXCHG16B) && \
    ((defined(__GNUC__) && defined(__x86_64__) && !defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16)) || \
     (defined(_MSC_VER) && defined(_M_AMD64)))
	class atomic_data_type {
		// Internal storage for the atomic data
# ifdef __GNUC__
		__uint128_t storage;
# else
#  pragma intrinsic(_InterlockedCompareExchange128)
		std::int64_t storage[2];
# endif
	static_assert(sizeof(internal_data) == 16, "Wrong size for internal_data");

	public:
		// These functions use memcpy to convert between internal_data and
		// integer types to avoid any potential issues with strict aliasing.
		// Tests have shown that compilers are smart enough to optimize the
		// memcpy calls away and produce optimal code.
		atomic_data_type(internal_data data)
		{
			std::memcpy(&storage, &data, sizeof(internal_data));
		}
		internal_data load(std::memory_order)
		{
# ifdef __GNUC__
			std::int64_t value[2];
			__asm__ __volatile__ (
				"movq %%rbx, %%rax\n\t"
				"movq %%rcx, %%rdx\n\t"
				"lock; cmpxchg16b %[storage]"
				: "=&a" (value[0]), "=&d" (value[1])
				: [storage] "m" (storage)
				: "cc", "memory", "rbx", "rcx"
			);
# else
			std::int64_t value[2] = {};
			_InterlockedCompareExchange128(storage, value[0], value[1], value);
# endif
			internal_data result;
			std::memcpy(&result, value, sizeof(internal_data));
			return result;
		}
		bool compare_exchange_weak(internal_data& expected, internal_data desired, std::memory_order, std::memory_order)
		{
			std::int64_t desired_value[2];
			std::memcpy(desired_value, &desired, sizeof(internal_data));
			std::int64_t expected_value[2];
			std::memcpy(expected_value, &expected, sizeof(internal_data));
			bool success;
# ifdef __GNUC__
			__asm__ __volatile__ (
				"lock; cmpxchg16b %[storage]\n\t"
				"sete %[success]"
				: "+a,a" (expected_value[0]), "+d,d" (expected_value[1]), [storage] "+m,m" (storage), [success] "=q,m" (success)
				: "b,b" (desired_value[0]), "c,c" (desired_value[1])
				: "cc", "memory"
			);
# else
			success = _InterlockedCompareExchange128(storage, desired_value[0], desired_value[1], expected_value) != 0;
# endif
			std::memcpy(&expected, expected_value, sizeof(internal_data));
			return success;
		}
	};
#else
	typedef std::atomic<internal_data> atomic_data_type;
#endif

	// All changes to the internal data are atomic
	atomic_data_type atomic_data;

public:
	// Start unlocked with zero elements in the fast path
	continuation_vector()
		: atomic_data(internal_data{false, false, {}}) {}

	// Free any left over data
	~continuation_vector()
	{
		// Converting task_ptr instead of using remove_ref because task_base
		// isn't defined yet at this point.
		internal_data data = atomic_data.load(std::memory_order_relaxed);
		if (!data.is_vector) {
			// If the data is locked then the inline pointer is already gone
			if (!data.is_locked)
				task_ptr tmp(data.inline_ptr);
		} else {
			for (task_base* i: data.vector_ptr->vector)
				task_ptr tmp(i);
			delete data.vector_ptr;
		}
	}

	// Try adding an element to the vector. This fails and returns false if
	// the vector has been locked. In that case t is not modified.
	bool try_add(task_ptr&& t)
	{
		// Cache to avoid re-allocating vector_data multiple times. This is
		// automatically freed if it is not successfully saved to atomic_data.
		std::unique_ptr<vector_data> vector;

		// Compare-exchange loop on atomic_data
		internal_data data = atomic_data.load(std::memory_order_relaxed);
		internal_data new_data;
		new_data.is_locked = false;
		do {
			// Return immediately if the vector is locked
			if (data.is_locked)
				return false;

			if (!data.is_vector) {
				if (!data.inline_ptr) {
					// Going from 0 to 1 elements
					new_data.inline_ptr = t.get();
					new_data.is_vector = false;
				} else {
					// Going from 1 to 2 elements, allocate a vector_data
					if (!vector)
						vector.reset(new vector_data{{data.inline_ptr, t.get()}, {}});
					new_data.vector_ptr = vector.get();
					new_data.is_vector = true;
				}
			} else {
				// Larger vectors use a mutex, so grab the lock
				std::atomic_thread_fence(std::memory_order_acquire);
				std::lock_guard<std::mutex> locked(data.vector_ptr->lock);

				// We need to check again if the vector has been locked here
				// to avoid a race condition with flush_and_lock
				if (atomic_data.load(std::memory_order_relaxed).is_locked)
					return false;

				// Add the element to the vector and return
				data.vector_ptr->vector.push_back(t.release());
				return true;
			}
		} while (!atomic_data.compare_exchange_weak(data, new_data, std::memory_order_release, std::memory_order_relaxed));

		// If we reach this point then atomic_data was successfully changed.
		// Since the pointers are now saved in the vector, release them from
		// the smart pointers.
		t.release();
		vector.release();
		return true;
	}

	// Lock the vector and flush all elements through the given function
	template<typename Func> void flush_and_lock(Func&& func)
	{
		// Try to lock the vector using a compare-exchange loop
		internal_data data = atomic_data.load(std::memory_order_relaxed);
		internal_data new_data;
		do {
			new_data = data;
			new_data.is_locked = true;
		} while (!atomic_data.compare_exchange_weak(data, new_data, std::memory_order_acquire, std::memory_order_relaxed));

		if (!data.is_vector) {
			// If there is an inline element, just pass it on
			if (data.inline_ptr)
				func(task_ptr(data.inline_ptr));
		} else {
			// If we are using vector_data, lock it and flush all elements
			std::lock_guard<std::mutex> locked(data.vector_ptr->lock);
			for (auto i: data.vector_ptr->vector)
				func(task_ptr(i));

			// Clear the vector to save memory. Note that we don't actually free
			// the vector_data here because other threads may still be using it.
			// This isn't a very significant cost since multiple continuations
			// are relatively rare.
			data.vector_ptr->vector.clear();
		}
	}
};

} // namespace detail
} // namespace async
