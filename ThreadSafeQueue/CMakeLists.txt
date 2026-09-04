cmake_minimum_required(VERSION 3.28)
project(ThreadSafeQueue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ThreadSafeQueue main.cpp
		ThreadSafeQueue.h)

