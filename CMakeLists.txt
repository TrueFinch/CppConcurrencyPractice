cmake_minimum_required(VERSION 3.28)
project(CppConcurrencyPractice)

set(CMAKE_CXX_STANDARD 23)

add_subdirectory(ThreadSafeQueue)
add_subdirectory(Synchronized)
add_subdirectory(ThreadPool)
add_subdirectory(SPSCQueue)
