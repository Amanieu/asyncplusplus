#!/bin/sh
clang++ -std=c++11 -Wall -Wextra -pedantic -pthread -Iinclude src/scheduler.cpp -fvisibility=hidden -O3 -DLIBASYNC_BUILD -DLIBASYNC_STATIC -c -o scheduler.o -stdlib=libc++
ar rcs libasync++.a scheduler.o
