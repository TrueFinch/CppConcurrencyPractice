cmake_minimum_required(VERSION 3.28)
project(SpscQueue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Catch2 3 REQUIRED)
add_executable(SpscQueueTests
	SpscQueue.h
	base_tests.cpp
	concurrency_tests.cpp
)
target_link_libraries(SpscQueueTests PRIVATE Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(SpscQueueTests)

add_executable(SpscQueueBench
	SpscQueue.h
	stages/SpscQueueStage1.h
	bench.cpp
)
