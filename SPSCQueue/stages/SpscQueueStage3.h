//
// Created by Vladimir Glushkov on 12.11.2026.
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

// Single-Producer Single-Consumer lock-free bounded queue.
// Frozen snapshot of stage 3 (index and data cache lines separated with alignas). Do not edit:
// the working version is ../SpscQueue.h. Task and stages — in ../README.md.
// default kCacheLineSize=64, measured also 128, no differences found
template<typename T, std::size_t Capacity, std::size_t kCacheLineSize = 64>
class SpscQueueStage3 { // NOLINT(*-pro-type-member-init)
	// Suppressed m_data init warning because
	// T constructs with construct_at, raw bytes never accessed directly

	static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
	using value_type = T;

	SpscQueueStage3() = default;

	// Should be called only after producer and consumer threads ended
	~SpscQueueStage3() {
		// The producer and consumer threads have already finished at this point.
		// Joining them establishes happens-before, so relaxed loads see the latest values.
		const std::size_t front = m_front.load(std::memory_order_relaxed);
		const std::size_t back = m_back.load(std::memory_order_relaxed);
		for (auto i = front; i != back; ++i) {
			std::destroy_at(slot(i));
		}
	}

	SpscQueueStage3(const SpscQueueStage3&) = delete;

	SpscQueueStage3& operator=(const SpscQueueStage3&) = delete;

	SpscQueueStage3(SpscQueueStage3&&) = delete;

	SpscQueueStage3& operator=(SpscQueueStage3&&) = delete;

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
		// acquire: pairs with the release store of m_front in tryPop().
		// If we observe the consumer's new front and back - front < Capacity, then the
		// destruction of the element in this slot is complete and it is safe to write to it.
		const std::size_t front = m_front.load(std::memory_order_acquire);
		// Our own index can be loaded relaxed: it is written only by this (producer) thread.
		const std::size_t back = m_back.load(std::memory_order_relaxed);
		if (back - front == Capacity) {
			return false;
		}
		std::construct_at(slot(back), std::forward<Args>(args)...);

		// release: pairs with the acquire load of m_back in tryPop(), establishing
		// happens-before with the consumer that will read and destroy the element.
		// A relaxed store could mark the slot as filled before the element is constructed.
		m_back.store(back + 1, std::memory_order_release);
		return true;
	}

	// ---------- Consumer side ----------

	std::optional<T> tryPop() {
		// Our own index can be loaded relaxed: it is written only by this (consumer) thread.
		const std::size_t front = m_front.load(std::memory_order_relaxed);
		// acquire: pairs with the release store of m_back in tryEmplace().
		// If we observe the producer's new back and back > front, then the construction
		// of the element in this slot is complete and it is safe to read it.
		const std::size_t back = m_back.load(std::memory_order_acquire);
		if (back == front) {
			return std::nullopt;
		}
		auto* ptr = slot(front);
		std::optional<T> value = std::move(*ptr);
		std::destroy_at(ptr);
		// release: pairs with the acquire load of m_front in tryEmplace(), establishing
		// happens-before with the producer that will write a new element into this slot.
		// A relaxed store could mark the slot as free before the element is destroyed.
		m_front.store(front + 1, std::memory_order_release);
		return value;
	}

	// ---------- Any thread ----------

	static constexpr std::size_t capacity() noexcept {
		return Capacity;
	}

	// Value can be outdated soon after return if other thread is active
	[[nodiscard]] std::size_t sizeApprox() const noexcept {
		// front is loaded with acquire to guarantee back >= front, because:
		// 1. the observer's acquire load synchronizes with the consumer's release store of front;
		// 2. before that store, the consumer loaded back and saw a value > front - 1,
		//    otherwise there would have been nothing to pop and no store at all;
		// 3. by the coherence rules (a later load of an atomic cannot observe an older value
		//    than one that happens-before it), the observer reads back no smaller than
		//    the consumer saw, i.e. back >= front.
		// In addition, acquire forbids moving the load of back above the load of front,
		// so back itself can be loaded relaxed.
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
	T* slot(std::size_t index) {
		return std::launder(reinterpret_cast<T*>(m_data[index & (Capacity - 1)]));
	}
	// front element of the queue
	alignas(kCacheLineSize) std::atomic<std::size_t> m_front{0};
	// next free index in array
	alignas(kCacheLineSize) std::atomic<std::size_t> m_back{0};
	alignas(kCacheLineSize) alignas(T) std::byte m_data[Capacity][sizeof(T)];
};
