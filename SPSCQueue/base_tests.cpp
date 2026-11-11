//
// Created by Vladimir Glushkov on 11.11.2026.
//

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "SpscQueue.h"

namespace {
	// Track alive objects, copy and move operations
	struct Tracked {
		static inline int alive = 0;
		static inline int copies = 0;
		static inline int moves = 0;

		static void reset() { alive = copies = moves = 0; }

		int value;

		explicit Tracked(int v): value(v) { ++alive; }

		Tracked(const Tracked& o): value(o.value) {
			++alive;
			++copies;
		}

		Tracked(Tracked&& o) noexcept: value(o.value) {
			++alive;
			++moves;
		}

		Tracked& operator=(const Tracked&) = default;

		Tracked& operator=(Tracked&&) = default;

		~Tracked() { --alive; }
	};

	// No default ctor
	struct NoDefault {
		explicit NoDefault(int a, std::string b): a(a), b(std::move(b)) {}

		int a;
		std::string b;
	};

	// Copy ctor throws if flag is true
	struct ThrowOnCopy {
		static inline bool shouldThrow = false;
		int value;

		explicit ThrowOnCopy(int v): value(v) {}

		ThrowOnCopy(const ThrowOnCopy& o): value(o.value) {
			if (shouldThrow) throw std::runtime_error("copy failed");
		}

		ThrowOnCopy(ThrowOnCopy&&) noexcept = default;
	};
}

TEST_CASE("01. Basic properties", "[SpscQueue][base]") {
	SECTION("B-01. Non-copyable, non-movable") {
		using Q = SpscQueue<int, 4>;
		STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Q>);
		STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<Q>);
		STATIC_REQUIRE_FALSE(std::is_move_constructible_v<Q>);
		STATIC_REQUIRE_FALSE(std::is_move_assignable_v<Q>);
	}

	SECTION("B-02. Capacity") {
		STATIC_REQUIRE(SpscQueue<int, 1>::capacity() == 1);
		STATIC_REQUIRE(SpscQueue<int, 1024>::capacity() == 1024);
	}

	SECTION("B-03. New queue is empty") {
		SpscQueue<int, 8> q;
		REQUIRE(q.emptyApprox());
		REQUIRE(q.sizeApprox() == 0);
		REQUIRE_FALSE(q.tryPop().has_value());
	}

	SECTION("B-04. Constructing a queue does not construct any T") {
		Tracked::reset();
		{
			SpscQueue<Tracked, 16> q;
			REQUIRE(Tracked::alive == 0);
		}
		REQUIRE(Tracked::alive == 0);
	}
}

TEST_CASE("02. FIFO and bounds", "[SpscQueue][base]") {
	SECTION("F-01. FIFO order") {
		SpscQueue<int, 8> q;
		for (int i = 0; i < 5; ++i) {
			REQUIRE(q.tryPush(i));
		}
		REQUIRE(q.sizeApprox() == 5);
		for (int i = 0; i < 5; ++i) {
			auto v = q.tryPop();
			REQUIRE(v.has_value());
			REQUIRE(*v == i);
		}
		REQUIRE_FALSE(q.tryPop().has_value());
	}

	SECTION("F-02. Holds exactly Capacity elements") {
		SpscQueue<int, 8> q;
		for (int i = 0; i < 8; ++i) {
			REQUIRE(q.tryPush(i));
		}
		REQUIRE(q.sizeApprox() == 8);
		REQUIRE_FALSE(q.tryPush(100));

		REQUIRE(q.tryPop() == 0);
		REQUIRE(q.tryPush(8));
		REQUIRE_FALSE(q.tryPush(101));

		for (int i = 1; i <= 8; ++i) {
			REQUIRE(q.tryPop() == i);
		}
		REQUIRE(q.emptyApprox());
	}

	SECTION("F-03. Capacity == 1") {
		SpscQueue<int, 1> q;
		REQUIRE(q.tryPush(1));
		REQUIRE_FALSE(q.tryPush(2));
		REQUIRE(q.tryPop() == 1);
		REQUIRE_FALSE(q.tryPop().has_value());
		REQUIRE(q.tryPush(3));
		REQUIRE(q.tryPop() == 3);
	}

	SECTION("F-04. Wrap-around keeps order over many cycles") {
		SpscQueue<int, 4> q;
		int next_push = 0;
		int next_pop = 0;
		for (int round = 0; round < 1000; ++round) {
			const int n = 1 + round % 4;
			for (int i = 0; i < n; ++i) {
				REQUIRE(q.tryPush(next_push++));
			}
			for (int i = 0; i < n; ++i) {
				REQUIRE(q.tryPop() == next_pop++);
			}
		}
		REQUIRE(q.emptyApprox());
	}
}

TEST_CASE("03. Element types and lifetime", "[SpscQueue][base]") {
	SECTION("L-01. Move-only type") {
		SpscQueue<std::unique_ptr<int>, 4> q;
		REQUIRE(q.tryPush(std::make_unique<int>(42)));
		auto v = q.tryPop();
		REQUIRE(v.has_value());
		REQUIRE(*v != nullptr);
		REQUIRE(**v == 42);
	}

	SECTION("L-02. Failed rvalue push does not consume the argument") {
		SpscQueue<std::unique_ptr<int>, 1> q;
		REQUIRE(q.tryPush(std::make_unique<int>(1)));
		auto p = std::make_unique<int>(2);
		REQUIRE_FALSE(q.tryPush(std::move(p)));
		REQUIRE(p != nullptr);
		REQUIRE(*p == 2);
	}

	SECTION("L-03. Type without default constructor + tryEmplace") {
		SpscQueue<NoDefault, 4> q;
		REQUIRE(q.tryEmplace(7, "seven"));
		auto v = q.tryPop();
		REQUIRE(v.has_value());
		REQUIRE(v->a == 7);
		REQUIRE(v->b == "seven");
	}

	SECTION("L-04. tryEmplace constructs in place (no copies, no moves)") {
		Tracked::reset();
		SpscQueue<Tracked, 4> q;
		REQUIRE(q.tryEmplace(5));
		REQUIRE(Tracked::alive == 1);
		REQUIRE(Tracked::copies == 0);
		REQUIRE(Tracked::moves == 0);
	}

	SECTION("L-05. Pop destroys the element inside the buffer") {
		Tracked::reset();
		SpscQueue<Tracked, 4> q;
		REQUIRE(q.tryEmplace(1));
		REQUIRE(q.tryEmplace(2));
		{
			auto v = q.tryPop();
			REQUIRE(v.has_value());
			// one inside the optional, one still in the queue
			REQUIRE(Tracked::alive == 2);
		}
		REQUIRE(Tracked::alive == 1);
		REQUIRE(Tracked::copies == 0);
	}

	SECTION("L-06. Destructor destroys remaining elements (incl. wrapped)") {
		Tracked::reset();
		{
			SpscQueue<Tracked, 4> q;
			for (int i = 0; i < 3; ++i) {
				REQUIRE(q.tryEmplace(i));
			}
			REQUIRE(q.tryPop().has_value());
			REQUIRE(q.tryPop().has_value());
			// head == 2: push enough elements to wrap around the end of the buffer
			for (int i = 0; i < 3; ++i) {
				REQUIRE(q.tryEmplace(i));
			}
			REQUIRE(Tracked::alive == 4);
		}
		REQUIRE(Tracked::alive == 0);
	}
}

TEST_CASE("04. Exception safety", "[SpscQueue][base]") {
	SECTION("E-01. Throwing constructor leaves the queue unchanged") {
		SpscQueue<ThrowOnCopy, 4> q;
		const ThrowOnCopy a(1);
		const ThrowOnCopy b(2);

		ThrowOnCopy::shouldThrow = false;
		REQUIRE(q.tryPush(a));

		ThrowOnCopy::shouldThrow = true;
		REQUIRE_THROWS_AS(q.tryPush(b), std::runtime_error);
		ThrowOnCopy::shouldThrow = false;

		REQUIRE(q.sizeApprox() == 1);
		REQUIRE(q.tryPush(b));

		auto v1 = q.tryPop();
		auto v2 = q.tryPop();
		REQUIRE(v1.has_value());
		REQUIRE(v2.has_value());
		REQUIRE(v1->value == 1);
		REQUIRE(v2->value == 2);
		REQUIRE_FALSE(q.tryPop().has_value());
	}
}
