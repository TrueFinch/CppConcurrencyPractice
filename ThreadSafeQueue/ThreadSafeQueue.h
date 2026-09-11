//
// Created by Vladimir Glushkov on 02.09.2026.
//

#pragma once

#include <mutex>
#include <condition_variable>
#include <queue>
#include <optional>

template<typename T>
class ThreadSafeQueue {
	struct ThreadCounter {
		const ThreadSafeQueue& q;
		ThreadCounter(const ThreadSafeQueue& queue) : q(queue) {
			++q.m_active_thread_counter;
		}
		~ThreadCounter() {
			std::lock_guard lock(q.m_mutex);
			if (--q.m_active_thread_counter == 0) {
				q.m_cv_destructor.notify_all();
			}
		}
	};
public:
	// Constructor: sets the maximum queue capacity
	explicit ThreadSafeQueue(size_t capacity) : m_capacity(capacity) {
		if (capacity == 0) {
			throw std::invalid_argument("capacity cannot be zero");
		}
	}

	// Destructor: automatically calls shutdown() if the queue is still active
	~ThreadSafeQueue() {
		shutdown();
		std::unique_lock lock(m_mutex);
		m_cv_destructor.wait(lock, [this] { return m_active_thread_counter == 0; });
	}

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
		ThreadCounter guard{*this};
		std::unique_lock lock(m_mutex);
		m_cv_not_full.wait(lock, [this] { return m_queue.size() < m_capacity || m_is_shutdown; });
		if (m_is_shutdown) {
			return false;
		}
		m_queue.emplace(std::forward<Args>(args)...);
		// unlock mutex before calling 'notify_one' to avoid Pessimistic wake-up
		// can do that because we have ThreadCounter guarding
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
		ThreadCounter guard{*this};
		std::unique_lock lock(m_mutex);
		m_cv_not_empty.wait(lock, [this] { return !m_queue.empty() || m_is_shutdown; });
		if (m_queue.empty()) {
			return std::nullopt;
		}
		std::optional<T> opt = std::move(m_queue.front());
		m_queue.pop();
		// unlock mutex before calling 'notify_one' to avoid Pessimistic wake-up
		// can do that because we have ThreadCounter guarding
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

private:
	mutable std::mutex m_mutex;
	mutable std::condition_variable m_cv_destructor; // To block destruction when there are active threads
	mutable std::atomic<int> m_active_thread_counter{0};

	std::condition_variable m_cv_not_full;  // To block push when queue is full
	std::condition_variable m_cv_not_empty; // To block pop when queue is empty

	std::queue<T> m_queue;
	const size_t m_capacity;
	bool m_is_shutdown{false};
};
