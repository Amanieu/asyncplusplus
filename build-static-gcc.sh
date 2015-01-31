#!/bin/sh
g++ -std=c++11 -Wall -Wextra -pedantic -pthread -Iinclude src/scheduler.cpp -fvisibility=hidden -O3 -DLIBASYNC_BUILD -DLIBASYNC_STATIC -c -o scheduler.o
g++ -std=c++11 -Wall -Wextra -pedantic -pthread -Iinclude src/scheduler.cpp -fvisibility=hidden -O3 -DLIBASYNC_BUILD -DLIBASYNC_STATIC -c -o threadpool_scheduler.o
ar rcs libasync++.a threadpool_scheduler.o
