//
// Created by Vladimir Glushkov on 27.09.2026.
//

#pragma once

#include <functional>
#include <mutex>
#include <utility>
#include <type_traits>
#include <cassert>

template<typename T>
class Synchronized;

namespace details {
	template<typename T>
	struct is_synchronized: std::false_type {};

	template<typename T>
	struct is_synchronized<Synchronized<T>>: std::true_type {};

	template<typename T>
	inline constexpr bool is_synchronized_v = is_synchronized<std::remove_cvref_t<T>>::value;

	template<typename... Ts>
	inline constexpr bool is_all_synchronized_v = (is_synchronized_v<Ts> && ...);

	template<typename TSynchronized>
	using synchronized_inner_ref_t = std::conditional_t<
		std::is_const_v<std::remove_reference_t<TSynchronized>>,
		const typename std::remove_cvref_t<TSynchronized>::value_type&,
		typename std::remove_cvref_t<TSynchronized>::value_type&>;

	template<typename TCallable, typename... TSynchronized>
	inline constexpr bool is_invocable_v = is_all_synchronized_v<TSynchronized...> && std::is_invocable_v<TCallable, synchronized_inner_ref_t<TSynchronized>...>;
}

template<typename TCallable, typename... TSynchronized>
	requires details::is_invocable_v<TCallable, TSynchronized...>
decltype(auto) withLock(TCallable&& callable, TSynchronized&&... syncs);

template<typename T>
class Synchronized final {
	template<typename TCallable, typename... TSynchronized>
	requires details::is_invocable_v<TCallable, TSynchronized...>
	friend decltype(auto) withLock(TCallable&& callable, TSynchronized&&... syncs);

	template<typename TData>
	class BaseProxy {
	public:
		explicit BaseProxy(std::mutex& mutex, TData& data) : m_lck(mutex), m_data(data) {}

		TData* operator->() noexcept {
			return &m_data;
		}

		const TData* operator->() const noexcept {
			return &m_data;
		}
	private:
		std::lock_guard<std::mutex> m_lck;
		TData& m_data;
	};

	using ConstProxy = BaseProxy<const T>;
	using Proxy = BaseProxy<T>;

public:
	using value_type = T;

	template<typename... Args>
	Synchronized(Args&&... args): m_data(std::forward<Args>(args)...) {}
	Synchronized(T&& value) : m_data(std::forward<T>(value)) {}

	Synchronized(const Synchronized&) = delete;
	Synchronized& operator=(const Synchronized&) = delete;

	Synchronized(Synchronized&&) = delete;
	Synchronized& operator=(Synchronized&&) = delete;

	// TODO: rewrite methods using C++23 deducing this feature (not possible yet due to old mingw version)
	template<typename TCallable>
		requires std::is_invocable_v<TCallable, T&>
	decltype(auto) withLock(TCallable&& callable) {
		std::lock_guard lock(m_mutex);
		return std::invoke(std::forward<TCallable>(callable), m_data);
	}

	template<typename TCallable>
		requires std::is_invocable_v<TCallable, const T&>
	decltype(auto) withLock(TCallable&& callable) const {
		std::lock_guard lock(m_mutex);
		return std::invoke(std::forward<TCallable>(callable), m_data);
	}

	Proxy operator->() {
		return Proxy(m_mutex, m_data);
	}

	ConstProxy operator->() const {
		return ConstProxy(m_mutex, m_data);
	}

	bool isLocked() const {
		std::unique_lock lock(m_mutex, std::try_to_lock);
		return !lock.owns_lock();
	}

private:
	mutable std::mutex m_mutex;
	T m_data;
};

// All synchronized objects must be distinct — passing the same object twice is UB (non-recursive std::mutex)
template<typename TCallable, typename... TSynchronized>
	requires details::is_invocable_v<TCallable, TSynchronized...>
decltype(auto) withLock(TCallable&& callable, TSynchronized&&... syncs) {
	// Debug assertion: detect duplicate objects by comparing mutex addresses
	std::array<void*, sizeof...(syncs)> addresses{&syncs.m_mutex...};
	for (std::size_t i = 0; i < sizeof...(syncs); ++i)
		for (std::size_t j = i + 1; j < sizeof...(syncs); ++j)
			assert(addresses[i] != addresses[j] && "Duplicate Synchronized object passed to withLock — causes UB");

	std::scoped_lock lock(syncs.m_mutex...);
	return std::invoke(std::forward<TCallable>(callable), syncs.m_data...);
}
