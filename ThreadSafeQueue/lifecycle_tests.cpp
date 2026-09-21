//
// Created by Vladimir Glushkov on 15.09.2026.
//
#include <future>
#include <catch2/catch_test_macros.hpp>

#include "ThreadSafeQueue.h"

TEST_CASE("02. Lifecycle, shutdown and threads count", "[queue]") {
	ThreadSafeQueue<int> queue(3);
	SECTION("LC-01 Waiting threads count") {
		std::optional<int> result;
		std::thread t([&queue, &result]() {
			auto popped = queue.pop();
			result = popped;
		});
		// wait until t1 will enter in pop
		while (queue.waiting_consumers() == 0) {
			std::this_thread::yield();
		}
		REQUIRE(queue.waiting_consumers() == 1);
		REQUIRE(queue.waiting_producers() == 0);
		queue.push(1);
		t.join();
		// wait all threads to complete
		while (queue.waiting_consumers() != 0) {
			std::this_thread::yield();
		}
		REQUIRE(queue.waiting_consumers() == 0);
		REQUIRE(queue.waiting_producers() == 0);
		REQUIRE(result);
		REQUIRE(result.value() == 1);
	}

	SECTION("LC-02. Shutdown blocks pushing") {
		SECTION("LC-02.1. Empty queue") {
			constexpr auto capacity = 3;
			ThreadSafeQueue<int> emptyQueue(capacity);
			REQUIRE(emptyQueue.empty());
			emptyQueue.shutdown();
			REQUIRE(emptyQueue.is_shutdown());
			REQUIRE_FALSE(emptyQueue.push(-1));
			REQUIRE_FALSE(emptyQueue.try_push(-1));
			REQUIRE_FALSE(emptyQueue.pop());
			REQUIRE_FALSE(emptyQueue.try_pop());
		}
		SECTION("LC-02.2. Not empty queue") {
			constexpr auto capacity = 3;
			ThreadSafeQueue<int> notEmptyQueue(capacity);
			constexpr auto n = capacity / 2;
			for (auto i = 0; i < n; ++i) {
				notEmptyQueue.push(i);
			}
			REQUIRE(n == notEmptyQueue.size());
			notEmptyQueue.shutdown();
			REQUIRE(notEmptyQueue.is_shutdown());
			REQUIRE_FALSE(notEmptyQueue.push(-1));
			REQUIRE_FALSE(notEmptyQueue.try_push(-1));
			auto i = 0;
			while (!notEmptyQueue.empty()) {
				auto popped = notEmptyQueue.pop();
				REQUIRE(popped);
				REQUIRE(popped.value() == i++);
			}
			REQUIRE_FALSE(notEmptyQueue.pop());
			REQUIRE_FALSE(notEmptyQueue.try_pop());
		}
		SECTION("LC-02.3. Full queue") {
			constexpr auto capacity = 3;
			ThreadSafeQueue<int> fullQueue(capacity);
			for (auto i = 0; i < fullQueue.capacity(); ++i) {
				fullQueue.push(i);
			}
			REQUIRE(fullQueue.capacity() == fullQueue.size());
			fullQueue.shutdown();
			REQUIRE(fullQueue.is_shutdown());
			REQUIRE_FALSE(fullQueue.push(-1));
			REQUIRE_FALSE(fullQueue.try_push(-1));

			for (auto i = 0; i < fullQueue.capacity(); ++i) {
				REQUIRE(fullQueue.pop().value() == i);
			}
			REQUIRE_FALSE(fullQueue.pop());
			REQUIRE_FALSE(fullQueue.try_pop());
		}
	}

	SECTION("LC-03. Shutdown unlocks threads blocked in push (full queue)") {
		// make queue full
		for (auto i = 0; i < queue.capacity(); ++i) {
			queue.push(i);
		}
		std::vector<std::future<bool>> pushFutures;
		for (auto i = 0; i < queue.capacity(); ++i) {
			pushFutures.emplace_back(std::async(std::launch::async, [&queue, i]() {
				return queue.push(i);
			}));
		}
		// wait all threads to enter in blocking push
		while (queue.waiting_producers() < queue.capacity()) {
			std::this_thread::yield();
		}
		REQUIRE(queue.waiting_producers() == queue.capacity());
		REQUIRE(queue.waiting_consumers() == 0);
		queue.shutdown();
		// wait all threads to complete
		while (queue.waiting_producers() != 0) {
			std::this_thread::yield();
		}
		for (auto& f: pushFutures) {
			REQUIRE_FALSE(f.get());
		}
		REQUIRE(queue.waiting_producers() == 0);
		REQUIRE(queue.size() == queue.capacity());
	}

	SECTION("LC-04. Shutdown unlocks threads blocked in pop (empty queue)") {
		REQUIRE(queue.empty());
		std::vector<std::future<std::optional<int>>> popFutures;
		for (auto i = 0; i < queue.capacity(); ++i) {
			popFutures.emplace_back(std::async(std::launch::async, [&queue]() {
				return queue.pop();
			}));
		}
		// wait all threads to enter in blocking pop
		while (queue.waiting_consumers() < queue.capacity()) {
			std::this_thread::yield();
		}
		REQUIRE(queue.waiting_consumers() == queue.capacity());
		REQUIRE(queue.waiting_producers() == 0);
		queue.shutdown();
		// wait all threads to complete
		while (queue.waiting_consumers() != 0) {
			std::this_thread::yield();
		}
		for (auto& f: popFutures) {
			f.wait();
			REQUIRE_FALSE(f.get());
		}
		REQUIRE(queue.waiting_consumers() == 0);
		REQUIRE(queue.waiting_producers() == 0);
		REQUIRE(queue.empty());
	}

	auto checkPopUnlocksThread = [](ThreadSafeQueue<int>& queue, bool useTry) {
		// make queue full
		for (auto i = 0; i < queue.capacity(); ++i) {
			REQUIRE(queue.push(i));
		}
		std::thread producer([&queue]() {
			queue.push(-1);
		});
		// wait producer to enter in blocking push
		while (queue.waiting_producers() == 0) {
			std::this_thread::yield();
		}
		// try_pop should unlock producer thread
		{
			auto popped = useTry ? queue.try_pop() : queue.pop();
			REQUIRE((popped && popped.value() == 0));
		}
		// wait for producer thread to complete
		producer.join();
		queue.shutdown();
		// check
		for (auto i = 1; i < queue.capacity(); ++i) {
			auto popped = queue.pop();
			REQUIRE((popped && popped.value() == i));
		}
		auto popped = queue.pop();
		REQUIRE((popped && popped.value() == -1));
	};

	SECTION("LC-05. pop unlocks threads blocked on push") {
		checkPopUnlocksThread(queue, false);
	}

	SECTION("LC-06. try_pop unlocks threads blocked on push") {
		checkPopUnlocksThread(queue, true);
	}

	auto checkPushUnlocksThread = [](ThreadSafeQueue<int>& queue, bool useTry) {
		// ensure queue is empty
		REQUIRE(queue.empty());
		// run consumer
		std::future<std::optional<int>> future = std::async(std::launch::async, [&queue]() {
			return queue.pop();
		});
		// wait consumer thread enter blocking pop
		while (queue.waiting_consumers() == 0) {
			std::this_thread::yield();
		}
		// try_push should unlock consumer thread
		REQUIRE((useTry ? queue.try_push(0) : queue.push(0)));
		// wait for consumer thread to complete
		future.wait();
		queue.shutdown();
		// check
		auto result = future.get();
		REQUIRE((result && result.value() == 0));
	};

	SECTION("LC-07. push unlocks threads blocked on pop") {
		checkPushUnlocksThread(queue, false);
	}

	SECTION("LC-08. try_push unlocks threads blocked on pop") {
		checkPushUnlocksThread(queue, true);
	}

	SECTION("LC-09. Double shutdown is idempotent", "[queue][shutdown][edge]") {
		// Repeated calls to shutdown() (including from multiple threads simultaneously) must not cause crashes, deadlocks, or UB.
		REQUIRE(queue.push(1));
		queue.shutdown();
		REQUIRE(queue.is_shutdown());
		// Repeated sequential call
		REQUIRE_NOTHROW(queue.shutdown());
		REQUIRE(queue.is_shutdown());
		// Concurrent repeated calls from multiple threads
		constexpr int num_threads = 8;
		std::vector<std::thread> shutters;
		for (int i = 0; i < num_threads; ++i) {
			shutters.emplace_back([&]() { queue.shutdown(); });
		}
		for (auto& t: shutters) {
			t.join();
		}
		REQUIRE(queue.is_shutdown());
		// Data added before shutdown must still be accessible via pop/try_pop
		int val = 0;
		REQUIRE(queue.try_pop(val));
		REQUIRE(val == 1);
	}
}
