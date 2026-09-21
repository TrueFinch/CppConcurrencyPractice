//
// Created by Vladimir Glushkov on 15.09.2026.
//

#include <algorithm>
#include <future>
#include <catch2/catch_test_macros.hpp>

#include "ThreadSafeQueue.h"

TEST_CASE("03. Multi thread tests", "[queue]") {
	SECTION("MT-01. SPSC (Single producer single consumer)") {
		// prepare data
		ThreadSafeQueue<int> queue(50);
		const auto n = queue.capacity() * 2;
		std::vector<int> expected(n);
		std::ranges::iota(expected, 0);
		// run producer and consumer
		std::thread producer([&queue, expected]() {
			for (int el: expected) {
				queue.push(el);
			}
		});
		std::vector<int> actual;
		std::thread consumer([&queue, &actual, n]() {
			for (auto i = 0; i < n; ++i) {
				auto popped = queue.pop();
				actual.push_back(popped.value());
			}
		});
		// wait for all threads to complete
		producer.join();
		consumer.join();
		// check
		REQUIRE(actual == expected);
	}

	constexpr auto capacity = 50;
	ThreadSafeQueue<int> queue(capacity);

	SECTION("MT-02. MPSC (Multi producer single consumer)") {
		// prepare data
		constexpr auto producersCount = 5;
		constexpr auto n = producersCount * capacity;
		// run producers and consumer
		std::vector<std::thread> producers;
		for (auto p = 0; p < producersCount; ++p) {
			producers.emplace_back([&queue, p]() {
				for (auto i = 0; i < capacity; ++i) {
					queue.push(p * capacity + i);
				}
			});
		}
		auto consumerFuture = std::async(std::launch::async, [&queue]() {
			std::vector used(n, false);
			for (auto p = 0; p < n; ++p) {
				auto popped = queue.pop().value();
				if (used[popped] || popped < 0 || n <= popped) {
					break;
				}
				used[popped] = true;
			}
			return used;
		});
		// wait for all threads to complete
		consumerFuture.wait();
		for (auto& producer: producers) {
			producer.join();
		}
		// check
		auto result = consumerFuture.get();
		REQUIRE(std::ranges::all_of(result, [](bool b) { return b; }));
	}

	SECTION("MT-03. SPMC (Single producer multiple consumer)") {
		// prepare data
		constexpr auto consumersCount = 5;
		constexpr auto n = capacity * consumersCount;
		// run producer and consumers
		std::thread producer([&queue]() {
			for (auto i = 0; i < n; ++i) {
				queue.push(i);
			}
		});
		std::vector<std::future<std::vector<int>>> futures;
		for (auto c = 0; c < consumersCount; ++c) {
			futures.emplace_back(std::async(std::launch::async, [&queue]() {
				std::vector<int> popped;
				for (auto i = 0; i < capacity; ++i) {
					popped.emplace_back(queue.pop().value());
				}
				return popped;
			}));
		}
		// wait for all threads to complete
		producer.join();
		for (auto& f : futures) {
			f.wait();
		}
		// check
		std::vector used(n, false);
		for (auto& f : futures) {
			auto result = f.get();
			for (const auto& el : result) {
				REQUIRE_FALSE(used[el]);
				REQUIRE((0 <= el && el < n));
				used[el] = true;
			}
		}
		REQUIRE(std::ranges::all_of(used, [](bool b) { return b; }));
	}

	SECTION("MT-04. MPMC (Multi producer multi consumer)") {
		// prepare data
		constexpr auto producersCount = 5;
		constexpr auto consumersCount = producersCount;
		constexpr auto n = capacity * consumersCount;
		// run threads
		std::vector<std::thread> producers;
		for (auto p = 0; p < producersCount; ++p) {
			producers.emplace_back([&queue, p]() {
				for (auto i = 0; i < capacity; ++i) {
					queue.push(p * capacity + i);
				}
			});
		}
		std::vector<std::future<std::vector<int>>> futures;
		for (auto c = 0; c < consumersCount; ++c) {
			futures.emplace_back(std::async(std::launch::async, [&queue]() {
				std::vector<int> popped;
				for (auto i = 0; i < capacity; ++i) {
					popped.emplace_back(queue.pop().value());
				}
				return popped;
			}));
		}
		// wait for all threads to complete
		for (auto& producer: producers) {
			producer.join();
		}
		for (auto& f : futures) {
			f.wait();
		}
		// check
		std::vector used(n, false);
		for (auto& f : futures) {
			auto result = f.get();
			for (const auto& el : result) {
				REQUIRE_FALSE(used[el]);
				REQUIRE((0 <= el && el < n));
				used[el] = true;
			}
		}
		REQUIRE(std::ranges::all_of(used, [](bool b) { return b; }));
	}

	SECTION("MT-05. MPMC stress test with shutdown") {
		// prepare data
		constexpr auto lowCapacity = 1;
		ThreadSafeQueue<int> stressQueue(lowCapacity);
		constexpr auto n = 1000;
		constexpr auto producersCount = 10;
		static_assert(n % producersCount == 0);
		constexpr auto perProducerCount = n / producersCount;
		constexpr auto consumersCount = producersCount;
		// run threads
		std::vector<std::thread> producers;
		for (auto p = 0; p < producersCount; ++p) {
			producers.emplace_back([&stressQueue, p]() {
				for (auto i = 0; i < perProducerCount; ++i) {
					stressQueue.push(p * perProducerCount + i);
				}
			});
		}
		std::vector<std::future<std::vector<int>>> futures;
		for (auto c = 0; c < consumersCount; ++c) {
			futures.emplace_back(std::async(std::launch::async, [&stressQueue]() {
				std::vector<int> popped;
				while (auto value = stressQueue.pop()) {
					popped.emplace_back(*value);
				}
				return popped;
			}));
		}
		// wait for all threads to complete
		for (auto& producer: producers) {
			producer.join();
		}
		// all consumers enter waiting state
		while (stressQueue.waiting_consumers() < consumersCount) {
			std::this_thread::yield();
		}
		stressQueue.shutdown();
		for (auto& f : futures) {
			f.wait();
		}
		// check
		std::vector used(n, false);
		for (auto& f : futures) {
			auto result = f.get();
			for (const auto& el : result) {
				REQUIRE_FALSE(used[el]);
				REQUIRE((0 <= el && el < n));
				used[el] = true;
			}
		}
		REQUIRE(std::ranges::all_of(used, [](bool b) { return b; }));
	}
}
