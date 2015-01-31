#!/bin/sh
icpc -std=c++11 -Wall -Wextra -pedantic -pthread -Iinclude src/scheduler.cpp -fvisibility=hidden -O3 -DLIBASYNC_BUILD -DLIBASYNC_STATIC -c -o scheduler.o
icpc -std=c++11 -Wall -Wextra -pedantic -pthread -Iinclude src/threadpool_scheduler.cpp -fvisibility=hidden -O3 -DLIBASYNC_BUILD -DLIBASYNC_STATIC -c -o threadpool_scheduler.o
ar rcs libasync++.a scheduler.o threadpool_scheduler.o
