#!/bin/sh
g++ -std=c++11 -Wall -Wextra -pedantic -pthread -Iinclude src/scheduler.cpp -O3 -DLIBASYNC_BUILD -DLIBASYNC_STATIC -c -o scheduler.o
ar rcs libasync++.a scheduler.o
