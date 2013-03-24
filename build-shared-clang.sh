#!/bin/sh
# Working around http://llvm.org/bugs/show_bug.cgi?id=12730
clang++ -std=c++11 -Wall -Wextra -pedantic -pthread -Iinclude src/scheduler.cpp -fvisibility=hidden -flto -O3 -fpic -DLIBASYNC_BUILD -shared -o libasync++.so -D__GCC_HAVE_SYNC_COMPARE_AND_SWAP_{1,2,4,8}
