/**
 * @file replay_test.cpp
 * @brief 黄金回放回归网：同 seed 的完整对局事件日志必须逐行一致。
 * @note 这是后续重构（枚举拆分/管线/接口）的行为契约：日志变了 =
 *       规则语义漂移，必须先解释再改。
 */

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "event_log.hpp"
#include "game/ai/aggressive.hpp"
#include "game/ai/simple.hpp"
#include "game/core/card_event.hpp"
#include "game/core/roles.hpp"
#include "game/flow/factory.hpp"
#include "game/flow/loop.hpp"
#include "test_game.hpp"

namespace
{
    using tkw::test::EventLog;
    using tkw::test::TestGame;

    // 攻击优先档自钉值（2p seed 1 / 4p seed 42，实跑钉入，见对应用例注释）
    constexpr std::size_t AGGRESSIVE_2P_LINES = 73;
    constexpr std::uint64_t AGGRESSIVE_2P_FP = 4647716859109022064ULL;
    constexpr std::size_t AGGRESSIVE_4P_LINES = 413;
    constexpr std::uint64_t AGGRESSIVE_4P_FP = 16001951569717656035ULL;

    // 身份局自钉值（4p/5p simple、4p aggressive，实跑钉入，见对应用例注释）
    constexpr std::size_t IDENTITY_4P_LORD_LINES = 184;
    constexpr std::uint64_t IDENTITY_4P_LORD_FP = 17659891135820584377ULL;
    constexpr std::size_t IDENTITY_4P_TRAITOR_LINES = 453;
    constexpr std::uint64_t IDENTITY_4P_TRAITOR_FP = 6165952664505355774ULL;
    constexpr std::size_t IDENTITY_5P_REBEL_LINES = 268;
    constexpr std::uint64_t IDENTITY_5P_REBEL_FP = 6096591313624321891ULL;
    constexpr std::size_t IDENTITY_4P_AGGRESSIVE_LINES = 169;
    constexpr std::uint64_t IDENTITY_4P_AGGRESSIVE_FP = 12315602747341503842ULL;

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

    /** 身份局整局结果：事件日志 + 终局 + 角色表 + 身份专属路径计数。 */
    struct IdentityRun
    {
        std::vector<std::string> lines; /**< 完整事件日志（发布序） */
        tkw::game::GameOutcome outcome; /**< 终局结果；ok=false 时 camp 为 None */
        tkw::game::RoleTable roles;     /**< 本局角色表（id 升序） */
        int kill_rewards = 0;           /**< DrawKind::KillReward 摸牌事件数 */
        int deaths = 0;                 /**< EntityDiedEvent 数 */
        bool ok = false;                /**< 是否在回合上限前终局 */
        tkw::game::LoopError err = tkw::game::LoopError::MaxRounds; /**< !ok 时的流程错误 */
    };

    /**
     * @brief 经 build_game（真实角色 Fisher-Yates 分配）开身份局并跑到底。
     * @param ai 决策源（引用须存活至调用返回）；seed 随机种子；players 玩家人数。
     * @return 事件日志、终局、角色表与击杀奖励/阵亡计数；回合上限平局时 ok=false。
     * @note 角色分配在 start_session 洗牌之前消费随机流，故同 seed 的身份局牌序
     *       与乱斗局不同；本 harness 不复用 TestGame 手工建局，以覆盖该路径。
     *       额外订阅不停止事件传播，不影响 EventLog 的行序。
     */
    IdentityRun run_identity(
        tkw::game::DecisionSource &ai, std::uint32_t seed, int players)
    {
        tkw::game::BuildOptions opt;
        opt.deck = TKW_TEST_RESOURCE_DIR;
        opt.players = players;
        opt.seed = seed;
        opt.mode = tkw::game::GameMode::Identity;
        auto built = tkw::game::build_game(opt);
        REQUIRE(built.is_ok());
        auto game = std::move(built).unwrap();

        EventLog log(game->bus);
        IdentityRun run;
        auto drawn = game->bus.subscribe(tkw::Handler<tkw::CardDrawnEvent>(
            [&](tkw::HandlerContext<tkw::CardDrawnEvent> &c)
            {
                if (c.event.kind == tkw::DrawKind::KillReward)
                    ++run.kill_rewards;
            }));
        auto died = game->bus.subscribe(tkw::Handler<tkw::EntityDiedEvent>(
            [&](tkw::HandlerContext<tkw::EntityDiedEvent> &) { ++run.deaths; }));

        auto ctx = game->context();
        auto r = tkw::game::play_game(ctx, ai, "P0");
        run.lines = log.lines();
        run.roles = game->roles;
        if (r.is_ok())
        {
            run.ok = true;
            run.outcome = r.unwrap();
        }
        else
        {
            run.err = r.unwrap_err();
        }
        return run;
    }

    /** 校验身份局角色表：P0=主公、各角色计数符合人数配比、覆盖全部座位。 */
    void check_identity_roles(const tkw::game::RoleTable &roles, int players)
    {
        const auto counts = tkw::game::roles_for_count(players);
        REQUIRE(counts.is_some());
        int lord = 0;
        int loyalist = 0;
        int rebel = 0;
        int traitor = 0;
        for (const auto &entry : roles)
        {
            switch (entry.second)
            {
            case tkw::game::Role::Lord: ++lord; break;
            case tkw::game::Role::Loyalist: ++loyalist; break;
            case tkw::game::Role::Rebel: ++rebel; break;
            case tkw::game::Role::Traitor: ++traitor; break;
            case tkw::game::Role::None: break;
            }
        }
        CHECK(lord == 1);
        CHECK(loyalist == counts.unwrap().loyalist);
        CHECK(rebel == counts.unwrap().rebel);
        CHECK(traitor == counts.unwrap().traitor);
        CHECK(tkw::game::role_of(&roles, "P0") == tkw::game::Role::Lord);
        CHECK(roles.size() == static_cast<std::size_t>(players));
    }

    /** 校验身份局终局：阵营非 None 且胜者角色与阵营一致。 */
    void check_identity_outcome(const IdentityRun &run)
    {
        CHECK(run.outcome.camp != tkw::game::WinCamp::None);
        const auto winner_role = tkw::game::role_of(&run.roles, run.outcome.winner);
        switch (run.outcome.camp)
        {
        case tkw::game::WinCamp::LordCamp:
            CHECK(winner_role == tkw::game::Role::Lord);
            CHECK(run.outcome.winner == "P0");
            break;
        case tkw::game::WinCamp::RebelCamp:
            CHECK(winner_role == tkw::game::Role::Rebel);
            break;
        case tkw::game::WinCamp::TraitorCamp:
            CHECK(winner_role == tkw::game::Role::Traitor);
            break;
        case tkw::game::WinCamp::Draw:
            CHECK(run.outcome.winner.empty());
            break;
        case tkw::game::WinCamp::None:
            break;
        }
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
    //
    // 弃牌手牌上限由「体力上限」改「当前体力值」后的漂移（新旧日志逐行 diff
    // 核对过）：四人/两线首分叉均落在某名已受伤角色的弃牌阶段，因上限等于其
    // 当前体力（低于上限）而多弃若干张，此后手牌/响应/装备/死亡序列级联改变。
    // - 2 人 seed 1：79 → 69 行。首个分叉在 P1 弃牌阶段：P1 被万箭打到 4→3，
    //   旧上限 4 只弃赤兔，新上限 3 再弃连弩；P1 失去连弩后无限出杀链消失，
    //   后续伤害/摸牌级联，行数减少。
    // - 4 人 seed 42：508 → 404 行。首个分叉在 P1 弃牌阶段：P1 当前体力 1
    //   （先前 2→1），旧上限 4 不弃牌，新上限 1 弃闪；P1 失去闪后其后的杀
    //   命中而非被闪掉，濒死救援与死亡序列随之改变。
    // - aggressive 2 人 seed 1：227 → 229 行。首个分叉在 P1 弃牌阶段：P1 被
    //   万箭打到 4→3，旧上限 4 只弃赤兔，新上限 3 再弃青釭剑；P1 无法再装
    //   青釭剑，后续装备/伤害链改变。
    // - aggressive 4 人 seed 42：336 → 391 行。首个分叉在 P1 弃牌阶段：P1
    //   当前体力 1（被 P0 的杀 2→1），旧上限 4 不弃牌，新上限 1 弃八卦阵；
    //   后续防御/死亡顺序级联，行数增加。
    //
    // 万箭齐发接入八卦阵闪响应（「需闪」共用入口）后的核对（新旧日志逐行
    // diff 核对过）：四条黄金线均未进入「万箭结算时目标持有八卦阵」的触发态，
    // 故行数与指纹不变、不重钉。四方对局中唯一打出万箭的时点（simple 2 人
    // 第 22 行 / simple 4 人第 14 行 / aggressive 两线第 189 行）之前，目标
    // 均未装备八卦阵：simple 两线的对手无八卦阵；aggressive 两线的 P1 抽到
    // 八卦阵后当回合即弃置、从未装备。装备八卦阵的一方均为万箭使用者 P0，
    // 而 AllOthers 目标不含使用者；P0 装备八卦阵也都晚于其打出万箭。
    //
    // AOE 从使用者下家起按座位序结算后的核对（新旧日志逐行 diff 核对过）：
    // - 2 人 seed 1：逐字节不变——唯一的万箭目标为 P1，单目标无重排。
    // - 4 人 seed 42：404 行不变、指纹更新。首个分叉在第 36 行：P1 南蛮的
    //   响应顺序由 P0 → P2 → P3（创建/座位升序）变为 P2 → P3 → P0（使用者
    //   P1 的下家 P2 起按座位环绕）。三方均以杀响应免伤、无状态级联，故仅
    //   弃置行顺序变化，行数不变。
    //
    // 目标手牌改「引擎随机暗抽 + Aggressive 去全知」后的漂移（新旧日志逐行
    // diff 核对过）：顺手牵羊/过河拆桥/寒冰剑取对手手牌不再携带真实身份，由引擎
    // resolve_target_pick 经 rng 均匀暗抽（手牌仅 1 张时 uniform_below 不消费
    // 随机流）；Aggressive 对手牌占位槽只按期望常量估值、不再按真实牌价值点名。
    // - 2 人 seed 1：逐字节不变、指纹不重钉。首个隐藏暗抽在 P0 顺手牵羊取 P1
    //   手牌（5 张），rng 抽中槽位 0（万箭齐发），与旧的 hand.front() 同牌；
    //   本局另一处隐藏暗抽（P1 过河拆桥取 P0）亦抽中槽位 0，此后无其它 rng
    //   消费点，故日志逐行一致。
    // - 4 人 seed 42：404 → 549 行。首个分叉在 P0 过河拆桥取 P1 手牌：旧线取
    //   首张杀，新线暗抽命中顺手牵羊；被拆牌不同导致 P1 出牌/弃牌/装备/死亡
    //   序列整体级联。
    // - Aggressive 2 人 seed 1：229 → 73 行；4 人 seed 42：391 → 378 行。两条
    //   线首个分叉均落在首个隐藏暗抽（P0 顺手牵羊取对手手牌）：旧档按真实牌
    //   价值点名最优牌，新档只能对无身份占位槽按期望常量估值并接受引擎暗抽，
    //   起手被抢的牌不再最优，后续死亡/装备级联改变。这是有意消除全知信息优势
    //   的规则保真取舍（强度下调）。
    //
    // 濒死询问起点由「濒死者起」改为「当前回合角色起」（无回合上下文回落濒死者）
    // 后的核对（新旧日志逐行 diff + 每处濒死救援的回合角色/濒死者/桃持有者三元组
    // 核对过）：两条 simple 线均逐字节不变，未进入「多个供桃者且顺序影响结果」的
    // 触发态，故不重钉。
    // - 2 人 seed 1：唯一濒死（P1 被击杀至 -2）全环无人持桃，救援顺序不影响事件。
    // - 4 人 seed 42：4 处濒死中 1 处为闪电自伤（濒死者 == 当前回合角色 P0），
    //   其余 3 处（回合 7 P2 桃救 P1、回合 26/28 P3 桃救 P0）起问起点替换后首个
    //   实际出桃者不变，手牌与后续级联不变。
    //
    // 五谷丰登无懈窗口由「整张全量单窗」改为「逐目标单元素窗」后的漂移
    // （新旧日志逐行 diff + 使用者/五谷选牌/无懈窗三元组核对过）：
    // - 2 人 seed 1：逐字节不变——五谷两个目标窗内均未出现「非当前目标的
    //   持无懈者被询问」的触发态，无懈事件与选牌序列与旧线一致。
    // - 4 人 seed 42：549 → 361 行，指纹更新。首个分叉在原第 26 行：P0 首回合
    //   五谷，旧线 P1、P2 各出一张无懈于同一全量窗（偶数相抵，效果仍生效），
    //   四人各选一张；新线逐目标开窗，P1、P2 的无懈各自抵消自己那一窗，二人
    //   不选牌，亮 4 张只被 P0、P3 选走 2 张，余 2 张空 owner 弃置（多出 2 条
    //   discard 行）。P1、P2 少得五谷牌，此后出牌/弃牌/死亡序列整体级联，
    //   行数与指纹随之改变。
    const auto two = run_game(1, 2);
    CHECK(two.size() == 69);
    CHECK(fingerprint(two) == 9283070076194552029ULL);

    const auto four = run_game(42, 4);
    CHECK(four.size() == 361);
    CHECK(fingerprint(four) == 12592335471416647501ULL);
}

TEST_CASE("replay: four-player seed 42 reaches a decisive result")
{
    // 旗舰默认对局（裸 tkw = deal 4 42）必须分出唯一胜者，而不是摸空僵持到
    // 回合上限：摸牌洗回口径统一后，牌堆耗尽会从弃牌堆补牌，对局在 max_turns
    // 之前结束。此用例直接钉住该结果，防止退回 1001 回合平局。
    // 濒死询问起点对齐当前回合角色后该结论不变（日志逐行未漂移，winner 仍 P2）。
    // 五谷丰登无懈窗口改逐目标后 winner 由 P2 翻为 P3：P1、P2 各失一张五谷牌，
    // 后续伤害/死亡顺序级联改变（漂移归因见黄金指纹用例），仍分胜负且在回合
    // 上限前结束。
    TestGame g("deck", 42);
    for (int i = 0; i < 4; ++i)
        g.add_player("P" + std::to_string(i), i, 4);

    tkw::game::SimpleAI ai;
    const auto r = tkw::game::play_game(g.ctx, ai, "P0");
    REQUIRE(r.is_ok());
    const auto outcome = r.unwrap();
    CHECK(outcome.winner == "P3");
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
    // 自第 10 行起分岔（229 行 vs 69 行）。
    //
    // 弃牌手牌上限改「当前体力值」后的漂移（新旧日志逐行 diff 核对过）：
    // 227 → 229 行。首个分叉在 P1 弃牌阶段：P1 被万箭打到 4→3，旧上限 4
    // 只弃赤兔，新上限 3 再弃青釭剑；P1 失去青釭剑后无法再装备，后续装备/
    // 伤害链级联，行数增加 2。
    //
    // AOE 从使用者下家起按座位序结算后的核对（新旧日志逐行 diff 核对过）：
    // 229 行不变、指纹更新。首个分叉在 P0 的桃园：scope=All 含使用者，使用者
    // P0 的下家 P1 先回血、P0 排最后；旧按创建/座位升序为 P0 → P1。两人各回
    // 1 点、无状态级联，仅回血事件顺序变化。
    //
    // 目标手牌改引擎随机暗抽 + 去全知后的漂移（新旧日志逐行 diff 核对过）：
    // 229 → 73 行。首个分叉在 P0 顺手牵羊取 P1 手牌（首个隐藏暗抽）：旧档按
    // 真实牌价值取走万箭齐发，新档对无身份占位槽只按期望常量估值并接受引擎
    // 暗抽，取到杀，后续出牌/伤害/死亡序列级联改变。规则保真取舍：Aggressive
    // 不再能精确抢走对手高价值手牌。
    //
    // 濒死询问起点改为「当前回合角色起」后的核对（新旧日志逐行 diff 核对过）：
    // 逐字节不变——唯一濒死（P1 被击杀至 -1 死亡）全环无人持桃，救援顺序不影响
    // 事件，未进入顺序差异触发态。
    //
    // 五谷丰登无懈窗口改逐目标后的核对（新旧日志逐行 diff 核对过）：逐字节
    // 不变——本线首回合打杀而非五谷，整局未进入「五谷结算且目标持无懈」的
    // 触发态，故不重钉。
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
    //
    // 弃牌手牌上限改「当前体力值」后的漂移（新旧日志逐行 diff 核对过）：
    // 336 → 391 行。首个分叉在 P1 弃牌阶段：P1 当前体力 1（被 P0 的杀 2→1），
    // 旧上限 4 不弃牌，新上限 1 弃八卦阵；此后防御/濒死/死亡顺序级联，行数
    // 增加 55。
    //
    // 目标手牌改引擎随机暗抽 + 去全知后的漂移（新旧日志逐行 diff 核对过）：
    // 391 → 378 行。首个分叉在 P0 顺手牵羊取 P1 手牌（首个隐藏暗抽）：旧档
    // 按真实牌价值取走南蛮入侵，新档暗抽取到过河拆桥，后续出牌/装备/死亡级联
    // 改变。
    //
    // 濒死询问起点改为「当前回合角色起」后的核对（新旧日志逐行 diff 核对过）：
    // 逐字节不变——5 处濒死中 1 处为闪电自伤（濒死者 == 当前回合角色 P0），其余
    // 4 处（回合 15/17 P2 桃救 P1、回合 35/37 P2 桃救 P3）起问起点替换后首个实际
    // 出桃者不变，未进入顺序差异触发态。
    //
    // 五谷丰登无懈窗口改逐目标后的漂移（新旧日志逐行 diff 核对过）：378 → 413
    // 行。首个分叉在原第 26 行：P0 首回合五谷，旧线 P1、P2 各出一张无懈于同一
    // 全量窗（偶数相抵、效果仍生效），四人各选一张；新线逐目标各窗抵消自己，
    // P1、P2 不选牌，亮 4 张只被 P0、P3 选走 2 张，余 2 张空 owner 弃置。P0 的
    // 顺手牵羊由此改夺 P1 留下的南蛮入侵（旧线 P1 多得的过河拆桥被夺），出牌/
    // 响应/死亡序列级联，行数增加、指纹更新。
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

TEST_CASE("replay: golden identity four-player lord fingerprint")
{
    // 身份局整局回放护栏：角色分配（build_game 的 Fisher-Yates 消费随机流）→
    // 阵营目标 → 击杀奖励 → 终局口径端到端。事件日志不含 camp/winner/角色表，
    // 故除指纹外必须显式断言终局阵营与胜者角色；角色分配消费的随机流使同 seed
    // 的身份局牌序与乱斗局不同，指纹天然独立。
    //
    // 隐藏角色可见性不在本 harness 覆盖范围：回放日志无可见性过滤，角色不进
    // 事件日志；真人视角的隐藏语义由 TUI/CLI 用例覆盖。
    //
    // seed 4：P0=主公、P1=内奸、P2=忠臣、P3=反贼；主公阵营胜，16 回合，
    // 2 人阵亡、1 次击杀反贼的奖励摸牌（证明身份击杀奖励分支执行）。
    //
    // 身份局 AI 阵营意识上线后的漂移（新旧日志逐行 diff 核对过）：189 → 184 行。
    // 首个分叉在 P1 回合：P1 的「杀」目标由 P0（主公）改为 P2（忠臣）——内奸在
    // 有反贼存活时避让主公，改打非主公目标。此后手牌/伤害/死亡序列级联，行数
    // 减少；终局仍主公阵营胜。
    //
    // 敌方自益锦囊无懈（窗口奇偶）上线后的核对（新旧日志逐行 diff 核对过）：
    // 四条身份局黄金线（seed 4、seed 5、5 人 seed 2 simple 与 seed 2 aggressive）
    // 均未进入「敌方打出无中生有且其敌人持有无懈」的触发态，日志逐字节不变，
    // 故身份局四个指纹与乱斗四常量均不重钉；窗口奇偶只作用于自益窗，既有有害
    // 窗的逐轮重问残差保持不变。
    tkw::game::SimpleAI ai;
    const auto a = run_identity(ai, 4, 4);
    const auto b = run_identity(ai, 4, 4);
    REQUIRE(a.ok);
    REQUIRE(b.ok);

    CHECK(a.lines == b.lines);
    CHECK(!a.lines.empty());
    CHECK(a.lines.size() == IDENTITY_4P_LORD_LINES);
    CHECK(fingerprint(a.lines) == IDENTITY_4P_LORD_FP);

    check_identity_roles(a.roles, 4);
    check_identity_outcome(a);
    CHECK(a.outcome.camp == tkw::game::WinCamp::LordCamp);
    CHECK(a.kill_rewards >= 1);
    CHECK(a.deaths >= 1);
}

TEST_CASE("replay: golden identity four-player traitor fingerprint")
{
    // seed 5：P0=主公、P1=反贼、P2=忠臣、P3=内奸；内奸阵营胜（主公阵亡后
    // 内奸唯一存活），39 回合，3 人阵亡、1 次击杀反贼奖励摸牌。该局覆盖
    // TraitorCamp 终局口径。
    //
    // 身份局 AI 阵营意识上线后的漂移（新旧日志逐行 diff 核对过）：328 → 453 行。
    // 首个分叉在 P3 的乐不思蜀冲 P2 的无懈窗口：P0（主公）作为窗内首位保护者
    // 打出无懈保护 P2（忠臣）；旧口径只在锦囊冲自己时出无懈，故此前无事件。
    // P0 持两张无懈，引擎逐轮重问首位保护者时再次打出（偶数相抵，乐仍生效、
    // 两张无懈均消耗；单点轮询无法感知链状态，属既有近似）。此后手牌/装备/
    // 死亡序列整体级联，行数增加；终局仍内奸阵营胜。
    //
    // 敌方自益锦囊无懈（窗口奇偶）上线后的核对：本条的偶数相抵落在有害延时
    // 锦囊判定窗，窗口奇偶仅作用于自益窗，故该窗行为不变、本指纹不重钉。
    tkw::game::SimpleAI ai;
    const auto a = run_identity(ai, 5, 4);
    const auto b = run_identity(ai, 5, 4);
    REQUIRE(a.ok);
    REQUIRE(b.ok);

    CHECK(a.lines == b.lines);
    CHECK(!a.lines.empty());
    CHECK(a.lines.size() == IDENTITY_4P_TRAITOR_LINES);
    CHECK(fingerprint(a.lines) == IDENTITY_4P_TRAITOR_FP);

    check_identity_roles(a.roles, 4);
    check_identity_outcome(a);
    CHECK(a.outcome.camp == tkw::game::WinCamp::TraitorCamp);
    CHECK(a.kill_rewards >= 1);
    CHECK(a.deaths >= 1);
}

TEST_CASE("replay: golden identity five-player rebel fingerprint")
{
    // seed 2、5 人配比（1 忠臣 / 2 反贼 / 1 内奸）：反贼阵营胜，19 回合，
    // 2 人阵亡、1 次击杀反贼奖励摸牌。覆盖 RebelCamp 终局口径与 5 人角色
    // 配比，胜者为角色表首个反贼。
    //
    // 身份局 AI 阵营意识上线后的漂移（新旧日志逐行 diff 核对过）：204 → 268 行。
    // seed 2 角色为 P0=主公、P1=反贼、P2=反贼、P3=内奸、P4=忠臣。首个分叉在
    // P2（反贼）被闪电劈至濒死：旧口径在场的 P3（内奸）与 P4（忠臣）各出桃
    // 相救；新口径按阵营取舍——内奸只救自己与（有反贼存活时的）主公，忠臣只救
    // 主公/忠臣，二人均不救反贼，P2 阵亡。此后手牌/装备/终局序列级联，行数
    // 增加；终局仍反贼阵营胜。
    tkw::game::SimpleAI ai;
    const auto a = run_identity(ai, 2, 5);
    const auto b = run_identity(ai, 2, 5);
    REQUIRE(a.ok);
    REQUIRE(b.ok);

    CHECK(a.lines == b.lines);
    CHECK(!a.lines.empty());
    CHECK(a.lines.size() == IDENTITY_5P_REBEL_LINES);
    CHECK(fingerprint(a.lines) == IDENTITY_5P_REBEL_FP);

    check_identity_roles(a.roles, 5);
    check_identity_outcome(a);
    CHECK(a.outcome.camp == tkw::game::WinCamp::RebelCamp);
    CHECK(a.kill_rewards >= 1);
    CHECK(a.deaths >= 1);
}

TEST_CASE("replay: golden identity aggressive fingerprint")
{
    // 攻击优先档身份局自钉：证明身份阵营目标在 aggressive 档同样生效。
    // seed 2：主公阵营胜，13 回合，2 人阵亡、1 次击杀反贼奖励摸牌。
    //
    // 身份局 AI 阵营意识上线后的漂移（新旧日志逐行 diff 核对过）：179 → 169 行。
    // seed 2 角色为 P0=主公、P1=内奸、P2=反贼、P3=忠臣。首个分叉在 P0 的万箭齐发
    // 结算到 P1 的无懈窗口：旧口径 P1 因锦囊冲自己而连出两张无懈（偶数相抵仍受伤）；
    // 新口径内奸视主公为非敌（保主制衡），不再无懈主公的锦囊，直接吃下万箭伤害。
    // 此后伤害/死亡序列级联，行数减少；终局仍主公阵营胜。
    tkw::game::AggressiveAI aggr;
    const auto a = run_identity(aggr, 2, 4);
    const auto b = run_identity(aggr, 2, 4);
    REQUIRE(a.ok);
    REQUIRE(b.ok);

    CHECK(a.lines == b.lines);
    CHECK(!a.lines.empty());
    CHECK(a.lines.size() == IDENTITY_4P_AGGRESSIVE_LINES);
    CHECK(fingerprint(a.lines) == IDENTITY_4P_AGGRESSIVE_FP);

    check_identity_roles(a.roles, 4);
    check_identity_outcome(a);
    CHECK(a.outcome.camp == tkw::game::WinCamp::LordCamp);
    CHECK(a.kill_rewards >= 1);
    CHECK(a.deaths >= 1);
}

TEST_CASE("replay: identity four-player scan stays deterministic and legal")
{
    // 4 人身份局 12 个种子各跑两遍：同 seed 逐行一致，失败仅允许 MaxRounds，
    // 终局阵营非 None、胜者角色与阵营一致、角色表合法。不硬钉阵营分布，
    // 避免 AI 调整时必然变红；seed 1..12 实测直方图：主公 6 / 反贼 5 /
    // 内奸 1，全部在回合上限前终局。
    tkw::game::SimpleAI ai;
    int decisive = 0;
    for (std::uint32_t seed = 1; seed <= 12; ++seed)
    {
        const auto a = run_identity(ai, seed, 4);

        CHECK(!a.lines.empty());
        check_identity_roles(a.roles, 4);

        if (a.ok)
        {
            check_identity_outcome(a);
            if (a.outcome.camp != tkw::game::WinCamp::Draw)
                ++decisive;
        }
        else
        {
            CHECK(a.err == tkw::game::LoopError::MaxRounds);
        }

        // 同种子可重放：第二局逐行一致（扫描不放松确定性契约）
        const auto b = run_identity(ai, seed, 4);
        CHECK(a.lines == b.lines);
    }
    CHECK(decisive >= 1);
}

