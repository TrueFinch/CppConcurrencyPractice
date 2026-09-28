//
// Created by Vladimir Glushkov on 27.09.2026.
//

#pragma once

#include <functional>
#include <mutex>
#include <utility>
#include <type_traits>

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

public:
	using value_type = T;

	template<typename... Args>
	Synchronized(Args&&... args): m_data(std::forward<Args>(args)...) {}

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

private:
	mutable std::mutex m_mutex;
	T m_data;
};

// All synchronized objects must to be distinct
template<typename TCallable, typename... TSynchronized>
	requires details::is_invocable_v<TCallable, TSynchronized...>
decltype(auto) withLock(TCallable&& callable, TSynchronized&&... syncs) {
	std::scoped_lock lock(syncs.m_mutex...);
	return std::invoke(std::forward<TCallable>(callable), syncs.m_data...);
}
