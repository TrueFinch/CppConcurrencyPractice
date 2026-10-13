//
// Created by Vladimir Glushkov on 12.10.2026.
//

#include <barrier>
#include <iostream>
#include <latch>
#include <set>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ThreadPool.h"

template<typename F, typename... Args>
concept CanPushTask = requires(ThreadPool& tp, F&& f, Args&&... args)
{
	tp.push(std::forward<F>(f), std::forward<Args>(args)...);
};

TEST_CASE("01. Base tests") {
	ThreadPool tp(1);
	SECTION("BT-01. Single task with return value") {
		auto f = tp.push([]() {
			return 42;
		}).value();
		tp.shutdown();
		f.wait();
		REQUIRE(f.get() == 42);
	}
	SECTION("BT-02. Single task with no return") {
		bool flag = false;
		auto f = tp.push([&]() {
			flag = true;
		}).value();
		tp.shutdown();
		f.wait();
		REQUIRE(flag);
	}
	SECTION("BT-03. Forward arguments") {
		auto f = tp.push([](int a, int b) {
			return a + b;
		}, 40, 2).value();
		tp.shutdown();
		f.wait();
		REQUIRE(f.get() == 42);
	}
	SECTION("BT-04. Forward move-only arguments") {
		auto ptr = std::make_unique<int>(42);
		auto f = tp.push([](std::unique_ptr<int> ptr) {
			return *ptr;
		}, std::move(ptr)).value();
		tp.shutdown();
		f.wait();
		REQUIRE(f.get() == 42);
	}
	SECTION("BT-05. Constraints and compile time checks") {
		SECTION("Valid invocable") {
			STATIC_CHECK(CanPushTask<void()>);
			STATIC_CHECK(CanPushTask<void(int), int>);
			STATIC_CHECK(CanPushTask<int(int), int>);
			STATIC_CHECK(CanPushTask<decltype([](int){}), int>);
			STATIC_CHECK(CanPushTask<decltype(&std::vector<int>::empty), std::vector<int>>);
			using PushBackMemPtr = void(std::vector<int>::*)(const int&);
			STATIC_CHECK(CanPushTask<PushBackMemPtr, std::vector<int>, int>);
		}
		SECTION("Invalid invocable") {
			STATIC_CHECK_FALSE(CanPushTask<void(), int>);
			STATIC_CHECK_FALSE(CanPushTask<void(int), int, int>);
			STATIC_CHECK_FALSE(CanPushTask<void(int), std::string>);
			STATIC_CHECK_FALSE(CanPushTask<decltype([](int){}), std::string>);
			using PushBackMemPtr = void(std::vector<int>::*)(const int&);
			STATIC_CHECK_FALSE(CanPushTask<PushBackMemPtr, std::vector<int>>);
		}
	}
	SECTION("BT-06. More tasks than workers in pool") {
		ThreadPool pool(99);
		std::atomic<int> counter(0);
		for (auto i = 0; i < 1000; ++i) {
			pool.push([&]() {
				using namespace std::chrono_literals;
				std::this_thread::sleep_for(100ms);
				++counter;
			});
		}
		pool.shutdown();
		pool.join();
		REQUIRE(counter == 1000);
	}
}

TEST_CASE("02. Concurrency tests") {
	SECTION("CT-01. Thread count") {
		constexpr int thread_count = 4;
		REQUIRE(std::thread::hardware_concurrency() >= thread_count);
		ThreadPool pool(thread_count);
		std::latch latch(thread_count);
		std::set<int> set;
		std::mutex mtx;
		for (auto i = 0; i < thread_count; ++i) {
			pool.push([&, i = i] {
				latch.count_down();
				latch.wait();
				std::scoped_lock lock(mtx);
				set.insert(i);
			});
		}
		pool.shutdown();
		pool.join();
		REQUIRE(set == std::set{0, 1, 2, 3});
	}
	SECTION("CT-02. Thread reuse") {
		constexpr int thread_count = 4;
		REQUIRE(std::thread::hardware_concurrency() >= thread_count);
		ThreadPool pool(thread_count);
		std::barrier barrier(thread_count);
		auto get_threads_ids = [&] {
			std::set<std::thread::id> thread_ids;
			std::vector<std::future<std::thread::id>> futures;
			for (auto i = 0; i < thread_count; ++i) {
				futures.emplace_back(pool.push([&]() {
					barrier.arrive_and_wait();
					return std::this_thread::get_id();
				}).value());
			}
			for (auto& f : futures) {
				f.wait();
				thread_ids.insert(f.get());
			}
			return thread_ids;
		};
		auto ids1 = get_threads_ids();
		auto ids2 = get_threads_ids();
		pool.shutdown();
		pool.join();
		REQUIRE(ids1 == ids2);
	}
	SECTION("CT-03. Nested tasks single worker") {
		ThreadPool pool(1);
		auto future = pool.push([&]() {
			auto nested = pool.push([&]() { return 42; }).value();
			// waiting for nested task in single worker pool causes deadlock
			return nested;
		}).value();
		future.wait();
		auto nested = future.get();
		nested.wait();
		REQUIRE(nested.get() == 42);
	}
	SECTION("CT-04. Nested tasks multiple worker") {
		ThreadPool pool(2);
		auto future = pool.push([&]() {
			auto nested = pool.push([&]() { return 42; }).value();
			return nested.get();
		}).value();
		future.wait();
		REQUIRE(future.get() == 42);
	}
}

TEST_CASE("03. Lifecycle tests") {
}

