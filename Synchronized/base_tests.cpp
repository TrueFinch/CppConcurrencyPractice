//
// Created by Vladimir Glushkov on 01.10.2026.
//

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <string>
#include <vector>
#include <memory>
#include <numeric>
#include <utility>
#include <atomic>
#include <thread>
#include <stdexcept>

#include "Synchronized.h"

namespace details {
	template<typename TSynchronized, typename TCallable>
	concept CanCallMemberWithLock = requires(TSynchronized& sync, TCallable&& callable)
	{
		sync.withLock(std::forward<TCallable>(callable));
	};

	template<typename TCallable, typename... TSyncs>
	concept CanCallFreeWithLock = requires(TCallable&& callable, TSyncs&&... syncs)
	{
		withLock(std::forward<TCallable>(callable), std::forward<TSyncs>(syncs)...);
	};
}

namespace freeFunctions {
	void inc(int& i) {
		i += 1;
	}

	int incReturn(int& i) {
		return ++i;
	}

	int sumReturn(int& i1, int& i2) {
		return i1 + i2;
	}
}

TEST_CASE("01. Basic operations: construction, access and constraints", "[Synchronized][basic]") {
	SECTION("BO-01. Construction") {
		SECTION("BO-01.1. Default construction") {
			Synchronized<int> si;
			REQUIRE(si.withLock([](int& v) {return v == 0; }));

			Synchronized<std::string> ss;
			REQUIRE(ss.withLock([](std::string& v) { return v.empty(); }));
		}

		SECTION("BO-01.2. Argument construction") {
			Synchronized<std::string> ss("Hello, World!");
			REQUIRE(ss.withLock([](std::string& v) {return !v.empty() && v.size() == 13; }));
			REQUIRE(ss.withLock([](std::string& v) {return v;}) == "Hello, World!");
		}

		SECTION("BO-01.3. Forwarding construction") {
			Synchronized<std::tuple<int, float, std::string>> st(1, 1.f, "1");
			auto [iv, fv, sv] = st.withLock([](auto v) { return v; });
			REQUIRE((iv == 1 && sv == "1"));
			REQUIRE_THAT(fv, Catch::Matchers::WithinRel(1.0f, 1e-5f));
		}

		SECTION("BO-01.4. Non-copyable/move-only types") {
			Synchronized<std::unique_ptr<int>> sp;
			REQUIRE(sp.withLock([](std::unique_ptr<int>& ptr) { return ptr.get(); }) == nullptr);
			sp.withLock([](std::unique_ptr<int>& ptr) {
				ptr = std::make_unique<int>(1);
			});
			REQUIRE(sp.withLock([](std::unique_ptr<int>& ptr) { return *ptr; }) == 1);
		}
	}

	SECTION("BO-02. Access & Return semantics") {
		Synchronized<int> si(0);
		SECTION("BO-02.1. Mutable access") {
			si.withLock([](int& v) { v = 1; });
			REQUIRE(si.withLock([](int& v){ return v;}) == 1);
		}

		SECTION("BO-02.2. Const access") {
			si.withLock([](int& v) { v = 1; });
			REQUIRE(si.withLock([](const int& v) { return v; }) == 1);
		}

		SECTION("BO-02.3. Void return") {
			REQUIRE_NOTHROW(si.withLock([](int& v) { v = 1; }));
			REQUIRE_NOTHROW(si.withLock([](const int& v) {}));
			REQUIRE(si.withLock([](const int& v) { return v;}) == 1);
		}

		SECTION("BO-02.4. Value return") {
			Synchronized<std::unique_ptr<int>> sp(std::make_unique<int>(1));
			auto ptr = sp.withLock([](std::unique_ptr<int>& v) { return std::move(v); });
			REQUIRE((ptr != nullptr && *ptr == 1));
		}

		SECTION("BO-02.5. Reference preservation (decltype(auto))") {
			int* ptr = nullptr;
			auto& ref = si.withLock([&ptr](int& v) -> int& {
				ptr = &v;
				return v;
			});
			REQUIRE((ptr != nullptr && ptr == &ref));
		}

		SECTION("BO-02.6. Complex type return") {
			const std::string expectedString = "Hello, World!";
			const std::vector expectedVector = {1, 2, 3};
			using pairSV = std::pair<std::string, std::vector<int>>;
			Synchronized<pairSV> sp(expectedString, expectedVector);
			auto pair = sp.withLock([](pairSV& v) { return v; });
			REQUIRE((pair.first == expectedString && pair.second == expectedVector));
			REQUIRE((sp.withLock([](pairSV& v) {return v.first; }) == expectedString
				&& sp.withLock([](pairSV& v) {return v.second; }) == expectedVector
			));
		}
	}

	SECTION("BO-03. Constraints & Compile-time checks") {
		using namespace details;
		using ivec = std::vector<int>;
		using iSync = Synchronized<int>;
		using ivecSync = Synchronized<ivec>;
		using iSyncConst = const iSync;
		using ivecSyncConst = const ivecSync;

		SECTION("BO-03.1. Mutable T& constraint validation") {
			STATIC_CHECK(CanCallMemberWithLock<iSync, void(int&)>);
			STATIC_CHECK(CanCallMemberWithLock<iSync, decltype([](int&){})>);
			STATIC_CHECK(CanCallMemberWithLock<ivecSync, decltype(&ivec::empty)>);
			STATIC_CHECK(CanCallFreeWithLock<decltype([](int&, ivec&){}), iSync, ivecSync>);
		}

		SECTION("BO-03.2. Const T& constraint validation") {
			STATIC_CHECK(CanCallMemberWithLock<iSyncConst, void(const int&)>);
			STATIC_CHECK(CanCallMemberWithLock<iSyncConst, decltype([](const int&){})>);
			STATIC_CHECK(CanCallMemberWithLock<ivecSyncConst, decltype(&ivec::empty)>);
			STATIC_CHECK(CanCallFreeWithLock<decltype([](const int&, const ivec&){}), iSyncConst, ivecSyncConst>);
		}

		SECTION("BO-03.3. Const mutation rejected") {
			STATIC_CHECK_FALSE(CanCallMemberWithLock<const Synchronized<int>, void(int&)>);
			STATIC_CHECK_FALSE(CanCallMemberWithLock<const Synchronized<int>, decltype([](int&){})>);
		}

		SECTION("BO-03.4. Invalid callable rejected") {
			STATIC_CHECK_FALSE(CanCallMemberWithLock<iSyncConst, void(int&)>);
			STATIC_CHECK_FALSE(CanCallMemberWithLock<iSyncConst, decltype([](int&){})>);
			// ide highlights error, but compilations is ok
			// ReSharper disable once CppStaticAssertFailure
			STATIC_CHECK_FALSE(CanCallMemberWithLock<ivecSyncConst, decltype(&ivec::pop_back)>);
			STATIC_CHECK_FALSE(CanCallFreeWithLock<decltype([](int&, ivec&){}), iSyncConst, ivecSync>);
			STATIC_CHECK_FALSE(CanCallFreeWithLock<decltype([](int&, ivec&){}), iSync, ivecSyncConst>);
		}

		SECTION("BO-03.5. Invalid arguments order rejected") {
			STATIC_CHECK_FALSE(CanCallFreeWithLock<decltype([](ivec&, int&){}), iSync, ivecSync>);
		}
	}

	SECTION("BO-04. Callable variants & std::invoke support") {
		Synchronized<int> si(0);
		SECTION("BO-04.1. Stateless and Stateful Lambda") {
			std::vector v(100, 1);
			REQUIRE_NOTHROW(si.withLock([v](int& i) {
				i = std::accumulate(v.begin(), v.end(), 0);
				}));
			const auto expected = std::accumulate(v.begin(), v.end(), 0);
			REQUIRE(si.withLock([](int& i) { return i; }) == expected);
		}

		SECTION("BO-04.2. Mutable lambda") {
			auto statefullLambda = [callCount = 0](int& i) mutable {
				i = ++callCount;
				return callCount;
			};
			REQUIRE(si.withLock(statefullLambda) == 1);
			REQUIRE(si.withLock([](int& i) { return i; }) == 1);
			REQUIRE(si.withLock(statefullLambda) == 2);
			REQUIRE(si.withLock([](int& i) { return i; }) == 2);
			REQUIRE(si.withLock(statefullLambda) == 3);
			REQUIRE(si.withLock([](int& i) { return i; }) == 3);

			Synchronized<int> i1(1), i2(3);
			auto sum = [acc = 0](int& i1, int& i2) mutable {
				acc += i1 + i2;
				return acc;
			};
			REQUIRE(withLock(sum, i1, i2) == 4);
			REQUIRE(withLock(sum, i1, i2) == 8);
		}

		SECTION("BO-04.3. Function pointer") {
			using namespace details;

			using iSync = Synchronized<int>;
			using iSyncConst = const iSync;

			STATIC_CHECK(CanCallMemberWithLock<iSync, decltype(freeFunctions::inc)>);
			STATIC_CHECK(CanCallMemberWithLock<iSync, decltype(freeFunctions::incReturn)>);

			STATIC_CHECK_FALSE(CanCallMemberWithLock<iSyncConst, decltype(freeFunctions::inc)>);
			STATIC_CHECK_FALSE(CanCallMemberWithLock<iSyncConst, decltype(freeFunctions::incReturn)>);

			si.withLock(freeFunctions::inc);
			REQUIRE(si.withLock([](int& i) {return i;}) == 1);
			REQUIRE(si.withLock(freeFunctions::incReturn) == 2);

			Synchronized<int> si2(4);
			REQUIRE(withLock(freeFunctions::sumReturn, si, si2) == 6);
		}

		SECTION("BO-04.4. Functor") {
			struct summator {
				int operator()(int& i) const { return i + i; }

				int operator()(int& i1, int& i2) const { return i1 + i2; }
			} summator;

			using namespace details;
			using iSync = Synchronized<int>;
			using fSync = Synchronized<float>;

			STATIC_CHECK(CanCallMemberWithLock<iSync, struct summator>);
			STATIC_CHECK(CanCallFreeWithLock<struct summator, iSync, iSync>);
			STATIC_CHECK_FALSE(CanCallFreeWithLock<struct summator, iSync, fSync>);

			Synchronized<int> i1(1), i2(3);
			REQUIRE(i1.withLock(summator) == 2);
			REQUIRE(i2.withLock(summator) == 6);
			REQUIRE(withLock(summator, i1, i2) == 4);
		}

		SECTION("BO-04.5. Move-only callable") {
			struct MoveOnlyMultiplier {
				std::unique_ptr<int> multiplier;

				explicit MoveOnlyMultiplier(int i): multiplier(std::make_unique<int>(i)) {}

				MoveOnlyMultiplier(MoveOnlyMultiplier&) = delete;

				MoveOnlyMultiplier& operator=(MoveOnlyMultiplier&) = delete;

				MoveOnlyMultiplier(MoveOnlyMultiplier&&) = default;

				MoveOnlyMultiplier& operator=(MoveOnlyMultiplier&&) = default;

				int operator()(int& i) const {
					i *= *multiplier;
					return i;
				}

				int operator()(int& i1, int& i2) const {
					return (i1 + i2) * *multiplier;
				}
			} mvoMul(2);

			using namespace details;
			using iSync = Synchronized<int>;
			using fSync = Synchronized<float>;

			STATIC_CHECK(CanCallMemberWithLock<iSync, MoveOnlyMultiplier>);
			STATIC_CHECK(CanCallFreeWithLock<MoveOnlyMultiplier, iSync, iSync>);
			STATIC_CHECK_FALSE(CanCallFreeWithLock<MoveOnlyMultiplier, iSync, fSync>);
			STATIC_CHECK(CanCallMemberWithLock<iSync,
						decltype([p = std::make_unique<int>(5)](int& i) {return i * *p;})>);

			si.withLock([](int& i) { i = 5; });
			REQUIRE(si.withLock([p = std::make_unique<int>(5)](int& i) {return i * *p;}) == 25);

			REQUIRE(si.withLock(mvoMul) == 10);
			REQUIRE(si.withLock([](int& i) {return i; }) == 10);

			iSync si2(5);
			REQUIRE(withLock([p = std::make_unique<int>(2)](int& i1, int& i2) {
				return (i1 + i2) * *p;
				}, si, si2) == 30);
			REQUIRE(withLock(MoveOnlyMultiplier(3), si, si2) == 45);
		}

		SECTION("BO-04.6. Pointer to member function") {
			using namespace details;
			using ivec = std::vector<int>;
			using vSync = Synchronized<ivec>;
			using vSyncConst = const Synchronized<ivec>;

			Synchronized<ivec> sv;
			STATIC_CHECK(CanCallMemberWithLock<vSync, decltype(&ivec::empty)>);
			STATIC_CHECK(CanCallMemberWithLock<const vSync, decltype(&ivec::empty)>);
			// can not call clear on const so this is correct. Disable resharper for this line cause it is wrong
			// ReSharper disable once CppStaticAssertFailure
			STATIC_CHECK_FALSE(CanCallMemberWithLock<vSyncConst, decltype(&ivec::clear)>);

			REQUIRE(sv.withLock(&ivec::empty));
			sv.withLock([](ivec& v) { v.push_back(0); });
			const auto& svConst = sv;
			REQUIRE_FALSE(svConst.withLock(&ivec::empty));
			sv.withLock(&ivec::clear);
			REQUIRE(sv.withLock(&ivec::empty));
		}

		SECTION("BO-04.7. Pointer to member data") {
			struct Point {
				int x{0};
				int y{0};
			};

			using namespace details;
			using SyncPoint = Synchronized<Point>;
			using ConstSyncPoint = const Synchronized<Point>;

			STATIC_CHECK(CanCallMemberWithLock<SyncPoint, decltype(&Point::x)>);
			STATIC_CHECK(CanCallMemberWithLock<ConstSyncPoint, decltype(&Point::x)>);

			SyncPoint point(10, 20);

			REQUIRE(point.withLock(&Point::x) == 10);
			REQUIRE(point.withLock(&Point::y) == 20);

			int& xRef = point.withLock(&Point::x);
			xRef = 42;
			REQUIRE(point.withLock(&Point::x) == 42);

			const ConstSyncPoint constPoint(100, 200);
			REQUIRE(constPoint.withLock(&Point::x) == 100);
			REQUIRE(constPoint.withLock(&Point::y) == 200);
		}
	}
}

TEST_CASE("02. Multi-object operations: free function withLock", "[Synchronized][multi_lock]") {
	SECTION("ML-01. Multi-lock execution") {
		SECTION("ML-01.1. Two objects locking and swapping") {
			Synchronized<int> a(42), b(99);

			withLock([](int& x, int& y) { std::swap(x, y); }, a, b);

			REQUIRE(a.withLock([](const int& v) { return v; }) == 99);
			REQUIRE(b.withLock([](const int& v) { return v; }) == 42);
		}

		SECTION("ML-01.2. Three or more objects (variadic parameter pack)") {
			Synchronized<int> si(1);
			Synchronized<double> sd(2.5);
			Synchronized<std::string> ss("hello");

			withLock([](int& i, double& d, std::string& s) {
				i = static_cast<int>(d);
				d += i;
				s.append(std::to_string(i));
			}, si, sd, ss);

			REQUIRE(si.withLock([](const int& v) { return v; }) == 2);
			REQUIRE(sd.withLock([](const double& v) { return v; }) == 4.5);
			REQUIRE(ss.withLock([](const std::string& v) { return v; }) == "hello2");
		}

		SECTION("ML-01.3. Mixed const and non-const objects") {
			Synchronized<int> si(10);
			const Synchronized<double> sd(3.14);

			withLock([](int& i, const double& d) {
				i = static_cast<int>(d * 2);
			}, si, sd);

			REQUIRE(si.withLock([](const int& v) { return v; }) == 6);
			REQUIRE(sd.withLock([](const double& v) { return v; }) == 3.14);
		}

		SECTION("ML-01.4. All const objects multi-lock") {
			const Synchronized<int> si(42);
			const Synchronized<std::string> ss("test");

			auto result = withLock([](const int& i, const std::string& s) {
				return i + static_cast<int>(s.size());
			}, si, ss);

			REQUIRE(result == 46);
		}
	}

	SECTION("ML-02. Multi-lock semantics & constraints") {
		SECTION("ML-02.1. Parameter order preservation") {
			Synchronized<int> first(1), second(2);

			REQUIRE(withLock([](int& a, int& b) {
				return (a == 2 && b == 1);
			}, second, first));
		}

		SECTION("ML-02.2. Return value from multi-lock") {
			Synchronized<int> si(10);
			Synchronized<int> sj(20);

			SECTION("Return by value") {
				auto sum = withLock([](const int& a, const int& b) -> int {
					return a + b;
				}, si, sj);
				REQUIRE(sum == 30);
			}

			SECTION("Return by reference") {
				int* ptr = nullptr;
				auto& ref = withLock([&ptr](int& a, int& b) -> int& {
					a += b;
					ptr = &a;
					return a;
				}, si, sj);
				REQUIRE(&ref == ptr); // ref points same object as captured pointer
				REQUIRE(si.withLock([](const int& v) { return v; }) == 30);
				REQUIRE(sj.withLock([](const int& v) { return v; }) == 20);
			}
		}

		SECTION("ML-02.3. Incompatible multi-lock callable rejected") {
			using namespace details;
			using iSync = Synchronized<int>;

			STATIC_CHECK_FALSE(CanCallFreeWithLock<
				decltype([](int&, int&, int&) {}), iSync, iSync>);

			STATIC_CHECK_FALSE(CanCallFreeWithLock<
				decltype([](double&, double&) {}), iSync, iSync>);

			STATIC_CHECK_FALSE(CanCallFreeWithLock<
				decltype([](int&) {}), iSync, iSync>);
		}
	}
}

TEST_CASE("03. Exception safety", "[Synchronized][exceptions]") {
	SECTION("EX-01. Single-object exception handling") {
		SECTION("EX-01.1. Exception releases lock") {
			Synchronized<int> si(0);

			REQUIRE_THROWS(si.withLock([](int& v) {
				v = 42;
				throw std::runtime_error("test exception");
			}));

			std::atomic completed{false};
			std::thread t([&]() {
				si.withLock([](int& v) { v += 1; });
				completed = true;
			});
			t.join();
			REQUIRE(completed);
		}

		SECTION("EX-01.2. Object state usability after exception") {
			Synchronized<std::vector<int>> sv;

			REQUIRE_THROWS(sv.withLock([](std::vector<int>& v) {
				v.push_back(1);
				v.push_back(2);
				throw std::runtime_error("abort");
			}));

			REQUIRE(sv.withLock([](const std::vector<int>& v) {
				return v.size() == 2 && v[0] == 1 && v[1] == 2;
			}));

			sv.withLock([](std::vector<int>& v) { v.push_back(3); });
			REQUIRE(sv.withLock([](const std::vector<int>& v) { return v.size(); }) == 3);
		}
	}

	SECTION("EX-02. Multi-object exception handling") {
		SECTION("EX-02.1. Exception releases all acquired locks") {
			Synchronized<int> si(0);
			Synchronized<double> sd(0.0);

			REQUIRE_THROWS(withLock([](int& i, double& d) {
				i = 99;
				throw std::runtime_error("multi-lock exception");
				(void)d;
			}, si, sd));
			REQUIRE(withLock([](int& i, double& d) {
				return i == 99 && d == 0.f;
			}, si, sd));
			std::atomic<bool> done1{false}, done2{false};
			std::thread t1([&]() {
				si.withLock([](int& v) { v = 1; });
				done1 = true;
			});
			std::thread t2([&]() {
				sd.withLock([](double& v) { v = 1.0; });
				done2 = true;
			});
			t1.join();
			t2.join();

			REQUIRE((done1 && done2));
		}

		SECTION("EX-02.2. All objects usable after multi-lock exception") {
			Synchronized<int> a(1), b(2), c(3);

			REQUIRE_THROWS(withLock([](int& x, int& y, int& z) {
				x = 100;
				throw std::logic_error("fail");
				(void)y; (void)z;
			}, a, b, c));

			REQUIRE(a.withLock([](const int& v) { return v; }) == 100);
			REQUIRE(b.withLock([](const int& v) { return v; }) == 2);
			REQUIRE(c.withLock([](const int& v) { return v; }) == 3);

			withLock([](int& x, int& y, int& z) { x += y + z; }, a, b, c);
			REQUIRE(a.withLock([](const int& v) { return v; }) == 105);
		}
	}
}
