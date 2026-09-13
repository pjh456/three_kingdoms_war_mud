#include <doctest/doctest.h>

#include <cstddef>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "game/ai/evaluator.hpp"
#include "game/ai/human.hpp"
#include "game/ai/legal.hpp"
#include "game/ai/simple.hpp"
#include "game/ai/view.hpp"
#include "test_game.hpp"

namespace
{
    using tkw::test::TestGame;
    using namespace tkw::game::ai;

    struct NoopDecider : Decider
    {
        DecisionChoice decide(const DecisionRequest &) override
        {
            return DecisionChoice{};
        }
    };

    struct RecordingDecider : Decider
    {
        DecisionKind last = DecisionKind::Play;
        std::size_t legal_count = 0;
        std::size_t pick_index = 0;     /**< 选中的 legal 动作下标（默认首个） */
        std::string second_instance_id; /**< 最近一次透传的第二张手牌 */
        std::vector<tkw::card::Card> options;     /**< 最近一次请求的候选牌 */
        std::vector<tkw::card::Zone> zone_labels; /**< 与 options 等长的来源分区 */

        DecisionChoice decide(const DecisionRequest &req) override
        {
            last = req.kind;
            legal_count = req.legal.size();
            options = req.options;
            zone_labels = req.zone_labels;
            DecisionChoice out;
            if (req.kind == DecisionKind::Play && req.legal.size() > pick_index)
            {
                const auto &act = req.legal[pick_index];
                out.instance_id = tkw::Option<std::string>::Some(
                    act.card.instance_id);
                out.targets = act.targets;
                out.second_instance_id = act.second_instance_id;
                second_instance_id = act.second_instance_id;
            }
            return out;
        }
    };
}

TEST_CASE("ai: view captures self and others")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 3);
    g.add_player("c", 2, 4);
    g.equip("b", "qinglong", "e#1");
    g.give("b", "sha", "s#1");
    g.give("a", "shan", "s#2");

    const auto v = make_view(g.ctx, "a");
    CHECK(v.self == "a");
    CHECK(v.self_seat == 0);
    CHECK(v.self_hp == 4);
    CHECK(v.hand.size() == 1);
    CHECK(v.equip.empty());
    CHECK(v.judge.empty());
    REQUIRE(v.others.size() == 2);
    CHECK(v.others[0].id == "b");
    CHECK(v.others[0].seat == 1);
    CHECK(v.others[0].hp == 3);
    CHECK(v.others[0].hand_size == 1);
    CHECK(v.others[0].has_weapon);
    CHECK(v.others[0].distance == 1);
    CHECK(v.others[0].in_attack_range);
    REQUIRE(v.others[0].equip.size() == 1);
    CHECK(v.others[0].equip[0].def_id == "qinglong");
    CHECK(v.others[0].judge.empty());
}

TEST_CASE("ai: view carries roles from the context")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Lord},
               {"b", tkw::game::Role::Rebel},
               {"c", tkw::game::Role::Loyalist}};

    const auto v = make_view(g.ctx, "a");
    CHECK(v.mode == tkw::game::GameMode::Identity);
    CHECK(v.self_role == tkw::game::Role::Lord);
    REQUIRE(v.others.size() == 2);
    CHECK(v.others[0].role == tkw::game::Role::Rebel);
    CHECK(v.others[1].role == tkw::game::Role::Loyalist);

    TestGame b("deck");
    b.add_player("a", 0, 4);
    b.add_player("b", 1, 4);
    const auto bv = make_view(b.ctx, "a");
    CHECK(bv.mode == tkw::game::GameMode::Brawl);
    CHECK(bv.self_role == tkw::game::Role::None);
    REQUIRE(bv.others.size() == 1);
    CHECK(bv.others[0].role == tkw::game::Role::None);
}

TEST_CASE("ai: evaluator scores cards")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);

    CHECK(card_value(*g.catalog.find("sha").unwrap()) > 0);
    CHECK(card_value(*g.catalog.find("tao").unwrap()) >
          card_value(*g.catalog.find("sha").unwrap()));
}

TEST_CASE("ai: simple choose_play only returns legal_actions")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");
    g.give("a", "tao", "t#1");

    tkw::game::SimpleAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto chosen = ai.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());

    const auto legal = tkw::game::legal_actions(g.ctx, "a", turn);
    bool found = false;
    for (const auto &a : legal)
        if (a.card.instance_id == chosen.unwrap().instance_id &&
            a.targets == chosen.unwrap().targets)
            found = true;
    CHECK(found);
}

TEST_CASE("ai: simple borrowed sword targets the lowest-hp victim")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.add_player("d", 3, 2);           // 最低体力，应被集火
    g.equip("b", "qinglong", "e#0");   // b 持武器，攻击范围 3 够到 c、d
    g.give("a", "jiedao", "j#0");

    tkw::game::SimpleAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto chosen = ai.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "j#0");
    CHECK(chosen.unwrap().targets == std::vector<std::string>{"b", "d"});
}

TEST_CASE("ai: identity lord camp targets the hostile not a lower-hp loyalist")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);  // 主公持杀
    g.add_player("b", 1, 1);  // 忠臣，体力最低但非敌意
    g.add_player("c", 2, 4);  // 反贼
    g.give("a", "sha", "s#1");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Lord},
               {"b", tkw::game::Role::Loyalist},
               {"c", tkw::game::Role::Rebel}};

    tkw::game::SimpleAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto chosen = ai.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().targets == std::vector<std::string>{"c"});
}

TEST_CASE("ai: identity rebel targets the lord before a lower-hp loyalist")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);  // 主公
    g.add_player("b", 1, 4);  // 反贼持杀
    g.add_player("c", 2, 1);  // 忠臣，体力最低
    g.give("b", "sha", "s#1");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Lord},
               {"b", tkw::game::Role::Rebel},
               {"c", tkw::game::Role::Loyalist}};

    tkw::game::SimpleAI ai;
    const tkw::game::TurnContext turn{"b", 0, 1};
    const auto chosen = ai.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().targets == std::vector<std::string>{"a"});
}

TEST_CASE("ai: identity traitor keeps the lowest-hp rule")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);  // 内奸持杀
    g.add_player("b", 1, 4);  // 主公
    g.add_player("c", 2, 1);  // 反贼，体力最低
    g.give("a", "sha", "s#1");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Traitor},
               {"b", tkw::game::Role::Lord},
               {"c", tkw::game::Role::Rebel}};

    tkw::game::SimpleAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto chosen = ai.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().targets == std::vector<std::string>{"c"});
}

TEST_CASE("ai: brawl targeting stays lowest-hp free-for-all")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 1);
    g.add_player("c", 2, 4);
    g.give("a", "sha", "s#1");
    // 不设 mode/roles：默认乱斗，全体无角色

    tkw::game::SimpleAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto chosen = ai.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().targets == std::vector<std::string>{"b"});
}

TEST_CASE("ai: RequestDecisionSource forwards choices to decider")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    NoopDecider noop;
    RequestDecisionSource src(noop);
    const tkw::game::TurnContext turn{"a", 0, 1};
    CHECK(src.choose_play(g.ctx, turn).is_none());
    CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha, {}).is_none());
    CHECK(src.choose_discards(g.ctx, "a", 1, tkw::game::DiscardReason::TurnLimit)
              .empty());
    CHECK_FALSE(src.trigger_effect(g.ctx, "a", tkw::card::Ability::NoShaLimit));
}

TEST_CASE("ai: decider receives play request with legal moves")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    RecordingDecider rec;
    RequestDecisionSource src(rec);
    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto act = src.choose_play(g.ctx, turn);
    CHECK(rec.last == DecisionKind::Play);
    CHECK(rec.legal_count > 0);
    REQUIRE(act.is_some());
    CHECK(act.unwrap().instance_id == "s#1");
}

TEST_CASE("ai: decider forwards the zhangba second card")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");  // 无真杀：两张当杀 pair 进入候选

    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto legal = tkw::game::legal_actions(g.ctx, "a", turn);
    std::size_t pair_index = legal.size();
    for (std::size_t i = 0; i < legal.size(); ++i)
        if (!legal[i].second_instance_id.empty())
        {
            pair_index = i;
            break;
        }
    REQUIRE(pair_index < legal.size());

    RecordingDecider rec;
    rec.pick_index = pair_index;
    RequestDecisionSource src(rec);
    const auto act = src.choose_play(g.ctx, turn);
    REQUIRE(act.is_some());
    CHECK(act.unwrap().second_instance_id == legal[pair_index].second_instance_id);
    CHECK(act.unwrap().second_instance_id == "x#2");
    CHECK(rec.second_instance_id == "x#2");
}

TEST_CASE("ai: simple trigger respects the discard cost")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    tkw::game::SimpleAI ai;
    CHECK_FALSE(ai.trigger_effect(
        g.ctx, "a", tkw::card::Ability::DiscardTwoForceDamage));
    g.give("a", "sha", "s#9");
    CHECK(ai.trigger_effect(
        g.ctx, "a", tkw::card::Ability::DiscardTwoForceDamage));
    CHECK(ai.trigger_effect(
        g.ctx, "a", tkw::card::Ability::ExtraShaAfterJink));
}

TEST_CASE("ai: human decider plays chosen legal action")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    std::istringstream in("play 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};

    const auto chosen = src.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "s#1");
    CHECK(out.str().find("s#1") != std::string::npos);
}

TEST_CASE("ai: human warns when borrowed sword targets self")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.equip("b", "qinglong", "e#0");
    g.give("a", "jiedao", "j#0");

    std::istringstream in("pass\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const auto chosen =
        src.choose_play(g.ctx, tkw::game::TurnContext{"a", 0, 1});
    CHECK(chosen.is_none());  // pass 结束出牌阶段

    const std::string text = out.str();
    // 自指候选带固定告警
    CHECK(text.find("-> b,a（警告：b 将对你出杀") != std::string::npos);
    // 非自指候选不带告警
    CHECK(text.find("-> b,c（警告") == std::string::npos);
    // 整次渲染只出现一条告警
    CHECK(text.find("将对你出杀") == text.rfind("将对你出杀"));
}

TEST_CASE("ai: human decider renders the situation summary")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 3);
    g.give("a", "sha", "s#1");
    g.give("b", "tao", "t#9");
    g.equip("a", "bagua", "e#0");
    g.equip("b", "qinglong", "e#1");
    const auto lesi = g.catalog.find("lesi");
    REQUIRE(lesi.is_some());
    const auto &copy = lesi.unwrap()->copies[0];
    g.cards.add_to_judge(
        "a", tkw::card::Card{"d#1", "lesi", copy.suit, copy.number});

    std::istringstream in("pass\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};
    CHECK(src.choose_play(g.ctx, turn).is_none());

    const std::string text = out.str();
    // 己方：体力/完整手牌牌名/装备/判定逐项渲染
    CHECK(text.find("[a] 体力 4/4  手牌 杀  装备 八卦阵  判定 乐不思蜀") !=
          std::string::npos);
    // 对手：体力/手牌数/装备/距离逐项渲染
    CHECK(text.find("b 体力 3/3  手牌 1  装备 青龙偃月刀  距离 1") !=
          std::string::npos);
    CHECK(text.find("s#1") != std::string::npos);      // 候选仍渲染
    CHECK(text.find("t#9") == std::string::npos);      // 对手手牌内容不泄露
}

TEST_CASE("ai: human decider lists the full own hand including unplayable cards")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");
    g.give("a", "shan", "j#2");  // 闪仅在响应窗口可用，不应进合法出牌候选
    g.give("b", "tao", "t#9");

    std::istringstream in("pass\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};
    CHECK(src.choose_play(g.ctx, turn).is_none());

    const std::string text = out.str();
    // 完整手牌按手牌序展开；对手仍只给数量
    CHECK(text.find("[a] 体力 4/4  手牌 杀/闪  装备 无  判定 无") !=
          std::string::npos);
    CHECK(text.find("b 体力 4/4  手牌 1") != std::string::npos);
    CHECK(text.find("t#9") == std::string::npos);
}

TEST_CASE("ai: human decider prints candidate card text before playing")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "guohe", "g#1");
    g.give("b", "sha", "s#2");  // 目标区域有牌，过河拆桥才进候选

    // card 1 先打印过河拆桥卡文，再 play 1 正常出牌
    std::istringstream in("card 1\nplay 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};

    const auto chosen = src.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "g#1");
    CHECK(out.str().find("过河拆桥：出牌阶段") != std::string::npos);
    CHECK(out.str().find("弃置其区域内的一张牌") != std::string::npos);
    CHECK(out.str().find("输入无效") == std::string::npos);
}

TEST_CASE("ai: human decider prints candidate card text in response and discard")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "shan", "j#1");
    g.give("a", "sha", "s#2");

    {
        // 响应窗口：card 1 打印闪的卡文后放弃
        std::istringstream in("card 1\npass\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Jink, {})
                  .is_none());
        CHECK(out.str().find("当你成为「杀」或「万箭齐发」的目标时") !=
              std::string::npos);
    }
    {
        // 弃牌窗口：card 1 打印候选卡文后再弃牌
        std::istringstream in("card 1\ndiscard 1 2\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        const auto chosen = src.choose_discards(
            g.ctx, "a", 2, tkw::game::DiscardReason::TurnLimit);
        REQUIRE(chosen.size() == 2);
        CHECK(chosen[0] == "j#1");
        CHECK(chosen[1] == "s#2");
        CHECK(out.str().find("闪：当你成为") != std::string::npos);
    }
}

TEST_CASE("ai: human decider reprompts on an out-of-range card lookup")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    std::istringstream in("card 9\nplay 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};

    const auto chosen = src.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "s#1");
    CHECK(out.str().find("输入无效：序号需为 1-") != std::string::npos);
}

TEST_CASE("ai: human decider plays the zhangba two-card action")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");

    // legal 顺序：1) 无中生有 2) 丈八两张当杀（满体力桃不列为候选）
    std::istringstream in("play 2\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};

    const auto chosen = src.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "x#1");
    CHECK(chosen.unwrap().second_instance_id == "x#2");
    CHECK(chosen.unwrap().targets == std::vector<std::string>{"b"});
    CHECK(out.str().find("x#2") != std::string::npos);  // 渲染了第二张牌
}

TEST_CASE("ai: simple ai answers a sha window with the zhangba pair")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");  // 无真杀：两张当杀

    SimpleAI ai;
    const auto chosen =
        ai.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha, {});
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "x#1");
    CHECK(chosen.unwrap().second_instance_id == "x#2");

    // 手牌有真杀：真杀优先，不出两张当杀
    g.give("a", "sha", "s#1");
    const auto again =
        ai.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha, {});
    REQUIRE(again.is_some());
    CHECK(again.unwrap().instance_id == "s#1");
    CHECK(again.unwrap().second_instance_id.empty());
}

TEST_CASE("ai: human decider answers a sha with a card pair")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");

    std::istringstream in("play 1 + 2\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto chosen =
        src.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha, {});
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "x#1");
    CHECK(chosen.unwrap().second_instance_id == "x#2");
    CHECK(out.str().find("x#2") != std::string::npos);  // 渲染了手牌列表
}

TEST_CASE("ai: human decider reprompts on a single index in the pair response")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");

    std::istringstream in("play 1\nplay 1 + 2\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto chosen =
        src.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha, {});
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "x#1");
    CHECK(chosen.unwrap().second_instance_id == "x#2");
    CHECK(out.str().find("输入无效") != std::string::npos);
}

TEST_CASE("ai: human decider declines the zhangba pair response")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");

    std::istringstream in("pass\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    CHECK(src.play_response(
                g.ctx, "a", tkw::card::ResponseKind::Sha, {})
              .is_none());
}

TEST_CASE("ai: human decider treats eof as decline in the pair response")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");

    std::istringstream in;  // 空流：首次读取即 EOF
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    CHECK(src.play_response(
                g.ctx, "a", tkw::card::ResponseKind::Sha, {})
              .is_none());
}

TEST_CASE("ai: human decider declines response")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "shan", "j#1");

    std::istringstream in("pass\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Jink, {}).is_none());
}

TEST_CASE("ai: human decider names the required response card")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");
    g.give("a", "shan", "j#1");

    {
        std::istringstream in("pass\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha, {})
                  .is_none());
        CHECK(out.str().find("需打出杀") != std::string::npos);
    }
    {
        std::istringstream in("pass\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Jink, {})
                  .is_none());
        CHECK(out.str().find("需打出闪") != std::string::npos);
    }
}

TEST_CASE("ai: human decider picks discards by indices")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");
    g.give("a", "shan", "j#2");
    g.give("a", "tao", "t#3");

    std::istringstream in("discard 1 1\ndiscard 1\ndiscard 1 3\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto chosen = src.choose_discards(
        g.ctx, "a", 2, tkw::game::DiscardReason::TurnLimit);
    REQUIRE(chosen.size() == 2);
    CHECK(chosen[0] == "s#1");
    CHECK(chosen[1] == "t#3");
    CHECK(out.str().find("输入无效") != std::string::npos);
    // 弃牌提示与非法原因统一用「序号」，重复选择给出专门原因
    CHECK(out.str().find("下标") == std::string::npos);
    CHECK(out.str().find("序号不能重复") != std::string::npos);
}

TEST_CASE("ai: human decider may pass the cixiong discard choice")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    {
        std::istringstream in("pass\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        const auto chosen = src.choose_discards(
            g.ctx, "a", 1, tkw::game::DiscardReason::CixiongChoice);
        CHECK(chosen.empty());
        CHECK(out.str().find("雌雄双股剑（可放弃）") != std::string::npos);
        CHECK(out.str().find("pass") != std::string::npos);
    }
    {
        // 其他弃牌原因不接受 pass：重提示后按序号弃牌
        std::istringstream in("pass\ndiscard 1\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        const auto chosen = src.choose_discards(
            g.ctx, "a", 1, tkw::game::DiscardReason::TurnLimit);
        REQUIRE(chosen.size() == 1);
        CHECK(chosen[0] == "s#1");
        CHECK(out.str().find("输入无效") != std::string::npos);
    }
}

TEST_CASE("ai: human decider reprompts on invalid input")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    std::istringstream in("foo\nplay 99\nplay 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};

    const auto chosen = src.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "s#1");

    std::size_t reprompts = 0;
    const std::string text = out.str();
    for (std::size_t pos = text.find("输入无效"); pos != std::string::npos;
         pos = text.find("输入无效", pos + 1))
        ++reprompts;
    CHECK(reprompts == 2);
    // 非法输入给出具体原因：动词/数量不对走用法提示，序号越界走范围提示
    CHECK(text.find("输入无效：请输入 play <序号> 或 pass。") !=
          std::string::npos);
    CHECK(text.find("输入无效：序号需为 1-") != std::string::npos);
}

TEST_CASE("ai: human decider silently reprompts on a blank line")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    std::istringstream in("\n   \nplay 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};

    const auto chosen = src.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "s#1");
    CHECK(out.str().find("输入无效") == std::string::npos);
}

TEST_CASE("ai: human decider trigger reads yes and no")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);

    {
        std::istringstream in("y\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK(src.trigger_effect(g.ctx, "a", tkw::card::Ability::NoShaLimit));
    }
    {
        std::istringstream in("n\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK_FALSE(
            src.trigger_effect(g.ctx, "a", tkw::card::Ability::NoShaLimit));
    }
    {
        std::istringstream in("maybe\ny\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK(src.trigger_effect(g.ctx, "a", tkw::card::Ability::NoShaLimit));
    }
}

TEST_CASE("ai: human decider treats eof as decline")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");
    g.give("a", "shan", "j#1");
    g.give("a", "tao", "t#1");
    g.give("b", "sha", "s#2");

    std::istringstream in;
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};

    CHECK(src.choose_play(g.ctx, turn).is_none());
    CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha, {}).is_none());
    CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Jink, {}).is_none());
    CHECK(src.play_peach(g.ctx, "a", "b").is_none());
    CHECK(src.play_counter(g.ctx, "a", "", {}, "").is_none());
    CHECK_FALSE(src.trigger_effect(g.ctx, "a", tkw::card::Ability::NoShaLimit));
    CHECK(src.pick_card_from_target(
              g.ctx, "a", "b", tkw::game::PickCardScope::HandEquipJudge)
              .is_none());
    const auto revealed = g.ctx.cards->hand("a");
    CHECK(src.pick_from_revealed(
              g.ctx, "a", revealed, tkw::game::RevealSource::Wugu)
              .is_none());
    CHECK(src.choose_discards(
                  g.ctx, "a", 1, tkw::game::DiscardReason::TurnLimit)
              .empty());
}

TEST_CASE("ai: human decider picks card from target")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("b", "sha", "s#2");

    std::istringstream in("pick 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto picked = src.pick_card_from_target(
        g.ctx, "a", "b", tkw::game::PickCardScope::HandEquipJudge);
    REQUIRE(picked.is_some());
    CHECK(picked.unwrap().instance_id == "s#2");
}

TEST_CASE("ai: pick_card_from_target filters the judgement zone by scope")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("b", "sha", "s#2");
    g.equip("b", "qinglong", "e#1");
    const auto lesi = g.catalog.find("lesi");
    REQUIRE(lesi.is_some());
    const auto &copy = lesi.unwrap()->copies[0];
    g.cards.add_to_judge(
        "b", tkw::card::Card{"d#1", "lesi", copy.suit, copy.number});

    RecordingDecider dec;
    RequestDecisionSource src(dec);

    // 寒冰剑范围：判定区不进候选，手牌/装备相对序不变
    (void)src.pick_card_from_target(
        g.ctx, "a", "b", tkw::game::PickCardScope::HandEquip);
    REQUIRE(dec.zone_labels.size() == 2);
    CHECK(dec.zone_labels[0] == tkw::card::Zone::Hand);
    CHECK(dec.zone_labels[1] == tkw::card::Zone::Equip);
    REQUIRE(dec.options.size() == 2);
    CHECK(dec.options[0].instance_id == "s#2");
    CHECK(dec.options[1].instance_id == "e#1");

    // 顺手牵羊/过河拆桥范围：判定区照常进入候选
    (void)src.pick_card_from_target(
        g.ctx, "a", "b", tkw::game::PickCardScope::HandEquipJudge);
    REQUIRE(dec.zone_labels.size() == 3);
    CHECK(dec.zone_labels[2] == tkw::card::Zone::Judge);
    REQUIRE(dec.options.size() == 3);
    CHECK(dec.options[2].instance_id == "d#1");
}

TEST_CASE("ai: human decider labels the target card zone")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("b", "sha", "s#2");
    g.equip("b", "qinglong", "e#1");
    const auto lesi = g.catalog.find("lesi");
    REQUIRE(lesi.is_some());
    const auto &copy = lesi.unwrap()->copies[0];
    g.cards.add_to_judge(
        "b", tkw::card::Card{"d#1", "lesi", copy.suit, copy.number});

    // 首槽 = 对手手牌：只渲染分区标签与遮挡占位，落子仍是真实牌
    {
        std::istringstream in("pick 1\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        const auto picked = src.pick_card_from_target(
            g.ctx, "a", "b", tkw::game::PickCardScope::HandEquipJudge);
        REQUIRE(picked.is_some());
        CHECK(picked.unwrap().instance_id == "s#2");

        const std::string text = out.str();
        CHECK(text.find("[手] （未知手牌）") != std::string::npos);
        CHECK(text.find("s#2") == std::string::npos);
        CHECK(text.find("杀") == std::string::npos);
        // 装备/判定区明置，照常显示
        CHECK(text.find("[装] 青龙偃月刀 e#1") != std::string::npos);
        CHECK(text.find("[判] 乐不思蜀 d#1") != std::string::npos);
    }
    // 遮挡不破坏跨区索引：第 2/3 槽仍映射到装备/判定区的真实牌
    {
        std::istringstream in("pick 2\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        const auto picked = src.pick_card_from_target(
            g.ctx, "a", "b", tkw::game::PickCardScope::HandEquipJudge);
        REQUIRE(picked.is_some());
        CHECK(picked.unwrap().instance_id == "e#1");
    }
    {
        std::istringstream in("pick 3\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        const auto picked = src.pick_card_from_target(
            g.ctx, "a", "b", tkw::game::PickCardScope::HandEquipJudge);
        REQUIRE(picked.is_some());
        CHECK(picked.unwrap().instance_id == "d#1");
    }
}

TEST_CASE("ai: human decider hides target hand card text on card lookup")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("b", "sha", "s#2");
    g.equip("b", "qinglong", "e#1");

    // card 1 命中对手手牌：只出遮挡提示，不打印该牌效果文案
    std::istringstream in("card 1\npick 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto picked = src.pick_card_from_target(
        g.ctx, "a", "b", tkw::game::PickCardScope::HandEquipJudge);
    REQUIRE(picked.is_some());
    CHECK(picked.unwrap().instance_id == "s#2");

    const std::string text = out.str();
    CHECK(text.find("无法查看") != std::string::npos);
    CHECK(text.find("出牌阶段限一次") == std::string::npos);
    CHECK(text.find("s#2") == std::string::npos);
}

TEST_CASE("ai: human decider still shows revealed zone card text")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("b", "sha", "s#2");
    g.equip("b", "qinglong", "e#1");

    // card 2 命中明置装备区：照常打印装备效果文案与实例号
    std::istringstream in("card 2\npick 2\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto picked = src.pick_card_from_target(
        g.ctx, "a", "b", tkw::game::PickCardScope::HandEquipJudge);
    REQUIRE(picked.is_some());
    CHECK(picked.unwrap().instance_id == "e#1");

    const std::string text = out.str();
    CHECK(text.find("再使用一张") != std::string::npos);
    CHECK(text.find("e#1") != std::string::npos);
}

TEST_CASE("ai: routed ai sends human actor to human and falls back")
{
    TestGame g("deck");
    auto *a = g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    a->take_damage("b", 1, false);  // 受伤：桃成为合法候选，真人可显式选杀
    g.give("a", "tao", "t#1");
    g.give("a", "sha", "s#1");
    g.give("b", "sha", "s#2");

    std::istringstream in("play 2\n");
    std::ostringstream out;
    RoutedAI routed({"a"}, in, out);

    const tkw::game::TurnContext ta{"a", 0, 1};
    const auto a_choice = routed.choose_play(g.ctx, ta);
    REQUIRE(a_choice.is_some());
    CHECK(a_choice.unwrap().instance_id == "s#1");

    const tkw::game::TurnContext tb{"b", 0, 1};
    const auto b_choice = routed.choose_play(g.ctx, tb);
    REQUIRE(b_choice.is_some());
    CHECK(b_choice.unwrap().instance_id == "s#2");
}

TEST_CASE("ai: routed ai falls back for non-human response window")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "shan", "j#1");
    g.give("b", "shan", "j#2");

    std::istringstream in("play 1\n");
    std::ostringstream out;
    RoutedAI routed({"a"}, in, out);

    const auto a_card =
        routed.play_response(g.ctx, "a", tkw::card::ResponseKind::Jink, {});
    REQUIRE(a_card.is_some());
    CHECK(a_card.unwrap().instance_id == "j#1");

    // b 非真人：SimpleAI 取首张闪，不消费输入流（输入已被 a 读空）。
    const auto b_card =
        routed.play_response(g.ctx, "b", tkw::card::ResponseKind::Jink, {});
    REQUIRE(b_card.is_some());
    CHECK(b_card.unwrap().instance_id == "j#2");
}

TEST_CASE("ai: simple discard prefers the lowest value cards")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "tao", "t#1");    // 桃 50
    g.give("a", "shan", "j#2");  // 闪 35
    g.give("a", "sha", "s#3");   // 杀 40
    g.give("a", "wuxie", "w#4");  // 无懈可击 55

    SimpleAI ai;
    const auto chosen = ai.choose_discards(
        g.ctx, "a", 2, tkw::game::DiscardReason::TurnLimit);
    REQUIRE(chosen.size() == 2);
    CHECK(chosen[0] == "j#2");  // 价值最低的先弃
    CHECK(chosen[1] == "s#3");
}

TEST_CASE("ai: simple discard keeps hand order for equal values")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "shan", "j#1");  // 闪 35
    g.give("a", "sha", "s#2");   // 杀 40
    g.give("a", "shan", "j#3");  // 闪 35

    SimpleAI ai;
    const auto chosen = ai.choose_discards(
        g.ctx, "a", 2, tkw::game::DiscardReason::TurnLimit);
    REQUIRE(chosen.size() == 2);
    CHECK(chosen[0] == "j#1");  // 同价值保持手牌序
    CHECK(chosen[1] == "j#3");
}

TEST_CASE("ai: simple discard orders the whole hand by value")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "tao", "t#1");
    g.give("a", "shan", "j#2");
    g.give("a", "sha", "s#3");
    g.give("a", "wuxie", "w#4");

    SimpleAI ai;
    const auto chosen = ai.choose_discards(
        g.ctx, "a", 4, tkw::game::DiscardReason::TurnLimit);
    REQUIRE(chosen.size() == 4);
    CHECK(chosen[0] == "j#2");  // 35
    CHECK(chosen[1] == "s#3");  // 40
    CHECK(chosen[2] == "t#1");  // 50
    CHECK(chosen[3] == "w#4");  // 55
}

TEST_CASE("ai: simple counter plays against an enemy trick targeting self")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("b", "wuxie", "w#0");

    SimpleAI ai;
    const auto chosen = ai.play_counter(g.ctx, "b", "a", {"b"}, "");
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap() == "w#0");
}

TEST_CASE("ai: simple counter declines own trick")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "wuxie", "w#0");

    SimpleAI ai;
    CHECK(ai.play_counter(g.ctx, "a", "a", {"b"}, "").is_none());  // 冲别人的
    CHECK(ai.play_counter(g.ctx, "a", "a", {"a"}, "").is_none());  // 自益
}

TEST_CASE("ai: simple counter declines a third party's trick")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.add_player("d", 3, 4);
    g.give("b", "wuxie", "w#2");
    g.give("c", "wuxie", "w#0");
    g.give("d", "wuxie", "w#3");

    SimpleAI ai;
    // a 的锦囊冲 c：旁观者 b/d 持无懈也不出（省牌），目标本人 c 出
    CHECK(ai.play_counter(g.ctx, "b", "a", {"c"}, "").is_none());
    CHECK(ai.play_counter(g.ctx, "d", "a", {"c"}, "").is_none());
    const auto chosen = ai.play_counter(g.ctx, "c", "a", {"c"}, "");
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap() == "w#0");
}

TEST_CASE("ai: simple counter plays on a delayed trick judged on self")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "wuxie", "w#1");
    g.give("b", "wuxie", "w#0");

    SimpleAI ai;
    // 判定窗口：使用者空串哨兵，目标 = 被判定玩家
    const auto chosen = ai.play_counter(g.ctx, "b", "", {"b"}, "");
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap() == "w#0");
    // 第三方持无懈也不救
    CHECK(ai.play_counter(g.ctx, "a", "", {"b"}, "").is_none());
}

TEST_CASE("ai: human decider counter window renders context and reads play")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "wuxie", "w#0");

    std::istringstream in("play 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto chosen = src.play_counter(g.ctx, "a", "b", {"a"}, "wugu");
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap() == "w#0");
    CHECK(out.str().find("w#0") != std::string::npos);  // 无懈候选渲染
    CHECK(out.str().find("使用者: b") != std::string::npos);
    CHECK(out.str().find("五谷丰登") != std::string::npos);
    CHECK(out.str().find("目标: a") != std::string::npos);
}

TEST_CASE("ai: human decider counter window declines on pass or eof")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "wuxie", "w#0");

    {
        std::istringstream in("pass\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK(src.play_counter(g.ctx, "a", "", {"a"}, "shandian").is_none());
        CHECK(out.str().find("延时锦囊判定") != std::string::npos);
        CHECK(out.str().find("闪电") != std::string::npos);
    }
    {
        std::istringstream in;  // 空流：首次读取即 EOF
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK(src.play_counter(g.ctx, "a", "b", {"a"}, "").is_none());
    }
}

TEST_CASE("ai: human discard window help prints usage and continues")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");
    g.give("a", "shan", "j#2");
    g.give("a", "tao", "t#3");

    std::istringstream in("help\ndiscard 1 2\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto chosen = src.choose_discards(
        g.ctx, "a", 2, tkw::game::DiscardReason::TurnLimit);
    REQUIRE(chosen.size() == 2);
    CHECK(out.str().find("用法：") != std::string::npos);
    CHECK(out.str().find("discard <序号>") != std::string::npos);
}

TEST_CASE("ai: human discard pass names the requirement")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    std::istringstream in("pass\ndiscard 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto chosen = src.choose_discards(
        g.ctx, "a", 1, tkw::game::DiscardReason::TurnLimit);
    REQUIRE(chosen.size() == 1);
    CHECK(chosen[0] == "s#1");
    CHECK(out.str().find("不能 pass") != std::string::npos);
    CHECK(out.str().find("需弃 1 张") != std::string::npos);
}

TEST_CASE("ai: human discard eof announces give-up")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    std::istringstream in;
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto chosen = src.choose_discards(
        g.ctx, "a", 1, tkw::game::DiscardReason::TurnLimit);
    CHECK(chosen.empty());
    CHECK(out.str().find("输入已结束") != std::string::npos);
    CHECK(out.str().find("按放弃处理") != std::string::npos);
}

TEST_CASE("ai: human play window help prints usage")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    std::istringstream in("?\npass\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};

    CHECK(src.choose_play(g.ctx, turn).is_none());
    CHECK(out.str().find("用法：") != std::string::npos);
    CHECK(out.str().find("pass 结束出牌阶段") != std::string::npos);
}

TEST_CASE("ai: human trigger window help prints usage")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);

    std::istringstream in("help\ny\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    CHECK(src.trigger_effect(g.ctx, "a", tkw::card::Ability::NoShaLimit));
    CHECK(out.str().find("输入 y 发动") != std::string::npos);
}

TEST_CASE("ai: human response window shows source and consequence")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "shan", "j#1");

    std::istringstream in("pass\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    CHECK(src.play_response(
              g.ctx, "a", tkw::card::ResponseKind::Jink,
              tkw::game::ResponsePrompt{"wanjian", "b", 1})
              .is_none());
    CHECK(out.str().find("来源: b 的 万箭齐发") != std::string::npos);
    CHECK(out.str().find("不出将受到 1 点伤害") != std::string::npos);
}

TEST_CASE("ai: human response window falls back when source unknown")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "shan", "j#1");

    std::istringstream in("pass\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    CHECK(src.play_response(
              g.ctx, "a", tkw::card::ResponseKind::Jink,
              tkw::game::ResponsePrompt{})
              .is_none());
    CHECK(out.str().find("响应（需打出闪）") != std::string::npos);
    CHECK(out.str().find("来源") == std::string::npos);
}

TEST_CASE("ai: human response window reports no candidate")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");  // 无闪：响应窗口无候选

    std::istringstream in;
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    CHECK(src.play_response(
              g.ctx, "a", tkw::card::ResponseKind::Jink,
              tkw::game::ResponsePrompt{})
              .is_none());
    CHECK(out.str().find("无可用响应牌") != std::string::npos);
}

TEST_CASE("ai: human reveal window names wugu source")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    std::istringstream in("pick 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto picked = src.pick_from_revealed(
        g.ctx, "a", g.ctx.cards->hand("a"), tkw::game::RevealSource::Wugu);
    REQUIRE(picked.is_some());
    CHECK(out.str().find("五谷丰登亮牌") != std::string::npos);
}

TEST_CASE("ai: human reveal window names qilin source")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("b", "qilin", "h#1");
    const auto horses = g.ctx.cards->equip("b");

    std::istringstream in("pick 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto picked = src.pick_from_revealed(
        g.ctx, "a", horses, tkw::game::RevealSource::Qilin);
    REQUIRE(picked.is_some());
    CHECK(out.str().find("麒麟弓") != std::string::npos);
}

TEST_CASE("ai: human trigger window explains ability")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("a", "qilin", "e#0");

    std::istringstream in("y\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    CHECK(src.trigger_effect(
        g.ctx, "a", tkw::card::Ability::DiscardHorseOnDamage));
    CHECK(out.str().find("麒麟弓") != std::string::npos);
    CHECK(out.str().find("坐骑") != std::string::npos);
}

namespace
{
    /** @brief 可变实体上可扣血（红侧对照：证明该操作确实存在）。 */
    template <typename T>
    concept HasTakeDamage = requires(T *e) {
        e->take_damage("x", 1, false);
    };

    /** @brief 可变牌容器上可移除手牌（红侧对照）。 */
    template <typename T>
    concept CanRemoveFromHand = requires(T *c, const std::string &id) {
        c->remove_from_hand(id, id);
    };

    /** @brief const 管理器可直接迭代（仅当提供了 const begin/end 时为真）。 */
    template <typename T>
    concept ConstIterable = requires(const T &m) {
        m.begin();
        m.end();
    };
}

TEST_CASE("ai: decision context is read-only at compile time")
{
    using tkw::game::GameContext;
    using tkw::game::ReadOnlyContext;
    // 接缝只暴露 const 容器指针
    static_assert(std::is_same_v<decltype(ReadOnlyContext{}.cards),
                                 const tkw::card::CardManager *>);
    static_assert(std::is_same_v<decltype(ReadOnlyContext{}.entities),
                                 const tkw::EntityManager *>);
    static_assert(std::is_same_v<decltype(ReadOnlyContext{}.catalog),
                                 const tkw::card::CardDefCatalog *>);
    static_assert(std::is_same_v<decltype(ReadOnlyContext{}.rules),
                                 const tkw::game::RulesConfig *>);
    static_assert(std::is_same_v<decltype(ReadOnlyContext{}.mode),
                                 const tkw::game::GameMode *>);
    static_assert(std::is_same_v<decltype(ReadOnlyContext{}.roles),
                                 const tkw::game::RoleTable *>);
    // 同一改状态操作在可变容器成立、在接缝的 const 容器上不可达
    static_assert(HasTakeDamage<tkw::entity::Entity>);
    static_assert(!HasTakeDamage<const tkw::entity::Entity>);
    static_assert(CanRemoveFromHand<tkw::card::CardManager>);
    static_assert(!CanRemoveFromHand<const tkw::card::CardManager>);
    // const 实体管理器不可迭代：实体只读遍历唯一入口是 const_view()
    static_assert(!ConstIterable<tkw::EntityManager>);
    // 接缝无法还原出可变上下文（一旦进入只读，改状态路径被类型切断）
    static_assert(!std::is_convertible_v<ReadOnlyContext, GameContext>);
    CHECK(true);
}
