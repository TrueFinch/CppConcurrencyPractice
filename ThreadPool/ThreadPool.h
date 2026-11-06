//
// Created by Vladimir Glushkov on 12.10.2026.
//

#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class ThreadPool final {
	using Task = std::move_only_function<void()>;

	std::queue<Task> m_queue;
	mutable std::mutex m_mutex;
	std::condition_variable m_cv;
	std::vector<std::jthread> m_workers;
	std::once_flag m_join_once;

	enum class eStopPolicy : uint8_t {
		None,
		Drain,
		Discard,
	} m_stop_policy{eStopPolicy::None};

public:
	explicit ThreadPool(unsigned thread_count = std::thread::hardware_concurrency()) {
		thread_count = std::max(1U, thread_count);
		m_workers.reserve(thread_count);
		for (auto i = 0U; i < thread_count; ++i) {
			m_workers.emplace_back([this] {
				worker_loop();
			});
		}
	}

	~ThreadPool() {
		if (is_worker_thread()) {
			std::cerr << "Thread pool destroyed from worker thread!\n";
			std::terminate();
		}
		join();
	}

	ThreadPool(const ThreadPool&) = delete;

	ThreadPool& operator=(const ThreadPool&) = delete;

	ThreadPool(ThreadPool&&) = delete;

	ThreadPool& operator=(ThreadPool&&) = delete;

	// stop pushing new tasks and discard existing,
	// m_stop_policy changes to `Discard`
	// futures will get future_error with `broken_promise` code
	bool request_stop() {
		{
			// tasks must be declared before lock:
			// * it must be destroyed after m_mutex is released
			// * destroying calls external code that may re-enter pool (cause deadlock)
			//   or run slowly
			std::vector<Task> tasks;
			std::scoped_lock lock(m_mutex);
			if (m_stop_policy == eStopPolicy::Discard) {
				return false;
			}
			m_stop_policy = eStopPolicy::Discard;
			tasks.reserve(m_queue.size());
			while (!m_queue.empty()) {
				tasks.emplace_back(std::move(m_queue.front()));
				m_queue.pop();
			}
		}

		m_cv.notify_all();
		return true;
	}

	// stop pushing new tasks and wait for existing to complete
	// m_stop_policy changes to `Drain`
	bool shutdown() {
		{
			std::scoped_lock lock(m_mutex);
			if (m_stop_policy != eStopPolicy::None) {
				return false;
			}
			m_stop_policy = eStopPolicy::Drain;
		}
		m_cv.notify_all();
		return true;
	}

	bool is_stopped() const {
		std::scoped_lock lock(m_mutex);
		return m_stop_policy != eStopPolicy::None;
	}

	// calling from worker threads causes throw std::system_error
	void join() {
		if (is_worker_thread()) {
			throw std::system_error(
				std::make_error_code(std::errc::resource_deadlock_would_occur),
				"ThreadPool::join called from a worker thread"
			);
		}
		std::call_once(m_join_once, [&]() {
			shutdown();
			for (auto& worker: m_workers) {
				if (worker.joinable()) {
					worker.join();
				}
			}
		});
	}

	template<typename F, typename... Args>
		requires std::is_invocable_v<std::decay_t<F>, std::decay_t<Args>...>
	std::optional<std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>>
	push(F&& f, Args&&... args) {
		using ReturnType = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;
		std::packaged_task<ReturnType()> task([f = std::forward<F>(f), ...args = std::forward<Args>(args)]() mutable {
			return std::invoke(std::move(f), std::move(args)...);
		});
		auto future = task.get_future();

		if (!push_to_queue(std::move(task))) {
			return std::nullopt;
		}

		return future;
	}

private:
	bool push_to_queue(Task&& task) {
		{
			std::scoped_lock lock(m_mutex);
			if (m_stop_policy != eStopPolicy::None) {
				return false;
			}
			m_queue.emplace(std::move(task));
		}
		m_cv.notify_one();
		return true;
	}

	void worker_loop() {
		while (true) {
			Task task;
			{
				std::unique_lock lock(m_mutex);
				m_cv.wait(lock, [&] {
					return m_stop_policy != eStopPolicy::None || !m_queue.empty();
				});
				if (m_stop_policy == eStopPolicy::Discard
					|| (m_stop_policy == eStopPolicy::Drain && m_queue.empty())) {
					return;
				}
				task = std::move(m_queue.front());
				m_queue.pop();
			}
			task();
		}
	}

	bool is_worker_thread() const {
		const auto self_id = std::this_thread::get_id();
		for (auto& worker: m_workers) {
			if (worker.get_id() == self_id) {
				return true;
			}
		}
		return false;
	}
};
