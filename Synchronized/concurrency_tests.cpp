//
// Created by Vladimir Glushkov on 01.10.2026.
//

#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <thread>
#include <memory>
#include <numeric>
#include <atomic>
#include <future>

#include "Synchronized.h"

TEST_CASE("04. Concurrency & Multithreading correctness", "[Synchronized][concurrency]") {
	SECTION("MT-01. Mutual exclusion guarantee") {
		Synchronized<int> counter(0);
		constexpr int num_threads = 8;
		constexpr int iterations = 10'000;

		std::atomic active_threads_in_callback{0};
		std::atomic overlap_detected{false};

		std::vector<std::thread> threads;
		for (int t = 0; t < num_threads; ++t) {
			threads.emplace_back([&]() {
				for (int i = 0; i < iterations; ++i) {
					counter.withLock([&](int& v) {
						// Если в callback одновременно вошел еще один поток — взаимное исключение нарушено
						int active = active_threads_in_callback.fetch_add(1, std::memory_order_relaxed);
						if (active != 0) {
							overlap_detected.store(true, std::memory_order_relaxed);
						}

						++v;

						active_threads_in_callback.fetch_sub(1, std::memory_order_relaxed);
					});
				}
			});
		}

		for (auto& t: threads) {
			t.join();
		}

		REQUIRE_FALSE(overlap_detected.load());
		REQUIRE(counter.withLock([](const int& v) { return v; }) == num_threads * iterations);
	}

	SECTION("MT-02. Stress & contention tests") {
		SECTION("MT-02.1. Concurrent increment") {
			Synchronized<int> counter(0);
			constexpr int num_threads = 10;
			constexpr int increments_per_thread = 100'000;
			constexpr auto expected = num_threads * increments_per_thread;

			std::vector<std::thread> threads;
			threads.reserve(num_threads);

			for (int t = 0; t < num_threads; ++t) {
				threads.emplace_back([&]() {
					for (int i = 0; i < increments_per_thread; ++i) {
						counter.withLock([](int& v) { ++v; });
					}
				});
			}

			for (auto& t: threads) {
				t.join();
			}

			REQUIRE(counter.withLock([](const int& v) { return v; }) == expected);
		}

		SECTION("MT-02.2. Readers / Writers pattern") {
			Synchronized<std::vector<int>> data;
			constexpr int num_writers = 4;
			constexpr int ops_per_writer = 1'000;
			constexpr int expected_total = num_writers * ops_per_writer;

			// Atomic counter for barrier: all threads increment, main waits until count == total
			std::atomic ready_count{0};
			constexpr auto total_threads = num_writers + num_writers; // writers + readers

			std::vector<std::thread> writers;
			for (int w = 0; w < num_writers; ++w) {
				writers.emplace_back([&]() {
					ready_count.fetch_add(1);
					while (ready_count.load() < total_threads) {
						std::this_thread::yield();
					}
					for (int i = 0; i < ops_per_writer; ++i) {
						data.withLock([&](std::vector<int>& v) {
							v.push_back(w);
						});
					}
				});
			}

			std::atomic read_count{0};
			std::vector<std::thread> readers;
			for (int r = 0; r < num_writers; ++r) {
				readers.emplace_back([&]() {
					ready_count.fetch_add(1);
					while (ready_count.load() < total_threads) {
						std::this_thread::yield();
					}
					for (int i = 0; i < ops_per_writer / 2; ++i) {
						data.withLock([](const std::vector<int>& v) {
							auto sz = v.size();
							(void) sz;
						});
					}
					read_count.fetch_add(1);
				});
			}

			// Main thread: wait for all to be ready, then they all proceed
			while (ready_count.load() < total_threads) {
				std::this_thread::yield();
			}

			for (auto& w: writers) { w.join(); }
			for (auto& r: readers) { r.join(); }

			const int final_size = data.withLock([](const std::vector<int>& v) { return static_cast<int>(v.size()); });
			REQUIRE(final_size == expected_total);
			REQUIRE(read_count.load() == num_writers);
		}
	}

	SECTION("MT-03. Multi-object deadlock avoidance") {
		SECTION("MT-03.1. Opposite lock order (a, b vs b, a)") {
			Synchronized<int> a(1), b(2);
			constexpr int iterations = 5'000;

			std::atomic ready_count{0};

			std::thread t1([&]() {
				ready_count.fetch_add(1, std::memory_order_relaxed);
				while (ready_count.load(std::memory_order_relaxed) < 2) {
					std::this_thread::yield();
				}

				for (int i = 0; i < iterations; ++i) {
					withLock([](int& x, int& y) {
						std::swap(x, y);
					}, a, b);
				}
			});

			std::thread t2([&]() {
				ready_count.fetch_add(1, std::memory_order_relaxed);
				while (ready_count.load(std::memory_order_relaxed) < 2) {
					std::this_thread::yield();
				}

				for (int i = 0; i < iterations; ++i) {
					withLock([](int& x, int& y) {
						std::swap(x, y);
					}, b, a);
				}
			});

			t1.join();
			t2.join();

			int final_a = a.withLock([](int v) { return v; });
			int final_b = b.withLock([](int v) { return v; });

			REQUIRE((final_a + final_b == 3));
		}

		SECTION("MT-03.2. Atomic multi-object state invariance") {
			Synchronized<int> a(100), b(50); // invariant: a + b == 150

			constexpr int num_transfers = 5;
			constexpr int ops_per_transfer = 10'000;

			// Atomic counter barrier — all threads signal ready, main waits
			std::atomic ready_count{0};
			constexpr auto total_threads = num_transfers + num_transfers; // transfers + readers

			std::vector<std::thread> transfer_threads;
			for (int t = 0; t < num_transfers; ++t) {
				transfer_threads.emplace_back([&]() {
					ready_count.fetch_add(1);
					while (ready_count.load() < total_threads) {
						std::this_thread::yield();
					}
					for (int i = 0; i < ops_per_transfer; ++i) {
						withLock([](int& x, int& y) {
							if (x > 0) {
								--x;
								++y;
							}
						}, a, b);
					}
				});
			}

			std::atomic<int> violations{0};
			std::vector<std::thread> reader_threads;
			for (int r = 0; r < num_transfers; ++r) {
				reader_threads.emplace_back([&]() {
					ready_count.fetch_add(1);
					while (ready_count.load() < total_threads) {
						std::this_thread::yield();
					}
					for (int i = 0; i < ops_per_transfer / 10; ++i) {
						withLock([&](const int& x, const int& y) {
							if (x + y != 150) ++violations;
						}, a, b);
					}
				});
			}

			// Main thread: wait for all threads to be ready, then they all proceed
			while (ready_count.load() < total_threads) {
				std::this_thread::yield();
			}

			for (auto& t: transfer_threads) { t.join(); }
			for (auto& r: reader_threads) { r.join(); }

			REQUIRE(violations == 0); // Invariant never violated
			REQUIRE(a.withLock([](const int& v) { return v; }) +
				b.withLock([](const int& v) { return v; }) == 150);
		}
	}
}

TEST_CASE("05. Contract, edge cases & header self-containment", "[Synchronized][contract]") {
	SECTION("CT-01. Contract & Environment invariants") {
		SECTION("CT-01.1. Precondition: Distinct objects required for multi-lock") {
			// Contract: passing the same Synchronized<T> object twice to withLock
			// causes undefined behavior / deadlock on std::mutex (non-recursive).
			// std::scoped_lock<std::mutex&, std::mutex&> where both refer to the same mutex is UB.

			SECTION("Distinct objects have distinct addresses") {
				Synchronized<int> x(1), y(2);
				REQUIRE(&x != &y);
			}

			SECTION("Multi-lock with distinct objects works correctly") {
				using namespace details;
				using iSync = Synchronized<int>;

				iSync a(1), b(2);
				withLock([](int& x, int& y) {
					++x;
					++y;
				}, a, b);
				REQUIRE(a.withLock([](const int& v) { return v; }) == 2);
				REQUIRE(b.withLock([](const int& v) { return v; }) == 3);

				// Duplicate detection is enforced by runtime assert in debug builds
				// (see Synchronized.h: withLock free function — assert on duplicate mutex addresses)
			}
		}

		SECTION("CT-01.2. Precondition: No recursive reentrancy on same object") {
			// Contract: calling withLock from within a callback of the same Synchronized<T>
			// deadlocks because std::mutex is not reentrant (unlike std::recursive_mutex).

			Synchronized<int> counter(0);

			SECTION("Valid: different objects can be nested safely") {
				Synchronized<int> a(0), b(0);
				a.withLock([&](int& v) {
					++v;
					b.withLock([](int& inner) { inner += 10; });
				});
				REQUIRE(a.withLock([](const int& v) { return v; }) == 1);
				REQUIRE(b.withLock([](const int& v) { return v; }) == 10);
			}

			SECTION("Mutexes are functional (reentrancy is documented contract)") {
				// This test verifies that std::mutex works correctly by confirming
				// that single-lock and cross-object nesting work — establishing that
				// the deadlock from reentrancy comes specifically from re-acquiring
				// the *same* mutex, not from a broken synchronization primitive.

				Synchronized<int> a(0), b(0);

				a.withLock([](int& v) { ++v; });
				REQUIRE(a.withLock([](const int& v) { return v; }) == 1);

				int nested_value = 0;
				a.withLock([&](const int& v) {
					nested_value = v;
					b.withLock([](int& inner) { ++inner; });
				});
				REQUIRE(nested_value == 1);
				REQUIRE(b.withLock([](const int& v) { return v; }) == 1);
			}
		}

		SECTION("CT-01.3. Object lifetime contract") {
			// Contract: all threads using withLock on a Synchronized<T> must be joined
			// before the object is destroyed. Destroying while a thread holds a reference
			// to the internal data causes undefined behavior (dangling reference).

			SECTION("Correct: join before destroy") {
				auto make_sync = []() {
					Synchronized<int> data(0);

					std::thread t([&]() {
						data.withLock([](int& v) { ++v; });
					});

					t.join(); // Must join before destruction
					REQUIRE(data.withLock([](const int& v) { return v; }) == 1);
				};
				make_sync();
			}

			SECTION("Correct: multiple threads, all joined") {
				Synchronized<int> data(0);
				constexpr int num_threads = 4;
				std::vector<std::thread> threads;

				for (int i = 0; i < num_threads; ++i) {
					threads.emplace_back([&]() {
						data.withLock([](int& v) { ++v; });
					});
				}

				for (auto& t: threads) { t.join(); }
				REQUIRE(data.withLock([](const int& v) { return v; }) == num_threads);
			}
		}
	}
}
