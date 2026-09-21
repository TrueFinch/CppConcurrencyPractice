//
// Created by Vladimir Glushkov on 02.09.2026.
//

#pragma once

#include <mutex>
#include <condition_variable>
#include <queue>
#include <optional>
#include <stdexcept>
#include <utility>
#include <type_traits>
#include <cstddef>

template<typename T>
class ThreadSafeQueue {
	struct WaitCounter {
		size_t& counter;

		explicit WaitCounter(size_t& counter): counter(counter) {
			++counter;
		}

		~WaitCounter() {
			--counter;
		}
	};

public:
	// Constructor: sets the maximum queue capacity
	explicit ThreadSafeQueue(size_t capacity): m_capacity(capacity) {
		if (capacity == 0) {
			throw std::invalid_argument("capacity cannot be zero");
		}
	}

	// Queue lifetime is guaranteed by the caller.
	~ThreadSafeQueue() = default;

	// Copy forbidden (mutexes and condition_variable are not copyable)
	ThreadSafeQueue(const ThreadSafeQueue&) = delete;

	ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

	// Allow or disallow move depending on architectural requirements
	ThreadSafeQueue(ThreadSafeQueue&&) = delete;

	ThreadSafeQueue& operator=(ThreadSafeQueue&&) = delete;

	// --- Blocking operations ---

	// Insert an element into the queue.
	// Blocks the calling thread if the queue is full.
	// Returns false if the queue has been stopped (shutdown).
	template<typename U>
		requires std::is_constructible_v<T, U&&>
	bool push(U&& value) {
		return emplace(std::forward<U>(value));
	}

	// Emplace an element into the queue.
	// Blocks the calling thread if the queue is full.
	// Returns false if the queue has been stopped (shutdown).
	template<typename... Args>
		requires std::is_constructible_v<T, Args...>
	bool emplace(Args&&... args) {
		std::unique_lock lock(m_mutex);
		if (auto predicate = [this] { return m_queue.size() < m_capacity || m_is_shutdown; }; !predicate()) {
			WaitCounter counter(m_waiting_producers);
			m_cv_not_full.wait(lock, predicate);
		}
		if (m_is_shutdown) {
			return false;
		}
		m_queue.emplace(std::forward<Args>(args)...);
		// Unlock before notify_one() to avoid pessimistic wake-up.
		// Queue lifetime is guaranteed by the caller.
		lock.unlock();
		m_cv_not_empty.notify_one();
		return true;
	}

	// Remove an element from the queue.
	// Blocks the calling thread if the queue is empty.
	// Returns false if the queue is empty.
	bool pop(T& value) {
		auto opt = pop();
		if (opt.has_value()) {
			value = std::move(*opt);
			return true;
		}
		return false;
	}

	// Remove an element from the queue
	// Blocks the calling thread if the queue is empty
	// Returns std::nullopt if the queue is empty
	std::optional<T> pop() {
		std::unique_lock lock(m_mutex);
		if (auto predicate = [this] { return !m_queue.empty() || m_is_shutdown; }; !predicate()) {
			WaitCounter counter(m_waiting_consumers);
			m_cv_not_empty.wait(lock, predicate);
		}
		if (m_queue.empty()) {
			return std::nullopt;
		}
		std::optional<T> opt = std::move(m_queue.front());
		m_queue.pop();
		// Unlock before notify_one() to avoid pessimistic wake-up.
		// Queue lifetime is guaranteed by the caller.
		lock.unlock();
		m_cv_not_full.notify_one();
		return opt;
	}

	// --- Non-blocking (Try) operations ---

	// Attempt to insert an element without blocking the thread.
	// Returns false if the queue is full or stopped.
	template<typename U>
		requires std::is_constructible_v<T, U&&>
	bool try_push(U&& value) {
		return try_emplace(std::forward<U>(value));
	}

	template<typename... Args>
		requires std::is_constructible_v<T, Args...>
	bool try_emplace(Args&&... args) {
		std::lock_guard lock(m_mutex);
		if (m_queue.size() == m_capacity || m_is_shutdown) {
			return false;
		}
		m_queue.emplace(std::forward<Args>(args)...);
		m_cv_not_empty.notify_one();
		return true;
	}

	// Attempt to remove an element without blocking the thread.
	// Returns false if the queue is empty.
	bool try_pop(T& value) {
		auto opt = try_pop();
		if (opt.has_value()) {
			value = std::move(*opt);
			return true;
		}
		return false;
	}

	// Attempt to remove an element without blocking the thread.
	// Returns std::nullopt if the queue is empty.
	std::optional<T> try_pop() {
		std::lock_guard lock(m_mutex);
		if (m_queue.empty()) {
			return std::nullopt;
		}
		std::optional<T> opt = std::move(m_queue.front());
		m_queue.pop();
		m_cv_not_full.notify_one();
		return opt;
	}

	// --- State management ---

	// Shutdown signal: unblocks all waiting threads, blocks pushing new elements
	void shutdown() {
		std::scoped_lock lock(m_mutex);
		if (m_is_shutdown) {
			return;
		}
		m_is_shutdown = true;
		m_cv_not_full.notify_all();
		m_cv_not_empty.notify_all();
	}

	// Check if the queue has been stopped
	[[nodiscard]] bool is_shutdown() const {
		std::scoped_lock lock(m_mutex);
		return m_is_shutdown;
	}

	// Current number of elements in the queue
	[[nodiscard]] size_t size() const {
		std::scoped_lock lock(m_mutex);
		return m_queue.size();
	}

	// Check if the queue is empty
	[[nodiscard]] bool empty() const {
		std::scoped_lock lock(m_mutex);
		return m_queue.empty();
	}

	// Maximum queue capacity
	[[nodiscard]] size_t capacity() const {
		return m_capacity;
	}

	[[nodiscard]] size_t waiting_producers() const {
		std::scoped_lock lock(m_mutex);
		return m_waiting_producers;
	}

	[[nodiscard]] size_t waiting_consumers() const {
		std::scoped_lock lock(m_mutex);
		return m_waiting_consumers;
	}

private:
	mutable std::mutex m_mutex;
	std::condition_variable m_cv_not_full; // To block push when queue is full
	std::condition_variable m_cv_not_empty; // To block pop when queue is empty

	std::queue<T> m_queue;
	const size_t m_capacity;
	size_t m_waiting_producers{0};
	size_t m_waiting_consumers{0};
	bool m_is_shutdown{false};
};
