//
// Created by Vladimir Glushkov on 15.09.2026.
//

#include <catch2/catch_test_macros.hpp>

#include "ThreadSafeQueue.h"

// Single thread tests checking FIFO structure and base invariants of queue
TEST_CASE("01. Single thread basic Sequential Operations", "[queue]") {
	ThreadSafeQueue<int> queue(3);

	SECTION("ST-01 Initial state") {
		REQUIRE(queue.empty());
		REQUIRE(queue.size() == 0); // NOLINT(*-container-size-empty)
		REQUIRE(queue.capacity() == 3);
		REQUIRE_FALSE(queue.is_shutdown());
		REQUIRE(queue.waiting_consumers() == 0);
		REQUIRE(queue.waiting_producers() == 0);
		// check zero capacity queue throws with invalid argument exception
		REQUIRE_THROWS_AS(ThreadSafeQueue<int>(0), std::invalid_argument);
	}

	SECTION("ST-02 FIFO is correct with sequential calling to push/emplace and pop") {
		REQUIRE(queue.emplace(10));
		REQUIRE(queue.push(20));
		REQUIRE(queue.push(30));

		REQUIRE(queue.size() == 3);

		int val = 0;
		REQUIRE(queue.pop(val));
		REQUIRE(queue.size() == 2);
		REQUIRE(val == 10);

		REQUIRE(queue.pop(val));
		REQUIRE(queue.size() == 1);
		REQUIRE(val == 20);

		REQUIRE(queue.pop(val));
		REQUIRE(queue.empty());
		REQUIRE(val == 30);
	}

	SECTION("ST-03 Move-only types support") {
		ThreadSafeQueue<std::unique_ptr<int>> ptr_queue(2);

		auto ptr1 = std::make_unique<int>(42);
		REQUIRE(ptr_queue.push(std::move(ptr1)));

		std::unique_ptr<int> popped_ptr;
		REQUIRE(ptr_queue.pop(popped_ptr));
		REQUIRE(popped_ptr != nullptr);
		REQUIRE(*popped_ptr == 42);
	}

	SECTION("ST-04 Non-blocking try operations") {
		for (auto i = 1; i < 4; ++i) {
			REQUIRE(queue.try_push(i));
		}
		REQUIRE_FALSE(queue.try_push(4)); // Full -> return false

		int val = 0;
		REQUIRE(queue.try_pop(val));
		REQUIRE(val == 1);
		REQUIRE(queue.try_pop(val));
		REQUIRE(val == 2);
		REQUIRE(queue.try_pop(val));
		REQUIRE(val == 3);
		REQUIRE_FALSE(queue.try_pop(val)); // Empty -> return false

		// ---

		for (auto i = 1; i < 4; ++i) {
			REQUIRE(queue.try_emplace(i));
		}
		REQUIRE_FALSE(queue.try_emplace(4)); // Full -> return false

		std::optional<int> opt;
		REQUIRE((opt = queue.try_pop()));
		REQUIRE(opt.value() == 1);
		REQUIRE((opt = queue.try_pop()));
		REQUIRE(opt.value() == 2);
		REQUIRE((opt = queue.try_pop()));
		REQUIRE(opt.value() == 3);
		REQUIRE_FALSE((opt = queue.try_pop())); // Empty -> return false
	}

	SECTION("ST-05 Popping elements without default constructor") {
		struct StructNoDefaultConstructor {
			int value{};
			StructNoDefaultConstructor() = delete;
			StructNoDefaultConstructor(int val) : value(val) {}
		};
		ThreadSafeQueue<StructNoDefaultConstructor> q(2);
		REQUIRE(q.try_emplace(1));
		q.shutdown();
		std::optional<StructNoDefaultConstructor> opt;
		REQUIRE((opt = q.try_pop()));
		REQUIRE(opt.value().value == 1);

		REQUIRE_FALSE((opt = q.try_pop())); // Empty -> return std::nullopt
	}
}
