cmake_minimum_required(VERSION 3.28)
project(Synchronized LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Catch2 3 REQUIRED)
add_executable(SynchronizedTests
		Synchronized.h
		tests.cpp)
target_link_libraries(SynchronizedTests PRIVATE Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(SynchronizedTests)
