#include <doctest/doctest.h>

#include <cstddef>
#include <sstream>
#include <string>

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

        DecisionChoice decide(const DecisionRequest &req) override
        {
            last = req.kind;
            legal_count = req.legal.size();
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
    CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha).is_none());
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
    // 己方：体力/手牌数/装备/判定逐项渲染
    CHECK(text.find("[a] 体力 4/4  手牌 1  装备 八卦阵  判定 乐不思蜀") !=
          std::string::npos);
    // 对手：体力/手牌数/装备/距离逐项渲染
    CHECK(text.find("b 体力 3/3  手牌 1  装备 青龙偃月刀  距离 1") !=
          std::string::npos);
    CHECK(text.find("s#1") != std::string::npos);      // 候选仍渲染
    CHECK(text.find("t#9") == std::string::npos);      // 对手手牌内容不泄露
}

TEST_CASE("ai: human decider plays the zhangba two-card action")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");

    // legal 顺序：1) 无中生有 2) 桃 3) 丈八两张当杀
    std::istringstream in("play 3\n");
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
        ai.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "x#1");
    CHECK(chosen.unwrap().second_instance_id == "x#2");

    // 手牌有真杀：真杀优先，不出两张当杀
    g.give("a", "sha", "s#1");
    const auto again =
        ai.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha);
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
        src.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha);
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
        src.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha);
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
                g.ctx, "a", tkw::card::ResponseKind::Sha)
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
                g.ctx, "a", tkw::card::ResponseKind::Sha)
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

    CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Jink).is_none());
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
        CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha)
                  .is_none());
        CHECK(out.str().find("需打出杀") != std::string::npos);
    }
    {
        std::istringstream in("pass\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Jink)
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
    CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha).is_none());
    CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Jink).is_none());
    CHECK(src.play_peach(g.ctx, "a", "b").is_none());
    CHECK(src.play_counter(g.ctx, "a", "", {}).is_none());
    CHECK_FALSE(src.trigger_effect(g.ctx, "a", tkw::card::Ability::NoShaLimit));
    CHECK(src.pick_card_from_target(g.ctx, "a", "b").is_none());
    const auto revealed = g.ctx.cards->hand("a");
    CHECK(src.pick_from_revealed(g.ctx, "a", revealed).is_none());
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

    const auto picked = src.pick_card_from_target(g.ctx, "a", "b");
    REQUIRE(picked.is_some());
    CHECK(picked.unwrap().instance_id == "s#2");
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

    std::istringstream in("pick 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto picked = src.pick_card_from_target(g.ctx, "a", "b");
    REQUIRE(picked.is_some());
    // 三区混排时按来源分区标注，避免拿错区域
    CHECK(out.str().find("[手] 杀 s#2") != std::string::npos);
    CHECK(out.str().find("[装] 青龙偃月刀 e#1") != std::string::npos);
    CHECK(out.str().find("[判] 乐不思蜀 d#1") != std::string::npos);
}

TEST_CASE("ai: routed ai sends human actor to human and falls back")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "tao", "t#1");
    g.give("a", "sha", "s#1");
    g.give("b", "sha", "s#2");

    std::istringstream in("play 1\n");
    std::ostringstream out;
    RoutedAI routed({"a"}, in, out);

    const tkw::game::TurnContext ta{"a", 0, 1};
    const auto a_choice = routed.choose_play(g.ctx, ta);
    REQUIRE(a_choice.is_some());
    CHECK(a_choice.unwrap().instance_id == "t#1");

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
        routed.play_response(g.ctx, "a", tkw::card::ResponseKind::Jink);
    REQUIRE(a_card.is_some());
    CHECK(a_card.unwrap().instance_id == "j#1");

    // b 非真人：SimpleAI 取首张闪，不消费输入流（输入已被 a 读空）。
    const auto b_card =
        routed.play_response(g.ctx, "b", tkw::card::ResponseKind::Jink);
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
    const auto chosen = ai.play_counter(g.ctx, "b", "a", {"b"});
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
    CHECK(ai.play_counter(g.ctx, "a", "a", {"b"}).is_none());  // 冲别人的
    CHECK(ai.play_counter(g.ctx, "a", "a", {"a"}).is_none());  // 自益
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
    CHECK(ai.play_counter(g.ctx, "b", "a", {"c"}).is_none());
    CHECK(ai.play_counter(g.ctx, "d", "a", {"c"}).is_none());
    const auto chosen = ai.play_counter(g.ctx, "c", "a", {"c"});
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
    const auto chosen = ai.play_counter(g.ctx, "b", "", {"b"});
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap() == "w#0");
    // 第三方持无懈也不救
    CHECK(ai.play_counter(g.ctx, "a", "", {"b"}).is_none());
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

    const auto chosen = src.play_counter(g.ctx, "a", "b", {"a"});
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap() == "w#0");
    CHECK(out.str().find("w#0") != std::string::npos);  // 无懈候选渲染
    CHECK(out.str().find("使用者: b") != std::string::npos);
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
        CHECK(src.play_counter(g.ctx, "a", "", {"a"}).is_none());
        CHECK(out.str().find("延时锦囊判定") != std::string::npos);
    }
    {
        std::istringstream in;  // 空流：首次读取即 EOF
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK(src.play_counter(g.ctx, "a", "b", {"a"}).is_none());
    }
}
