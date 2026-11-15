//
// Created by Vladimir Glushkov on 15.11.2026.
//

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

#include "FFSpscQueue.h"

namespace {
	template<typename Producer, typename Consumer>
	void runPair(Producer&& producer, Consumer&& consumer) {
		auto c = std::async(std::launch::async, std::forward<Consumer>(consumer));
		auto p = std::async(std::launch::async, std::forward<Producer>(producer));
		p.get();
		c.get();
	}

	template<typename Q, typename T>
	void pushSpin(Q& q, const T* ptr) {
		while (!q.tryPush(ptr)) {
			std::this_thread::yield();
		}
	}

	template<typename Q>
	auto popSpin(Q& q) {
		for (;;) {
			if (auto* p = q.tryPop()) return p;
			std::this_thread::yield();
		}
	}

	// A "wide" object behind the pointer: if the consumer sees the pointer before the
	// producer's writes to the object (a release/acquire violation), the checksum breaks.
	struct Payload {
		std::uint64_t seq;
		std::uint64_t a;
		std::uint64_t b;
		std::uint64_t c;
		std::uint64_t checksum;

		void fill(std::uint64_t s) {
			seq = s;
			a = s * 3;
			b = s * 5 + 1;
			c = ~s;
			checksum = seq ^ a ^ b ^ c;
		}

		bool valid() const { return checksum == (seq ^ a ^ b ^ c); }
	};
}

TEST_CASE("01. Pointer queue: interface", "[FFPtrSpscQueue][base]") {
	using Q = FFPtrSpscQueue<int, 8>;

	SECTION("P-01. Types: stores and returns const T*") {
		STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Q&>().tryPop()), const int*>);
		STATIC_REQUIRE(std::is_same_v<Q::value_type, int>);
		STATIC_REQUIRE(Q::capacity() == 8);
	}

	SECTION("P-02. Not copyable, not movable") {
		STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Q>);
		STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<Q>);
		STATIC_REQUIRE_FALSE(std::is_move_constructible_v<Q>);
		STATIC_REQUIRE_FALSE(std::is_move_assignable_v<Q>);
	}
}

TEST_CASE("02. Pointer queue: single thread behaviour", "[FFPtrSpscQueue][base]") {
	SECTION("P-03. New queue is empty") {
		FFPtrSpscQueue<int, 4> q;
		REQUIRE(q.tryPop() == nullptr);
		REQUIRE(q.emptyApprox());
		REQUIRE(q.sizeApprox() == 0);
	}

	SECTION("P-04. FIFO order, the same pointers come out") {
		FFPtrSpscQueue<int, 8> q;
		int v[5] = {10, 11, 12, 13, 14};
		for (auto& x : v) REQUIRE(q.tryPush(&x));
		REQUIRE(q.sizeApprox() == 5);
		for (auto& x : v) {
			const int* p = q.tryPop();
			REQUIRE(p == &x);
			REQUIRE(*p == x);
		}
		REQUIRE(q.tryPop() == nullptr);
		REQUIRE(q.emptyApprox());
	}

	SECTION("P-05. Full queue rejects push, one pop frees one slot") {
		FFPtrSpscQueue<int, 4> q;
		int v[6] = {0, 1, 2, 3, 4, 5};
		for (int i = 0; i < 4; ++i) REQUIRE(q.tryPush(&v[i]));
		REQUIRE(q.sizeApprox() == 4);
		REQUIRE_FALSE(q.tryPush(&v[4]));
		REQUIRE(q.sizeApprox() == 4);

		REQUIRE(q.tryPop() == &v[0]);
		REQUIRE(q.tryPush(&v[4]));
		REQUIRE_FALSE(q.tryPush(&v[5]));

		for (int i = 1; i <= 4; ++i) REQUIRE(q.tryPop() == &v[i]);
		REQUIRE(q.tryPop() == nullptr);
	}

	SECTION("P-06. nullptr is rejected and does not change the queue") {
		FFPtrSpscQueue<int, 4> q;
		int x = 1;
		REQUIRE_FALSE(q.tryPush(nullptr));
		REQUIRE(q.sizeApprox() == 0);
		REQUIRE(q.tryPop() == nullptr);

		REQUIRE(q.tryPush(&x));
		REQUIRE_FALSE(q.tryPush(nullptr));
		REQUIRE(q.sizeApprox() == 1);
		REQUIRE(q.tryPop() == &x);
		REQUIRE(q.tryPop() == nullptr);
	}

	SECTION("P-07. Wrap-around: many times over the capacity") {
		FFPtrSpscQueue<int, 4> q;
		std::vector<int> v(1000);
		for (int i = 0; i < 1000; ++i) v[i] = i;
		std::size_t next = 0;
		for (std::size_t i = 0; i < v.size(); ++i) {
			REQUIRE(q.tryPush(&v[i]));
			if (i % 3 == 2) {
				// keep up to 3 elements inside, so the indices move across slot boundaries
				REQUIRE(q.tryPop() == &v[next++]);
				REQUIRE(q.tryPop() == &v[next++]);
			}
			if (q.sizeApprox() == 4) REQUIRE(q.tryPop() == &v[next++]);
		}
		while (const int* p = q.tryPop()) REQUIRE(p == &v[next++]);
		REQUIRE(next == v.size());
	}

	SECTION("P-08. Capacity 1") {
		FFPtrSpscQueue<int, 1> q;
		int a = 1;
		int b = 2;
		REQUIRE(q.tryPush(&a));
		REQUIRE_FALSE(q.tryPush(&b));
		REQUIRE(q.tryPop() == &a);
		REQUIRE(q.tryPush(&b));
		REQUIRE(q.tryPop() == &b);
		REQUIRE(q.tryPop() == nullptr);
	}

	SECTION("P-09. The queue does not own objects: destroying a non-empty queue is fine") {
		auto obj = std::make_unique<int>(42);
		{
			FFPtrSpscQueue<int, 4> q;
			REQUIRE(q.tryPush(obj.get()));
		}
		REQUIRE(*obj == 42);
	}
}

TEST_CASE("03. Pointer queue: concurrency correctness", "[FFPtrSpscQueue][concurrency]") {
	SECTION("PMT-01. FIFO order across threads (small capacity -> frequent full/empty)") {
		FFPtrSpscQueue<std::uint64_t, 16> q;
		constexpr std::uint64_t N = 1'000'000;
		std::vector<std::uint64_t> values(N);
		for (std::uint64_t i = 0; i < N; ++i) values[i] = i;
		std::uint64_t mismatches = 0;

		runPair(
			[&] {
				for (std::uint64_t i = 0; i < N; ++i) pushSpin(q, &values[i]);
			},
			[&] {
				for (std::uint64_t i = 0; i < N; ++i) {
					const std::uint64_t* p = popSpin(q);
					if (p != &values[i] || *p != i) ++mismatches;
				}
			});

		REQUIRE(mismatches == 0);
		REQUIRE(q.tryPop() == nullptr);
	}

	SECTION("PMT-02. Pointed-to object is published by the push (release/acquire on the slot)") {
		// The producer writes each object right before pushing its pointer.
		// Every object is used once: reusing objects would need a separate hand-back channel.
		FFPtrSpscQueue<Payload, 64> q;
		constexpr std::uint64_t N = 500'000;
		auto pool = std::make_unique<Payload[]>(N);
		std::uint64_t corrupted = 0;
		std::uint64_t out_of_order = 0;

		runPair(
			[&] {
				for (std::uint64_t i = 0; i < N; ++i) {
					pool[i].fill(i);
					pushSpin(q, &pool[i]);
				}
			},
			[&] {
				for (std::uint64_t i = 0; i < N; ++i) {
					const Payload* p = popSpin(q);
					if (!p->valid()) ++corrupted;
					if (p->seq != i) ++out_of_order;
				}
			});

		REQUIRE(corrupted == 0);
		REQUIRE(out_of_order == 0);
	}

	SECTION("PMT-03. Capacity 1 ping-pong") {
		FFPtrSpscQueue<int, 1> q;
		constexpr int N = 200'000;
		std::vector<int> values(N);
		for (int i = 0; i < N; ++i) values[i] = i;
		int mismatches = 0;

		runPair(
			[&] {
				for (int i = 0; i < N; ++i) pushSpin(q, &values[i]);
			},
			[&] {
				for (int i = 0; i < N; ++i) {
					if (*popSpin(q) != i) ++mismatches;
				}
			});

		REQUIRE(mismatches == 0);
	}

	SECTION("PMT-04. Elements left after threads finish are counted by sizeApprox") {
		FFPtrSpscQueue<int, 128> q;
		constexpr int N = 100'000;
		constexpr int Consumed = N - 100;
		std::vector<int> values(N);

		runPair(
			[&] {
				for (int i = 0; i < N; ++i) pushSpin(q, &values[i]);
			},
			[&] {
				for (int i = 0; i < Consumed; ++i) (void)popSpin(q);
			});

		REQUIRE(q.sizeApprox() == N - Consumed);
		for (int i = Consumed; i < N; ++i) REQUIRE(q.tryPop() == &values[i]);
		REQUIRE(q.tryPop() == nullptr);
	}

	SECTION("PMT-06. Reusing objects from a pool of Capacity + 2 (see the FFPtrSpscQueue comment)") {
		// The producer overwrites pool objects round-robin, the consumer is done with each object
		// before its next tryPop(). Under TSan a too small pool or a relaxed load of the slot
		// in tryPush() shows up as a data race on Payload; without TSan as corrupted payloads.
		constexpr std::size_t Cap = 4;
		constexpr std::size_t PoolSize = Cap + 2;
		FFPtrSpscQueue<Payload, Cap> q;
		constexpr std::uint64_t N = 500'000;
		Payload pool[PoolSize]{};
		std::uint64_t corrupted = 0;
		std::uint64_t out_of_order = 0;

		runPair(
			[&] {
				for (std::uint64_t j = 0; j < N; ++j) {
					Payload& obj = pool[j % PoolSize];
					obj.fill(j);
					pushSpin(q, &obj);
				}
			},
			[&] {
				for (std::uint64_t i = 0; i < N; ++i) {
					const Payload* p = popSpin(q);
					// all work with *p is finished here, before the next tryPop()
					if (!p->valid()) ++corrupted;
					if (p->seq != i) ++out_of_order;
				}
			});

		REQUIRE(corrupted == 0);
		REQUIRE(out_of_order == 0);
	}

	SECTION("PMT-05. sizeApprox from a third thread stays within [0, Capacity]") {
		// Probabilistic: a green run does not prove correctness, a red run proves a bug.
		// In a Debug build the assert(back >= front) inside sizeApprox() also checks the ordering.
		FFPtrSpscQueue<int, 4> q;
		std::atomic<bool> stop{false};
		int dummy = 0;

		auto producer = std::async(std::launch::async, [&] {
			while (!stop.load(std::memory_order_relaxed)) (void)q.tryPush(&dummy);
		});
		auto consumer = std::async(std::launch::async, [&] {
			while (!stop.load(std::memory_order_relaxed)) (void)q.tryPop();
		});

		std::uint64_t calls = 0;
		std::uint64_t out_of_range = 0;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (out_of_range == 0 && std::chrono::steady_clock::now() < deadline) {
			for (int k = 0; k < 10'000; ++k) {
				if (q.sizeApprox() > q.capacity()) ++out_of_range;
				++calls;
			}
		}

		stop.store(true, std::memory_order_relaxed);
		producer.get();
		consumer.get();

		INFO("sizeApprox() calls: " << calls);
		REQUIRE(out_of_range == 0);
	}
}
