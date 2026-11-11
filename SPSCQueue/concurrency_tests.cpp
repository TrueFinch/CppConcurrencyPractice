//
// Created by Vladimir Glushkov on 11.11.2026.
//

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <thread>

#include "SpscQueue.h"

namespace {
	// Runs the producer and the consumer in separate threads.
	// Exceptions thrown in those threads are rethrown in the test thread via future::get().
	template<typename Producer, typename Consumer>
	void runPair(Producer&& producer, Consumer&& consumer) {
		auto c = std::async(std::launch::async, std::forward<Consumer>(consumer));
		auto p = std::async(std::launch::async, std::forward<Producer>(producer));
		p.get();
		c.get();
	}

	template<typename Q, typename V>
	void pushSpin(Q& q, V&& v) {
		while (!q.tryPush(std::forward<V>(v))) {
			std::this_thread::yield();
		}
	}

	template<typename Q>
	auto popSpin(Q& q) {
		for (;;) {
			if (auto v = q.tryPop()) return std::move(*v);
			std::this_thread::yield();
		}
	}

	// A "wide" element: if the consumer observes the new tail before the slot's data
	// (a release/acquire violation), the checksum invariant breaks.
	struct Payload {
		std::uint64_t seq;
		std::uint64_t a;
		std::uint64_t b;
		std::uint64_t c;
		std::uint64_t checksum;

		static Payload make(std::uint64_t s) {
			Payload p{s, s * 3, s * 5 + 1, ~s, 0};
			p.checksum = p.seq ^ p.a ^ p.b ^ p.c;
			return p;
		}

		bool valid() const { return checksum == (seq ^ a ^ b ^ c); }
	};

	struct LiveCounter {
		static inline std::atomic<int> alive{0};
		int value;
		explicit LiveCounter(int v) : value(v) { alive.fetch_add(1, std::memory_order_relaxed); }
		LiveCounter(LiveCounter&& o) noexcept : value(o.value) { alive.fetch_add(1, std::memory_order_relaxed); }
		LiveCounter(const LiveCounter&) = delete;
		~LiveCounter() { alive.fetch_sub(1, std::memory_order_relaxed); }
	};
}

TEST_CASE("05. Concurrency correctness", "[SpscQueue][concurrency]") {
	SECTION("MT-01. FIFO order across threads (small capacity -> frequent full/empty)") {
		SpscQueue<std::uint64_t, 16> q;
		constexpr std::uint64_t N = 2'000'000;
		std::uint64_t mismatches = 0;

		runPair(
			[&] {
				for (std::uint64_t i = 0; i < N; ++i) pushSpin(q, i);
			},
			[&] {
				for (std::uint64_t i = 0; i < N; ++i) {
					if (popSpin(q) != i) ++mismatches;
				}
			});

		REQUIRE(mismatches == 0);
		REQUIRE_FALSE(q.tryPop().has_value());
	}

	SECTION("MT-02. Payload visibility (release/acquire publishes element data)") {
		SpscQueue<Payload, 64> q;
		constexpr std::uint64_t N = 1'000'000;
		std::uint64_t corrupted = 0;
		std::uint64_t out_of_order = 0;

		runPair(
			[&] {
				for (std::uint64_t i = 0; i < N; ++i) pushSpin(q, Payload::make(i));
			},
			[&] {
				for (std::uint64_t i = 0; i < N; ++i) {
					const Payload p = popSpin(q);
					if (!p.valid()) ++corrupted;
					if (p.seq != i) ++out_of_order;
				}
			});

		REQUIRE(corrupted == 0);
		REQUIRE(out_of_order == 0);
	}

	SECTION("MT-03. Move-only heap objects: values intact, nothing leaked") {
		SpscQueue<std::unique_ptr<std::uint64_t>, 32> q;
		constexpr std::uint64_t N = 500'000;
		std::uint64_t sum = 0;

		runPair(
			[&] {
				for (std::uint64_t i = 0; i < N; ++i) pushSpin(q, std::make_unique<std::uint64_t>(i));
			},
			[&] {
				for (std::uint64_t i = 0; i < N; ++i) sum += *popSpin(q);
			});

		REQUIRE(sum == N * (N - 1) / 2);
	}

	SECTION("MT-04. Capacity 1 ping-pong") {
		SpscQueue<int, 1> q;
		constexpr int N = 200'000;
		int mismatches = 0;

		runPair(
			[&] {
				for (int i = 0; i < N; ++i) pushSpin(q, i);
			},
			[&] {
				for (int i = 0; i < N; ++i) {
					if (popSpin(q) != i) ++mismatches;
				}
			});

		REQUIRE(mismatches == 0);
	}

	SECTION("MT-05. Elements left after threads finish are destroyed with the queue") {
		LiveCounter::alive.store(0);
		{
			SpscQueue<LiveCounter, 128> q;
			constexpr int N = 100'000;
			constexpr int Consumed = N - 100;

			runPair(
				[&] {
					for (int i = 0; i < N; ++i) {
						while (!q.tryEmplace(i)) std::this_thread::yield();
					}
				},
				[&] {
					for (int i = 0; i < Consumed; ++i) (void)popSpin(q);
				});

			REQUIRE(q.sizeApprox() == N - Consumed);
			REQUIRE(LiveCounter::alive.load() == N - Consumed);
		}
		REQUIRE(LiveCounter::alive.load() == 0);
	}

	SECTION("MT-06. sizeApprox from a third thread stays within [0, Capacity]") {
		// Probabilistic test: the producer and the consumer hammer a small queue without pauses,
		// while the test thread (the "observer") keeps calling sizeApprox().
		// If head and tail are loaded in the wrong order, the consumer may move past the loaded
		// tail between the two loads -> tail - head wraps around to a huge number.
		// A green run does not prove correctness; a red run proves a bug.
		SpscQueue<int, 4> q;
		std::atomic<bool> stop{false};

		auto producer = std::async(std::launch::async, [&] {
			int i = 0;
			while (!stop.load(std::memory_order_relaxed)) (void)q.tryPush(i++);
		});
		auto consumer = std::async(std::launch::async, [&] {
			while (!stop.load(std::memory_order_relaxed)) (void)q.tryPop();
		});

		std::uint64_t calls = 0;
		std::uint64_t out_of_range = 0;
		std::size_t example = 0;

		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (out_of_range == 0 && std::chrono::steady_clock::now() < deadline) {
			for (int k = 0; k < 10'000; ++k) {
				const std::size_t s = q.sizeApprox();
				++calls;
				if (s > q.capacity()) {
					++out_of_range;
					example = s;
				}
			}
		}

		stop.store(true, std::memory_order_relaxed);
		producer.get();
		consumer.get();

		INFO("sizeApprox() calls: " << calls << ", out of range: " << out_of_range << ", e.g. " << example);
		REQUIRE(out_of_range == 0);
	}
}
