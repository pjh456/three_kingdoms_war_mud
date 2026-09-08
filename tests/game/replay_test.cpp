/**
 * @file replay_test.cpp
 * @brief 黄金回放回归网：同 seed 的完整对局事件日志必须逐行一致。
 * @note 这是后续重构（枚举拆分/管线/接口）的行为契约：日志变了 =
 *       规则语义漂移，必须先解释再改。
 */

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "event_log.hpp"
#include "game/ai/simple.hpp"
#include "game/flow/loop.hpp"
#include "test_game.hpp"

namespace
{
    using tkw::test::EventLog;
    using tkw::test::TestGame;

    /** 跑一局并返回完整事件日志（Ok 或 MaxRounds 都算完整对局）。 */
    std::vector<std::string> run_game(std::uint32_t seed, int players)
    {
        TestGame g("deck", seed);
        for (int i = 0; i < players; ++i)
            g.add_player("P" + std::to_string(i), i, 4);

        EventLog log(g.bus);
        tkw::game::SimpleAI ai;
        auto r = tkw::game::play_game(g.ctx, ai, "P0");
        if (r.is_err())
            REQUIRE(r.unwrap_err() == tkw::game::LoopError::MaxRounds);
        return log.lines();
    }

    /** 用 start/step_session 手动逐回合驱动，返回完整事件日志。 */
    std::vector<std::string> run_game_stepwise(std::uint32_t seed, int players)
    {
        TestGame g("deck", seed);
        for (int i = 0; i < players; ++i)
            g.add_player("P" + std::to_string(i), i, 4);

        EventLog log(g.bus);
        tkw::game::SimpleAI ai;
        tkw::game::GameSession session;
        REQUIRE(tkw::game::start_session(g.ctx, session, "P0").is_ok());
        while (!tkw::game::session_over(g.ctx))
        {
            auto r = tkw::game::step_session(g.ctx, ai, session);
            if (r.is_err())
            {
                REQUIRE(r.unwrap_err() == tkw::game::LoopError::MaxRounds);
                break;
            }
        }
        return log.lines();
    }

    /** FNV-1a 64 位指纹（对日志逐行逐字节）。 */
    std::uint64_t fingerprint(const std::vector<std::string> &lines)
    {
        std::uint64_t h = 1469598103934665603ULL;
        for (const auto &line : lines)
        {
            for (unsigned char c : line)
            {
                h ^= c;
                h *= 1099511628211ULL;
            }
            h ^= '\n';
            h *= 1099511628211ULL;
        }
        return h;
    }
}

TEST_CASE("replay: same seed yields identical event log")
{
    const auto a = run_game(1, 2);
    const auto b = run_game(1, 2);
    const auto c = run_game(2, 2);

    CHECK(!a.empty());
    CHECK(a == b);
    CHECK(a != c);
}

TEST_CASE("replay: 4-player draw is still deterministic")
{
    const auto a = run_game(42, 4);
    const auto b = run_game(42, 4);

    CHECK(!a.empty());
    CHECK(a == b);
}

TEST_CASE("replay: step_session drives the same log as play_game")
{
    CHECK(run_game_stepwise(1, 2) == run_game(1, 2));
    CHECK(run_game_stepwise(42, 4) == run_game(42, 4));
}

TEST_CASE("replay: golden fingerprints pin the rule semantics")
{
    const auto two = run_game(1, 2);
    CHECK(two.size() == 69);
    CHECK(fingerprint(two) == 10127578936120532578ULL);

    const auto four = run_game(42, 4);
    CHECK(four.size() == 361);
    CHECK(fingerprint(four) == 5757600367389828630ULL);
}
