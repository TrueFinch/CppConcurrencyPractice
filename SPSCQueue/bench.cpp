//
// Created by Vladimir Glushkov on 11.11.2026.
//

// Throughput benchmark: one producer, one consumer.
// Run ONLY in a Release build, otherwise the numbers are meaningless.
//
// Usage: SpscQueueBench [producer_cpu consumer_cpu]
// Both threads are pinned to fixed CPUs (default 0 and 2), otherwise the OS may place them on
// hyper-threading siblings, different cores or (on hybrid CPUs) P- and E-cores, and the results
// jump by several times between runs. Pass -1 -1 to disable pinning.

#include <array>
#include <chrono>
// ReSharper disable once CppUnusedIncludeDirective
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#include <future>

#include "SpscQueue.h"
#include "stages/SpscQueueStage1.h"
#include "stages/SpscQueueStage2.h"
#include "stages/SpscQueueStage3.h"

// Optional local-only baseline: the benchmark builds without it.
#if __has_include("BlackBoxSpscQueue.h")
#include "BlackBoxSpscQueue.h"
#define HAVE_SPSC_PTR_QUEUE 1
#endif

namespace {

	// Spin-wait hint (PAUSE on x86): lowers the rate at which the waiting thread polls the other
	// thread's cache line and avoids the memory-order machine clear when the spin loop exits.
	// All queues are polled through this loop, so they are compared under equal conditions.
	inline void cpuRelax() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
		_mm_pause();
#elif defined(__aarch64__)
		asm volatile("yield");
#endif
	}

	int g_producerCpu = 0;
	int g_consumerCpu = 2;

	// RAII: pins the calling thread to one CPU and restores its previous affinity on destruction,
	// so a thread we don't own (e.g. a pool thread behind std::async) is returned unchanged.
	// cpu < 0 means "do not pin".
	class ScopedCpuPin {
	public:
		explicit ScopedCpuPin(int cpu) {
			if (cpu < 0) return;
#ifdef __linux__
			if (const int err = pthread_getaffinity_np(pthread_self(), sizeof(m_saved), &m_saved); err != 0) {
				std::fprintf(stderr, "warning: failed to read thread affinity (error %d)\n", err);
				return;
			}
			cpu_set_t set;
			CPU_ZERO(&set);
			CPU_SET(cpu, &set);
			if (const int err = pthread_setaffinity_np(pthread_self(), sizeof(set), &set); err != 0) {
				std::fprintf(stderr, "warning: failed to pin thread to CPU %d (error %d)\n", cpu, err);
				return;
			}
			m_pinned = true;
#else
			static bool warned = false;
			if (!warned) {
				std::fprintf(stderr, "warning: thread pinning is implemented only for Linux\n");
				warned = true;
			}
#endif
		}

		~ScopedCpuPin() {
#ifdef __linux__
			if (m_pinned) {
				pthread_setaffinity_np(pthread_self(), sizeof(m_saved), &m_saved);
			}
#endif
		}

		ScopedCpuPin(const ScopedCpuPin&) = delete;
		ScopedCpuPin& operator=(const ScopedCpuPin&) = delete;

	private:
#ifdef __linux__
		cpu_set_t m_saved{};
#endif
		bool m_pinned = false;
	};

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
			const ScopedCpuPin pin(g_consumerCpu);
			for (std::uint64_t i = 0; i < n; ++i) {
				std::optional<std::uint64_t> v;
				while (!(v = q->tryPop())) {
					// busy-spin: in HFT, threads usually don't yield the core
					cpuRelax();
				}
				checksum += *v;
			}
		});
		for (std::uint64_t i = 0; i < n; ++i) {
			while (!q->tryPush(i)) {
				cpuRelax();
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

int main(int argc, char** argv) {
	constexpr std::uint64_t N = 50'000'000;
	constexpr std::size_t Cap = 1024;

	if (argc == 3) {
		g_producerCpu = std::atoi(argv[1]);
		g_consumerCpu = std::atoi(argv[2]);
	} else if (argc != 1) {
		std::fprintf(stderr, "usage: %s [producer_cpu consumer_cpu]\n", argv[0]);
		return 1;
	}
	// The main thread is the producer; it stays pinned until main returns.
	const ScopedCpuPin pin(g_producerCpu);

	std::printf("items: %llu, capacity: %zu, producer cpu: %d, consumer cpu: %d\n\n",
		static_cast<unsigned long long>(N), Cap, g_producerCpu, g_consumerCpu);
	for (int rep = 0; rep < 3; ++rep) {
		std::printf("Run #%d\n", rep + 1);
		run<MutexRingQueue<std::uint64_t, Cap>>("MutexRingQueue", N);
		run<SpscQueueStage1<std::uint64_t, Cap>>("SpscQueueStage1", N);
		run<SpscQueueStage2<std::uint64_t, Cap>>("SpscQueueStage2", N);
		run<SpscQueueStage3<std::uint64_t, Cap>>("SpscQueueStage3", N);
		run<SpscQueue<std::uint64_t, Cap>>("SpscQueue (current)", N);
#ifdef HAVE_SPSC_PTR_QUEUE
		run<BlackBoxSpscPtrAdapter<Cap>>("blackbox_spsc_queue", N);
#endif
		std::printf("\n");
	}
}
