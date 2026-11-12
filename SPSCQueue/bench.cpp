//
// Created by Vladimir Glushkov on 11.11.2026.
//

// Throughput benchmark: one producer, one consumer.
// Run ONLY in a Release build, otherwise the numbers are meaningless.

#include <array>
#include <chrono>
// ReSharper disable once CppUnusedIncludeDirective
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "SpscQueue.h"
#include "stages/SpscQueueStage1.h"
#include "stages/SpscQueueStage2.h"

// Optional local-only baseline: the benchmark builds without it.
#if __has_include("BlackBoxSpscQueue.h")
#include "BlackBoxSpscQueue.h"
#define HAVE_SPSC_PTR_QUEUE 1
#endif

namespace {

	// Baseline: the same ring buffer, but guarded by a std::mutex.
	template<typename T, std::size_t Capacity>
	class MutexRingQueue {
	public:
		bool tryPush(const T& v) {
			std::lock_guard lock(m_mutex);
			if (m_size == Capacity) return false;
			m_buf[(m_head + m_size) % Capacity] = v;
			++m_size;
			return true;
		}

		std::optional<T> tryPop() {
			std::lock_guard lock(m_mutex);
			if (m_size == 0) return std::nullopt;
			T v = m_buf[m_head];
			m_head = (m_head + 1) % Capacity;
			--m_size;
			return v;
		}

	private:
		std::mutex m_mutex;
		std::array<T, Capacity> m_buf{};
		std::size_t m_head = 0;
		std::size_t m_size = 0;
	};

	template<typename Queue>
	void run(const char* name, std::uint64_t n) {
		auto q = std::make_unique<Queue>();
		std::uint64_t checksum = 0;

		const auto start = std::chrono::steady_clock::now();
		std::thread consumer([&] {
			for (std::uint64_t i = 0; i < n; ++i) {
				std::optional<std::uint64_t> v;
				while (!(v = q->tryPop())) {
					// busy-spin: in HFT, threads usually don't yield the core
				}
				checksum += *v;
			}
		});
		for (std::uint64_t i = 0; i < n; ++i) {
			while (!q->tryPush(i)) {
			}
		}
		consumer.join();
		const auto elapsed = std::chrono::steady_clock::now() - start;

		const double ns = std::chrono::duration<double, std::nano>(elapsed).count();
		const bool ok = checksum == n * (n - 1) / 2;
		std::printf("%-28s %8.2f ns/op  %8.2f Mops/s  %s\n",
			name, ns / static_cast<double>(n), static_cast<double>(n) * 1e3 / ns, ok ? "" : "CHECKSUM MISMATCH!");
	}
}

int main() {
	constexpr std::uint64_t N = 50'000'000;
	constexpr std::size_t Cap = 1024;

	std::printf("items: %llu, capacity: %zu\n\n", static_cast<unsigned long long>(N), Cap);
	for (int rep = 0; rep < 3; ++rep) {
		std::printf("Run #%d\n", rep + 1);
		run<MutexRingQueue<std::uint64_t, Cap>>("MutexRingQueue", N);
		run<SpscQueueStage1<std::uint64_t, Cap>>("SpscQueueStage1", N);
		run<SpscQueueStage2<std::uint64_t, Cap>>("SpscQueueStage2", N);
		run<SpscQueue<std::uint64_t, Cap>>("SpscQueue (current)", N);
#ifdef HAVE_SPSC_PTR_QUEUE
		run<BlackBoxSpscPtrAdapter<Cap>>("blackbox_spsc_queue", N);
#endif
		std::printf("\n");
	}
}
