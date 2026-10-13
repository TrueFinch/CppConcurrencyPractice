//
// Created by Vladimir Glushkov on 12.10.2026.
//

#pragma once

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <optional>
#include <type_traits>
#include <utility>
#include <stdexcept>

class ThreadPool final {
	struct Task {
		std::move_only_function<void()> run;
		std::move_only_function<void()> cancel;
	};

	std::queue<Task> m_queue;
	std::mutex m_mutex;
	std::condition_variable m_cv;

	std::vector<std::jthread> m_workers;

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
		auto hc = std::thread::hardware_concurrency();
		hc = hc > 0 ? hc : 1u;
		thread_count = std::clamp(thread_count, 1u, hc);
		m_workers.reserve(thread_count);
		for (auto i = 0u; i < thread_count; ++i) {
			m_workers.emplace_back([this] {
				while (true) {
					Task task;
					{
						std::unique_lock lock(m_mutex);
						m_cv.wait(lock, [&]() {
							return m_stop_policy != eStopPolicy::None || !m_queue.empty();
						});
						if (m_stop_policy == eStopPolicy::Discard
							|| m_stop_policy == eStopPolicy::Drain && m_queue.empty()) {
							return;
						}
						task = std::move(m_queue.front());
						m_queue.pop();
					}
					task.run();
				}
			});
		}
	}

	~ThreadPool() {
		shutdown();
		join();
	}

	ThreadPool(const ThreadPool&) = delete;

	ThreadPool& operator=(const ThreadPool&) = delete;

	ThreadPool(ThreadPool&&) = delete;

	ThreadPool& operator=(ThreadPool&&) = delete;

	// stop pushing new tasks and discard existing
	bool request_stop() {
		std::vector<Task> tasks;
		{
			std::scoped_lock lock(m_mutex);
			if (m_stop_policy != eStopPolicy::None) {
				return false;
			}

			m_stop_policy = eStopPolicy::Discard;
			tasks.reserve(m_queue.size());
			while (!m_queue.empty()) {
				tasks.emplace_back(std::move(m_queue.front()));
				m_queue.pop();
			}
		}

		for (auto& [_, cancel]: tasks) {
			cancel();
		}

		m_cv.notify_all();
		return true;
	}

	// stop pushing new tasks and wait for existing to complete
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

	void join() {
		for (auto& worker: m_workers) {
			if (worker.joinable()) {
				worker.join();
			}
		}
	}

	template<typename F, typename... Args>
		requires std::is_invocable_v<F, Args...>
	std::optional<std::future<std::invoke_result_t<F, Args...>>> push(F&& f, Args&&... args) {
		using ReturnType = std::invoke_result_t<F, Args...>;
		auto promise_ptr = std::make_shared<std::promise<ReturnType>>();
		auto future = promise_ptr->get_future();
		{
			std::scoped_lock lock(m_mutex);
			if (m_stop_policy != eStopPolicy::None) {
				return std::nullopt;
			}
			m_queue.emplace(Task{
				.run = [
					promise_ptr = promise_ptr,
					f = std::forward<F>(f), ...args = std::forward<Args>(args)
				]() mutable {
					try {
						if constexpr (std::is_void_v<ReturnType>) {
							std::invoke(std::move(f), std::move(args)...);
							promise_ptr->set_value();
						} else {
							promise_ptr->set_value(std::invoke(std::move(f), std::move(args)...));
						}
					} catch (...) {
						try {
							promise_ptr->set_exception(std::current_exception());
						} catch (const std::future_error&) {}
					}
				},
				.cancel = [promise_ptr = promise_ptr]() mutable {
					try {
						promise_ptr->set_exception(std::make_exception_ptr(PoolDiscarded{}));
					} catch (const std::future_error&) {}
				}
			});
		}
		m_cv.notify_one();
		return future;
	}
};
