#!/bin/sh
icpc -std=c++11 -Wall -Wextra -pedantic -pthread -Iinclude src/scheduler.cpp src/threadpool_scheduler.cpp -fvisibility=hidden -ipo -O3 -fpic -DLIBASYNC_BUILD -shared -o libasync++.so
