Async++
=======

Async++ is a lightweight concurrency framework for C++11. The concept was inspired by the [Microsoft PPL library](http://msdn.microsoft.com/en-us/library/dd492418.aspx) and the [N3428](http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2012/n3428.pdf) C++ standard proposal.

Example
-------
Here is a short example which shows some features of Async++:

```c++
#include <iostream>
#include <async++.h>

int main()
{
    auto task1 = async::spawn([] {
        std::cout << "Task 1 executes asynchronously" << std::endl;
    });
    auto task2 = async::spawn([]() -> int {
        std::cout << "Task 2 executes in parallel with task 1" << std::endl;
        return 42;
    });
    auto task3 = task2.then([](int value) {
        std::cout << "Task 3 executes after task 2, which returned " << value << std::endl;
        return value * 3;
    });
    auto task4 = async::when_all(task1, task3).then([](std::tuple<async::void_, int> results) {
        std::cout << "Task 4 executes after tasks 1 and 3. Task 3 returned " << std::get<1>(results) << std::endl;
    });

    task4.get();
    std::cout << "Task 4 has completed" << std::endl;
}

// Output (order of tasks 1 and 2 may be different):
// Task 1 executes asynchronously
// Task 2 executes in parallel with task 1
// Task 3 executes after task 2, which returned 42
// Task 4 executes after tasks 1 and 3. Task 3 returned 126
// Task 4 has completed
```

Supported Platforms
-------------------

The only requirement to use Async++ is a C++11 compiler and standard library. Unfortunately C++11 is not yet fully implemented on most platforms. Here is the list of OS and compiler combinations which are known to work.

- Linux: GCC 4.7+ and Clang 3.2+
- Mac: Not tested yet, but it should work with the latest Clang and GCC 4.7+
- Windows: MSVC 2012 (even with the Nov CTP) does not support enough of C++11. Work is in progress to port to GCC 4.8 on MinGW.

Building and Installing
-----------------------
Instructions for compiling Async++ and using it in your code are available on the [Building and Installing](https://github.com/Amanieu/asyncplusplus/wiki/Building-and-Installing) page.

Tutorial
--------
The [Tutorial](https://github.com/Amanieu/asyncplusplus/wiki/Tutorial) provides a step-by-step guide to all the features of Async++.

API Reference
-------------
The [API Reference](https://github.com/Amanieu/asyncplusplus/wiki/API-Reference) gives detailed descriptions of all the classes and functions available in Async++.

Contact
-------
You can contact me by email at amanieu@gmail.com.
