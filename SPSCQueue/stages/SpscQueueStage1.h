//
// Created by Vladimir Glushkov on 11.11.2026.
//

#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <new>
#include <optional>
// ReSharper disable once CppUnusedIncludeDirective
#include <stdexcept>
#include <type_traits>
#include <utility>

// Single-Producer Single-Consumer lock-free bounded queue.
// Frozen snapshot of stage 1 (all memory orders are seq_cst). Do not edit:
// the working version is ../SpscQueue.h. Task and stages — in ../README.md.
template<typename T, std::size_t Capacity>
class SpscQueueStage1 final { // NOLINT(*-pro-type-member-init)
	// Suppressed m_data init warning because
	// T constructs with construct_at, raw bytes never accessed directly

	static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
	using value_type = T;

	SpscQueueStage1() = default;

	// Should be called only after producer and consumer threads ended
	~SpscQueueStage1() {
		const size_t head = m_head;
		const size_t tail = m_tail;
		for (auto i = head; i != tail; ++i) {
			std::destroy_at(slot(i));
		}
	}

	SpscQueueStage1(const SpscQueueStage1&) = delete;

	SpscQueueStage1& operator=(const SpscQueueStage1&) = delete;

	SpscQueueStage1(SpscQueueStage1&&) = delete;

	SpscQueueStage1& operator=(SpscQueueStage1&&) = delete;

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
		const size_t head = m_head;
		const size_t tail = m_tail;
		if (tail - head == Capacity) {
			return false;
		}
		std::construct_at(slot(tail), std::forward<Args>(args)...);
		m_tail.store(m_tail + 1);
		return true;
	}

	// ---------- Consumer side ----------

	std::optional<T> tryPop() {
		const size_t head = m_head;
		const size_t tail = m_tail;
		if (tail - head == 0) {
			return std::nullopt;
		}
		auto* ptr = slot(head);
		std::optional<T> value = std::move(*ptr);
		std::destroy_at(ptr);
		m_head.store(m_head + 1);
		return value;
	}

	// ---------- Any thread ----------

	static constexpr std::size_t capacity() noexcept {
		return Capacity;
	}

	// Value can be outdated soon after return if other thread is active
	// In case if `tail` was loaded first it is possible to get in case
	// when this thread was interrupted, some iterations of pop and push was executed
	// and `head` become greater than `tail`
	// So read `head` first so `tail` value is always on greater or equal than `head`
	[[nodiscard]] std::size_t sizeApprox() const noexcept {
		const auto head = m_head.load();
		const auto tail = m_tail.load();
		assert(tail >= head);
		const auto res = std::min(Capacity, tail - head);
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
	std::atomic<size_t> m_head{0};
	// next free index in array
	std::atomic<size_t> m_tail{0};
	alignas(T) std::byte m_data[Capacity][sizeof(T)];
};
