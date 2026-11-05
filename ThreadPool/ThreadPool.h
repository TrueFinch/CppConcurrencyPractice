//
// Created by Vladimir Glushkov on 12.10.2026.
//

#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class ThreadPool final {
	template<typename ReturnType>
	struct CallableGuard {
		std::promise<ReturnType> promise;
		bool done = false;

		CallableGuard() = default;

		CallableGuard(CallableGuard&& other) noexcept
			: promise(std::move(other.promise))
			, done(std::exchange(other.done, true)) {}

		CallableGuard(const CallableGuard&) = delete;

		CallableGuard& operator=(const CallableGuard&) = delete;

		CallableGuard& operator=(CallableGuard&&) = delete;

		~CallableGuard() {
			if (done) {
				return;
			}
			promise.set_exception(std::make_exception_ptr(PoolDiscarded{}));
		}
	};

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
	struct PoolDiscarded: std::logic_error {
		PoolDiscarded(): logic_error("ThreadPool: task discarded on shutdown!") {}
	};

	explicit ThreadPool(unsigned thread_count = std::thread::hardware_concurrency()) {
		thread_count = std::max(1u, thread_count);
		m_workers.reserve(thread_count);
		for (auto i = 0u; i < thread_count; ++i) {
			m_workers.emplace_back([this] {
				worker_loop();
			});
		}
	}

	~ThreadPool() {
		const auto self_id = std::this_thread::get_id();
		for (auto& worker: m_workers) {
			if (worker.get_id() == self_id) {
				std::cerr << "Thread pool destroyed from worker thread!\n";
				std::terminate();
			}
		}
		join();
	}

	ThreadPool(const ThreadPool&) = delete;

	ThreadPool& operator=(const ThreadPool&) = delete;

	ThreadPool(ThreadPool&&) = delete;

	ThreadPool& operator=(ThreadPool&&) = delete;

	// stop pushing new tasks and discard existing
	// m_stop_policy -> Discard
	bool request_stop() {
		std::vector<Task> tasks;
		{
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
	// m_stop_policy -> Drain
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

	void join() {
		std::call_once(m_join_once, [this] {
			shutdown();
			const auto self_id = std::this_thread::get_id();
			for (auto& worker: m_workers) {
				if (worker.joinable() && worker.get_id() != self_id) {
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
		CallableGuard<ReturnType> callable_guard;
		auto future = callable_guard.promise.get_future();

		if (!push_to_queue(make_task(std::move(callable_guard), std::forward<F>(f), std::forward<Args>(args)...))) {
			return std::nullopt;
		}

		return future;
	}

private:
	template<typename ReturnType, typename F, typename... Args>
	Task make_task(CallableGuard<ReturnType>&& guard, F&& f, Args&&... args) {
		return [guard = std::move(guard), f = std::forward<F>(f), ...args = std::forward<Args>(args)]() mutable {
			try {
				if constexpr (std::is_void_v<ReturnType>) {
					std::invoke(std::move(f), std::move(args)...);
					guard.promise.set_value();
				} else {
					guard.promise.set_value(std::invoke(std::move(f), std::move(args)...));
				}
			} catch (...) {
				guard.promise.set_exception(std::current_exception());
			}
			guard.done = true;
		};
	}

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
};
