//
// Created by Vladimir Glushkov on 12.10.2026.
//

#include <barrier>
#include <iostream>
#include <latch>
#include <set>
#include <string>
#include <catch2/catch_test_macros.hpp>

#include "ThreadPool.h"

template<typename F, typename... Args>
concept CanPushTask = requires(ThreadPool& tp, F&& f, Args&&... args)
{
	tp.push(std::forward<F>(f), std::forward<Args>(args)...);
};

TEST_CASE("01. Base tests") {
	SECTION("BT-01. Single task with return value") {
		ThreadPool tp(1);
		auto f = tp.push([]() {
			return 42;
		}).value();
		tp.shutdown();
		f.wait();
		REQUIRE(f.get() == 42);
	}
	SECTION("BT-02. Single task with no return") {
		ThreadPool tp(1);
		bool flag = false;
		auto f = tp.push([&]() {
			flag = true;
		}).value();
		tp.shutdown();
		f.wait();
		REQUIRE(flag);
	}
	SECTION("BT-03. Forward arguments") {
		ThreadPool tp(1);
		auto f = tp.push([](int a, int b) {
			return a + b;
		}, 40, 2).value();
		tp.shutdown();
		f.wait();
		REQUIRE(f.get() == 42);
	}
	SECTION("BT-04. Forward move-only arguments") {
		ThreadPool tp(1);
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
		ThreadPool pool(std::thread::hardware_concurrency());
		std::atomic<int> counter(0);
		for (auto i = 0; i < 1000; ++i) {
			pool.push([&]() {
				using namespace std::chrono_literals;
				std::this_thread::sleep_for(1ms);
				++counter;
			});
		}
		pool.shutdown();
		pool.join();
		REQUIRE(counter == 1000);
	}
	SECTION("BT-07. stop_request cancels tasks") {
		ThreadPool pool(1);
		std::promise<void> promise;
		pool.push([f = promise.get_future()]() {
			f.wait();
		});
		auto canceledFuture = pool.push([]() {}).value();
		pool.request_stop();
		promise.set_value();
		pool.join();
		REQUIRE_THROWS_AS(canceledFuture.get(), ThreadPool::PoolDiscarded);
	}
	SECTION("BT-08. shutdown stops thread pool") {
		ThreadPool pool(std::thread::hardware_concurrency());
		std::atomic<int> counter(0);
		for (auto i = 0; i < 42; ++i) {
			pool.push([&]() {
				using namespace std::chrono_literals;
				std::this_thread::sleep_for(5ms);
				++counter;
			});
		}
		pool.shutdown();
		pool.join();
		REQUIRE(counter == 42);
	}
	SECTION("BT-09. Move-only callable func") {
		ThreadPool pool(1);
		std::move_only_function<int()> func = []() {
			return 42;
		};
		REQUIRE_FALSE((std::is_copy_assignable_v<decltype(func)> || std::is_copy_constructible_v<decltype(func)>));
		auto f = pool.push(std::move(func)).value();
		pool.shutdown();
		pool.join();
		REQUIRE(f.get() == 42);
	}
}

TEST_CASE("02. Concurrency tests") {
	SECTION("CT-01. Thread count") {
		constexpr int thread_count = 4;
		if (std::thread::hardware_concurrency() < thread_count) {
			SKIP("not enough available threads count");
		}
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
		if (std::thread::hardware_concurrency() < thread_count) {
			SKIP("not enough available threads count");
		}
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
			for (auto& f: futures) {
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
			// nested.wait() -> Deadlock. Waiting for nested task in single worker pool causes deadlock
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
	SECTION("LT-01. Destruction drains queue by default") {
		constexpr int task_count = 10;
		std::vector<std::future<int>> futures;
		{
			ThreadPool pool(1);
			std::promise<void> promise;
			pool.push([f = promise.get_future()]() {
				f.wait();
			}).value();
			for (auto i = 0; i < task_count; ++i) {
				futures.push_back(pool.push([i]() {
					return i;
				}).value());
			}
			pool.shutdown();
			promise.set_value();
		}
		for (auto i = 0; i < task_count; ++i) {
			REQUIRE(futures[i].get() == i);
		}
	}
	SECTION("LT-02. Double shutdown is safe") {
		auto f = []() {
			ThreadPool pool(1);
			pool.shutdown();
			REQUIRE_NOTHROW(pool.shutdown());
		};
		// check destruction of pool will not throw
		REQUIRE_NOTHROW(f());
	}
	SECTION("LT-03. request_stop after shutdown discards non-started tasks") {
		constexpr unsigned task_count = 42;
		ThreadPool pool(1);
		std::barrier barrier(2);
		std::atomic<int> counter{0};
		pool.push([&]() {
			barrier.arrive_and_wait();
		});
		pool.push([&]() {
			pool.shutdown();
		});
		for (auto i = 0; i < task_count; ++i) {
			pool.push([&]() {
				++counter;
			});
		}
		pool.push([&]() {
			pool.request_stop();
		});
		for (auto i = 0; i < task_count; ++i) {
			pool.push([&]() {
				++counter;
			});
		}
		barrier.arrive_and_wait();
		pool.join();
		REQUIRE(counter == task_count);
	}
	SECTION("LT-04. Unable push new tasks after shutdown/request_stop was called") {
		SECTION("shutdown") {
			ThreadPool pool(std::thread::hardware_concurrency());
			auto work = [] {
				return 42;
			};
			auto future = pool.push(work).value();
			future.wait();
			REQUIRE(future.get() == 42);
			pool.shutdown();
			{
				auto future_opt = pool.push(work);
				REQUIRE_FALSE(future_opt.has_value());
			}
			pool.join();
			{
				auto future_opt = pool.push(work);
				REQUIRE_FALSE(future_opt.has_value());
			}
		}
		SECTION("request_stop") {
			ThreadPool pool(std::thread::hardware_concurrency());
			auto work = [] {
				return 42;
			};
			auto future = pool.push(work).value();
			future.wait();
			REQUIRE(future.get() == 42);
			pool.request_stop();
			{
				auto future_opt = pool.push(work);
				REQUIRE_FALSE(future_opt.has_value());
			}
			pool.join();
			{
				auto future_opt = pool.push(work);
				REQUIRE_FALSE(future_opt.has_value());
			}
		}
	}
}

TEST_CASE("04. Exception safety") {
	SECTION("ES-01. Exception inside task can be caught") {
		ThreadPool pool(1);
		auto f = pool.push([]() {
			throw std::logic_error("Expected");
		}).value();
		REQUIRE_THROWS_AS(f.get(), std::logic_error);
	}
	SECTION("ES-02. Destruction of stopped pool cancels tasks") {
		constexpr int task_count = 10;
		std::vector<std::future<int>> futures;
		{
			ThreadPool pool(1);
			std::promise<void> promise;
			pool.push([f = promise.get_future()]() {
				f.wait();
			}).value();
			for (auto i = 0; i < task_count; ++i) {
				futures.push_back(pool.push([i]() {
					return i;
				}).value());
			}
			pool.request_stop();
			promise.set_value();
		}
		for (auto i = 0; i < task_count; ++i) {
			REQUIRE_THROWS_AS(futures[i].get(), ThreadPool::PoolDiscarded);
		}
	}
}
