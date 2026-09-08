//
// Created by Vladimir Glushkov on 02.09.2026.
//

#pragma once

#include <mutex>
#include <condition_variable>
#include <queue>

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
	explicit ThreadSafeQueue(size_t capacity) : m_capacity(capacity) {}

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
		ThreadCounter guard{*this};
		std::unique_lock lock(m_mutex);
		m_cv_not_full.wait(lock, [this] { return m_queue.size() < m_capacity || m_is_shutdown; });
		if (m_is_shutdown) {
			return false;
		}
		m_queue.push(std::forward<U>(value));
		lock.unlock(); // unlock mutex before calling 'notify_one' to avoid Pessimistic wake-up
		m_cv_not_empty.notify_one();
		return true;
	}

	// Remove an element from the queue.
	// Blocks the calling thread if the queue is empty.
	// Returns false if the queue is empty.
	bool pop(T& value) {
		ThreadCounter guard{*this};
		std::unique_lock lock(m_mutex);
		m_cv_not_empty.wait(lock, [this] { return !m_queue.empty() || m_is_shutdown; });
		if (m_queue.empty()) {
			return false;
		}
		value = std::move(m_queue.front());
		m_queue.pop();
		lock.unlock(); // unlock mutex before calling 'notify_one' to avoid Pessimistic wake-up
		m_cv_not_full.notify_one();
		return true;
	}

	// --- Non-blocking (Try) operations ---

	// Attempt to insert an element without blocking the thread.
	// Returns false if the queue is full or stopped.
	template<typename U>
	requires std::is_constructible_v<T, U&&>
	bool try_push(U&& value) {
		std::unique_lock lock(m_mutex);
		if (m_queue.size() == m_capacity || m_is_shutdown) {
			return false;
		}
		m_queue.push(std::forward<U>(value));
		m_cv_not_empty.notify_one();
		return true;
	}

	// Attempt to remove an element without blocking the thread.
	// Returns false if the queue is empty.
	bool try_pop(T& value) {
		std::unique_lock lock(m_mutex);
		if (m_queue.empty()) {
			return false;
		}
		value = std::move(m_queue.front());
		m_queue.pop();
		m_cv_not_full.notify_one();
		return true;
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
