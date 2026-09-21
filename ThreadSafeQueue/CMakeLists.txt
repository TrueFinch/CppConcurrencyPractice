cmake_minimum_required(VERSION 3.28)
project(ThreadSafeQueue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Catch2 3 REQUIRED)
add_executable(ThreadSafeQueueTests
		ThreadSafeQueue.h
		single_thread_tests.cpp
		lifecycle_tests.cpp
		multi_thread_tests.cpp)
target_link_libraries(ThreadSafeQueueTests PRIVATE Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(ThreadSafeQueueTests)
