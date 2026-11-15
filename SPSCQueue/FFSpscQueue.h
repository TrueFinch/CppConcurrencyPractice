//
// Created by Vladimir Glushkov on 14.11.2026.
//

#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

// Fast Forward Single-Producer Single-Consumer lock-free bounded queue.
// Inspired by https://www.researchgate.net/publication/213894711_FastForward_for_Efficient_Pipeline_Parallelism_A_Cache-Optimized_Concurrent_Lock-Free_Queue.
template<typename T, std::size_t Capacity, std::size_t kCacheLineSize = 64>
class FFSpscQueue {
	static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
	using value_type = T;

	FFSpscQueue() = default;

	// Should be called only after producer and consumer threads ended
	~FFSpscQueue() {
		// The producer and consumer threads have already finished at this point.
		// Joining them establishes happens-before, so relaxed loads see the latest values.
		const std::size_t front = m_front.load(std::memory_order_relaxed);
		const std::size_t back = m_back.load(std::memory_order_relaxed);
		for (auto i = front; i != back; ++i) {
			std::destroy_at(slot(i).get());
		}
	}

	FFSpscQueue(const FFSpscQueue&) = delete;

	FFSpscQueue& operator=(const FFSpscQueue&) = delete;

	FFSpscQueue(FFSpscQueue&&) = delete;

	FFSpscQueue& operator=(FFSpscQueue&&) = delete;

	// ---------- Producer side ----------

	bool tryPush(const T& value) {
		return tryEmplace(value);
	}

	bool tryPush(T&& value) {
		return tryEmplace(std::move(value));
	}

	template<typename... Args>
		requires std::is_constructible_v<T, Args...>
	bool tryEmplace(Args&&... args) {
		// Producer own index can be loaded relaxed: it is written only by this thread.
		const auto back = m_back.load(std::memory_order_relaxed);
		auto& back_slot = slot(back);
		if (back_slot.busy.load(std::memory_order_acquire) == true) {
			return false;
		}
		std::construct_at(back_slot.get(), std::forward<Args>(args)...);

		// m_back is read by sizeApprox() from other threads. relaxed is enough here:
		// the store is published by the release store of the busy flag below, so it must stay before it.
		m_back.store(back + 1, std::memory_order_relaxed);
		// release: pairs with acquire load of Slot::busy flag in tryPop(),
		// establishing happens-before with the consumer that will read and destroy the element.
		// A relaxed store could mark the slot as filled before the element is constructed.
		back_slot.busy.store(true, std::memory_order_release);
		return true;
	}

	// ---------- Consumer side ----------

	std::optional<T> tryPop() {
		// Consumer own index can be loaded relaxed: it is written only by this thread.
		const auto front = m_front.load(std::memory_order_relaxed);
		auto& front_slot = slot(front);
		if (front_slot.busy.load(std::memory_order_acquire) == false) {
			return std::nullopt;
		}
		auto* ptr = front_slot.get();
		std::optional<T> value = std::move(*ptr);
		std::destroy_at(ptr);
		// release: pairs with acquire load of Slot::busy flag in tryEmplace(),
		// establishing happens-before with the producer that will write a new element into this slot.
		// A relaxed store could mark the slot as free before the element is destroyed.
		front_slot.busy.store(false, std::memory_order_release);

		// release: pairs with acquire load of m_front in sizeApprox()
		m_front.store(front + 1, std::memory_order_release);
		return value;
	}

	// ---------- Any thread ----------

	static constexpr std::size_t capacity() noexcept {
		return Capacity;
	}

	// Value can be outdated soon after return if other thread is active.
	// Why back >= front:
	//  1. producer: m_back.store(b) is sequenced before busy.store(true, release);
	//  2. consumer: busy.load(acquire) synchronizes with it, then m_front.store(f, release);
	//  3. here: m_front.load(acquire) synchronizes with that store,
	//     so m_back.store(b) happens-before our m_back load;
	//  4. by coherence our m_back load returns b or a later value, and b >= f.
	[[nodiscard]] std::size_t sizeApprox() const noexcept {
		const auto front = m_front.load(std::memory_order_acquire);
		const auto back = m_back.load(std::memory_order_relaxed);
		assert(back >= front);
		const auto res = std::min(Capacity, back - front);
		return res;
	}

	[[nodiscard]] bool emptyApprox() const noexcept {
		return sizeApprox() == 0;
	}

private:
	struct Slot {
		// synchronization flag
		std::atomic<bool> busy{false};
		alignas(T) std::byte value[sizeof(T)]{};

		T* get() {
			return std::launder(reinterpret_cast<T*>(value));
		}
	};

	Slot& slot(std::size_t index) {
		return m_data[index & (Capacity - 1)];
	}

	// Indices are atomic only because sizeApprox() reads them from other threads;
	// producer and consumer synchronize through Slot::busy.
	// front element of the queue
	alignas(kCacheLineSize) std::atomic<std::size_t> m_front{0};
	// next free index in array
	alignas(kCacheLineSize) std::atomic<std::size_t> m_back{0};

	alignas(kCacheLineSize) Slot m_data[Capacity]{};
};

