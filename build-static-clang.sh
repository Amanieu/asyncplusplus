#!/bin/sh
# Working around http://llvm.org/bugs/show_bug.cgi?id=12730
clang++ -std=c++11 -Wall -Wextra -pedantic -pthread -Iinclude src/scheduler.cpp -fvisibility=hidden -O3 -DLIBASYNC_BUILD -DLIBASYNC_STATIC -c -o scheduler.o -D__GCC_HAVE_SYNC_COMPARE_AND_SWAP_{1,2,4,8}
clang++ -std=c++11 -Wall -Wextra -pedantic -pthread -Iinclude src/threadpool_scheduler.cpp -fvisibility=hidden -O3 -DLIBASYNC_BUILD -DLIBASYNC_STATIC -c -o threadpool_scheduler.o -D__GCC_HAVE_SYNC_COMPARE_AND_SWAP_{1,2,4,8}
ar rcs libasync++.a scheduler.o threadpool_scheduler.o
