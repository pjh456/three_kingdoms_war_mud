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
#include "game/ai/aggressive.hpp"
#include "game/ai/simple.hpp"
#include "game/flow/loop.hpp"
#include "test_game.hpp"

namespace
{
    using tkw::test::EventLog;
    using tkw::test::TestGame;

    // 攻击优先档自钉值（2p seed 1 / 4p seed 42，实跑钉入，见对应用例注释）
    constexpr std::size_t AGGRESSIVE_2P_LINES = 227;
    constexpr std::uint64_t AGGRESSIVE_2P_FP = 5211696238794449955ULL;
    constexpr std::size_t AGGRESSIVE_4P_LINES = 336;
    constexpr std::uint64_t AGGRESSIVE_4P_FP = 13493724924669352056ULL;

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

    /** 用指定决策源跑一局，返回完整事件日志（Ok 或 MaxRounds 都算完整对局）。 */
    std::vector<std::string> run_game_ai(
        tkw::game::DecisionSource &ai, std::uint32_t seed, int players)
    {
        TestGame g("deck", seed);
        for (int i = 0; i < players; ++i)
            g.add_player("P" + std::to_string(i), i, 4);

        EventLog log(g.bus);
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
    //
    // 无懈三段判定（自己的锦囊不抵消 / 锦囊冲我则出 / 其余不出）生效后的
    // 漂移（新旧日志逐行 diff + 无懈窗口询问轨迹核对过）：
    // - 2 人 seed 1：逐字节不变——唯一触发态是 P1 过河拆桥冲 P0 的窗口：
    //   新线 P0 打出无懈抵消，旧线 P0 的无懈被过河拆桥拆走；两线都恰是一行
    //   `discard P0 wuxie` 事件，P0 剩余手牌相同，未起级联。
    // - 4 人 seed 42：415 → 311 行，结局由 P3 胜翻为 MaxRounds 平局。首个
    //   分叉在 P0 首回合五谷丰登：P1、P2 各出 1 张无懈（多出的 2 行；偶数
    //   相抵，效果仍生效）。旧线 P1 的无懈留着抵消其后 P0 的过河拆桥（旧 30
    //   行无懈行）、P2 的无懈被丈八两张当杀消费（旧 248 行 play 行）；新线
    //   两牌均已在五谷窗口消耗，过河拆桥生效拆走 P1 的杀，手牌/装备/死亡
    //   级联：P0、P1 提前阵亡（旧 256/334 行 → 新 207/232 行），P2 不再
    //   阵亡。P2/P3 双存活、摸牌堆抽空后静默停摸、剩余手牌均不可主动打出，
    //   后续回合零事件行，主循环落到回合上限（1000）以 MaxRounds 平局结束。
    //
    // 摸牌堆耗尽洗回弃牌堆口径统一后的漂移（新旧日志逐行 diff 核对过）：
    // - 2 人 seed 1：逐字节不变——整局摸牌堆从未抽空，不触发洗回。
    // - 4 人 seed 42：311 → 508 行，结局由 MaxRounds 平局翻为 P2 胜（40
    //   回合）。首个分叉在 P3 摸牌阶段：旧线摸牌堆只剩 1 张（无懈可击），
    //   第二张静默摸空；新线摸牌堆空时将弃牌堆洗回，第二张摸到顺手牵羊。
    //   此后 P3/P2 持续摸牌出牌，P3 阵亡、P2 成为唯一存活者；洗回消费 rng
    //   使后续摸牌/判定序列整体重排，日志行数与指纹随之改变。
    //
    // 无懈窗口按目标绑定（多目标效果逐目标单元素窗口）后的核对（新旧日志逐行
    // diff 核对过）：
    // - 2 人 seed 1：逐字节不变——2 人局 AllOthers 的南蛮/万箭只有一个目标，
    //   窗内集合与修复前相等；桃园虽多目标，但未出现非使用者目标持无懈且在
    //   他人窗口被问的触发态。
    // - 4 人 seed 42：逐字节不变——两个固定种子对局均未进入「多目标锦囊结算
    //   时非当前目标的玩家持无懈」的状态；已发生的无懈窗口均为单窗口效果
    //   （过河拆桥/五谷丰登）或目标集合仅含当事人，新旧携带集合一致，故不重钉。
    //
    // 五谷亮牌洗回口径统一后的核对（新旧日志逐行 diff 核对过）：两组固定种子
    // 对局均未在五谷亮牌途中遇摸牌堆空，洗回分支未触发，故行数与指纹不变。
    //
    // 闪电移送跳过判定区同名者后的核对（新旧日志逐行 diff 核对过）：两组固定
    // 种子对局均未进入「闪电判定失败且下家判定区已有闪电」的触发态，移送目标
    // 与修复前相同，故行数与指纹不变。
    //
    // 借刀杀人禁止 B==A 后的核对（新旧日志逐行 diff 核对过）：2 人 seed 1 与
    // 4 人 seed 42 的 simple 线均逐字节不变——simple 线的借刀落子处，被指定
    // 的受害者均未打出杀，取武器结果与旧的自目标选择同构（目标身份不进事件
    // 日志，仅响应结果决定事件），故不重钉。4 人 seed 42 的 aggressive 线确有
    // 漂移，口径见 aggressive 4 人用例。
    const auto two = run_game(1, 2);
    CHECK(two.size() == 79);
    CHECK(fingerprint(two) == 12977149775915994001ULL);

    const auto four = run_game(42, 4);
    CHECK(four.size() == 508);
    CHECK(fingerprint(four) == 14208062105892487506ULL);
}

TEST_CASE("replay: four-player seed 42 reaches a decisive result")
{
    // 旗舰默认对局（裸 tkw = deal 4 42）必须分出唯一胜者，而不是摸空僵持到
    // 回合上限：摸牌洗回口径统一后，牌堆耗尽会从弃牌堆补牌，对局在 max_turns
    // 之前结束。此用例直接钉住该结果，防止退回 1001 回合平局。
    TestGame g("deck", 42);
    for (int i = 0; i < 4; ++i)
        g.add_player("P" + std::to_string(i), i, 4);

    tkw::game::SimpleAI ai;
    const auto r = tkw::game::play_game(g.ctx, ai, "P0");
    REQUIRE(r.is_ok());
    const auto outcome = r.unwrap();
    CHECK(outcome.winner == "P2");
    CHECK(!outcome.winner.empty());
    CHECK(outcome.turns < 1000);
    CHECK(g.ctx.entities->find(outcome.winner).is_some());
}

TEST_CASE("replay: two-player games stay consistent across seeds")
{
    // 黄金指纹只钉两个固定种子；这里多种子扫描补轻量不变量，不重钉任何指纹。
    // 种子区间覆盖自然分胜负与到达回合上限的两种结束形态。
    for (std::uint32_t seed = 1; seed <= 40; ++seed)
    {
        TestGame g("deck", seed);
        g.add_player("P0", 0, 4);
        g.add_player("P1", 1, 4);

        EventLog log(g.bus);
        tkw::game::SimpleAI ai;
        const auto r = tkw::game::play_game(g.ctx, ai, "P0");

        // 事件流非空：每局至少含开局发牌
        CHECK(!log.lines().empty());

        // 存活实体体力为正：死亡实体已从容器移除，不留非法血条
        for (const auto &e : *g.ctx.entities)
            CHECK(e->get_hp() > 0);

        if (r.is_ok())
        {
            CHECK(tkw::game::session_over(g.ctx));
            const auto winner = tkw::game::session_winner(g.ctx);
            const bool winner_alive =
                winner.empty() || g.ctx.entities->find(winner).is_some();
            CHECK(winner_alive);
        }
        else
        {
            CHECK(r.unwrap_err() == tkw::game::LoopError::MaxRounds);
        }

        // 同种子可重放：第二局逐行一致（扫描不放松确定性契约）
        TestGame again("deck", seed);
        again.add_player("P0", 0, 4);
        again.add_player("P1", 1, 4);
        EventLog log2(again.bus);
        tkw::game::SimpleAI ai2;
        (void)tkw::game::play_game(again.ctx, ai2, "P0");
        CHECK(log.lines() == log2.lines());
    }
}

TEST_CASE("replay: aggressive ai is deterministic and pins its golden fingerprint")
{
    // 攻击优先档自钉：同 seed 两遍逐行一致 + 非空，行数与 FNV-1a 64 指纹
    // 钉死当前行为（AI 调参漂移时此处变红，先解释再改）。首分叉（对 simple
    // 线）在 P0 首回合出牌阶段：simple 按手牌序打五谷丰登，aggressive
    // 按卡类优先级先打杀，后续顺手牵羊抢高价值牌并立刻打出万箭，事件流
    // 自第 10 行起分岔（227 行 vs 79 行）。
    tkw::game::AggressiveAI aggr;
    const auto a = run_game_ai(aggr, 1, 2);
    const auto b = run_game_ai(aggr, 1, 2);

    CHECK(!a.empty());
    CHECK(a == b);
    CHECK(a.size() == AGGRESSIVE_2P_LINES);
    CHECK(fingerprint(a) == AGGRESSIVE_2P_FP);
}

TEST_CASE("replay: aggressive 4-player seed 42 is deterministic and pinned")
{
    // 双种子模式镜像贪心档：4 人 seed 42 攻击优先档自钉。摸牌洗回口径统一后
    // 该局由 MaxRounds 平局翻为分胜负（行数 302 → 677）。
    //
    // 借刀杀人禁止 B==A 后的漂移（新旧日志逐行 diff 核对过）：677 → 336 行。
    // 首个分叉在老第 256 行：P3 打借刀后的取武器结果由「取 P0 的诸葛连弩」
    // 变为「取 P2 的寒冰剑」（move P0:equip->P3:hand liangnu → move
    // P2:equip->P3:hand hanbing）。旧枚举含 {A=P0, B=A 自身} 且 AI 优先取
    // 该自目标对；新枚举禁止 B==A，AI 在合法对中改选集火对象体力最低者——
    // 此刻 P0 刚被救回仅 1 体力，故选定 {A=P2, B=P0}，P2 无杀响应，P3 得
    // P2 的寒冰剑。所得武器不同导致后续出牌与死亡顺序级联，行数与指纹整体
    // 改变。2 人 seed 1 的 simple/aggressive 两线均未进入借刀自目标态，逐字节
    // 不变；4 人 seed 42 simple 线的借刀落子被指定者均未响应，事件流同构，亦
    // 逐字节不变（故黄金指纹用例不重钉）。
    tkw::game::AggressiveAI aggr;
    const auto a = run_game_ai(aggr, 42, 4);
    const auto b = run_game_ai(aggr, 42, 4);

    CHECK(!a.empty());
    CHECK(a == b);
    CHECK(a.size() == AGGRESSIVE_4P_LINES);
    CHECK(fingerprint(a) == AGGRESSIVE_4P_FP);
}

TEST_CASE("replay: aggressive ai diverges from the simple tier")
{
    // 差分守卫：两档决策源在同一 seed 下必须给出不同对局，防攻击优先档
    // 意外退化成改名贪心档（静默 bug）。2 人 seed 1 两线日志逐行比对，
    // 首分叉在 P0 首回合出牌（simple 打五谷 / aggressive 打杀）。
    tkw::game::AggressiveAI aggr;
    const auto aggressive = run_game_ai(aggr, 1, 2);
    const auto simple = run_game(1, 2);

    CHECK(aggressive != simple);
}

TEST_CASE("replay: aggressive two-player scan stays consistent")
{
    // 多 seed 扫描补轻量不变量（口径同贪心档扫描）：日志非空、存活者体力
    // 为正、结束形态合法、同 seed 可重放。不重钉任何指纹。
    for (std::uint32_t seed = 1; seed <= 20; ++seed)
    {
        TestGame g("deck", seed);
        g.add_player("P0", 0, 4);
        g.add_player("P1", 1, 4);

        EventLog log(g.bus);
        tkw::game::AggressiveAI ai;
        const auto r = tkw::game::play_game(g.ctx, ai, "P0");

        CHECK(!log.lines().empty());

        for (const auto &e : *g.ctx.entities)
            CHECK(e->get_hp() > 0);

        if (r.is_ok())
        {
            CHECK(tkw::game::session_over(g.ctx));
            const auto winner = tkw::game::session_winner(g.ctx);
            const bool winner_alive =
                winner.empty() || g.ctx.entities->find(winner).is_some();
            CHECK(winner_alive);
        }
        else
        {
            CHECK(r.unwrap_err() == tkw::game::LoopError::MaxRounds);
        }

        // 同种子可重放：第二局逐行一致
        TestGame again("deck", seed);
        again.add_player("P0", 0, 4);
        again.add_player("P1", 1, 4);
        EventLog log2(again.bus);
        tkw::game::AggressiveAI ai2;
        (void)tkw::game::play_game(again.ctx, ai2, "P0");
        CHECK(log.lines() == log2.lines());
    }
}

