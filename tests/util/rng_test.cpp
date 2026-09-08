#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "util/rng.hpp"

namespace
{
    using tkw::RngState;
    using tkw::SeededRng;
}

TEST_CASE("rng: save/load reproduces the exact sequence")
{
    SeededRng a(12345);
    for (int i = 0; i < 10; ++i)
        (void)a.next();

    const RngState state = a.save_state();
    std::vector<std::uint32_t> expected;
    for (int i = 0; i < 20; ++i)
        expected.push_back(a.next());

    SeededRng b(999);  // 不同种子，靠 load_state 覆盖
    REQUIRE(b.load_state(state));
    for (int i = 0; i < 20; ++i)
        CHECK(b.next() == expected[i]);
}

TEST_CASE("rng: malformed state is rejected without corrupting engine")
{
    SeededRng a(1);
    SeededRng b(1);
    const RngState bad{"not a valid engine state"};
    CHECK(!b.load_state(bad));
    CHECK(a.next() == b.next());  // b 未被破坏
}

TEST_CASE("rng: state round-trips through save")
{
    SeededRng a(7);
    (void)a.next();
    const RngState s1 = a.save_state();

    SeededRng b(0);
    REQUIRE(b.load_state(s1));
    CHECK(b.save_state() == s1);
}
