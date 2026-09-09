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
    // 方天画戟多目标仅在杀为最后一张手牌时可触发，两个固定种子对局均未进入
    // 该状态，日志逐字节不变（逐行 diff 核对过）。
    //
    // 丈八蛇矛（两张手牌当杀）上线后的漂移（新旧日志逐行 diff 核对过）：
    // - 2 人 seed 1：逐字节不变——对局中无人进入「装备丈八 + 手牌无真杀 +
    //   手牌 ≥2 + 有攻击范围内目标」的虚拟杀状态。
    // - 4 人 seed 42：358 → 416 行。首个分叉在 P2 回合（刚装备丈八、1 体力、
    //   手牌 [无懈可击, 闪, 借刀杀人]、无真杀）：旧 AI 按手牌序先打借刀
    //   （从 P0 夺诸葛连弩）；新 AI 杀优先，两张手牌（无懈可击 + 闪）当杀
    //   打出，1 体力且无闪的 P0 阵亡，击杀奖励摸 3 张后局面级联改变
    //   （本回合杀次数已达上限，虚拟杀不再枚举，后续改打借刀/南蛮，
    //   P1 也阵亡）。虚拟杀本身：两张牌各发打出事件并进弃牌堆，无花色，
    //   仁王盾黑杀判定不适用。
    //
    // 丈八响应侧打出（杀响应窗口两张手牌当杀）上线后的漂移（新旧日志逐行
    // diff 核对过）：
    // - 2 人 seed 1：逐字节不变——对局中无人进入响应侧虚拟杀状态（装备丈八 +
    //   无真杀 + 手牌 ≥2 + 杀响应窗口）。
    // - 4 人 seed 42：416 → 415 行。首个分叉在 P2 的借刀（P0 刚阵亡、P2 刚装备
    //   丈八且仍持借刀）：P3（持武器者、B=A 自目标）有真杀，以真杀响应；自杀
    //   被自己的闪闪掉，不受伤。对目标结算的杀响应事件语法统一为打出（与虚拟
    //   杀一致），原先随出的额外一行弃置事件行移除，即减少的一行。局内未达响应
    //   侧 pair 态（无真杀 + 装备丈八 + 手牌 ≥2 + 杀响应窗口）。
    //
    // 弃牌接入牌价值升序排序后的漂移（新旧日志逐行 diff + 决策请求计数
    // 核对过）：
    // - 2 人 seed 1：75 → 79 行。首个分叉在 P0 第二轮弃牌阶段：手牌
    //   [无懈可击, 闪, 桃, 借刀, 杀] 超上限 1 张，旧 AI 弃手牌序首张
    //   无懈可击（55），新 AI 弃最低价值闪（35）。级联：旧线 P0 仍持闪、
    //   P1 出杀被闪掉；新线 P0 无闪吃下第一刀（4->3），P1 过河拆桥拆走
    //   桃改为拆走无懈可击，P0 低血时以桃自救（多出 play/healed 行）。
    // - 4 人 seed 42：逐字节不变——整局 choose_discards 零调用（每回合出
    //   牌阶段均把手牌清到上限内；贯石斧/雌雄双股剑的代价弃牌未发生；
    //   日志中的目标弃牌行均为过河/顺拆/寒冰剑经选牌路径产生，不走
    //   弃牌排序）。
    const auto two = run_game(1, 2);
    CHECK(two.size() == 79);
    CHECK(fingerprint(two) == 12977149775915994001ULL);

    const auto four = run_game(42, 4);
    CHECK(four.size() == 415);
    CHECK(fingerprint(four) == 14527444784400290652ULL);
}
