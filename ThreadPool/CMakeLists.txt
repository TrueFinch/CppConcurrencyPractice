cmake_minimum_required(VERSION 3.28)
project(ThreadPool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Catch2 3 REQUIRED)
add_executable(ThreadPoolTests
		ThreadPool.h
		base_tests.cpp
)
target_link_libraries(ThreadPoolTests PRIVATE Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(ThreadPoolTests)
