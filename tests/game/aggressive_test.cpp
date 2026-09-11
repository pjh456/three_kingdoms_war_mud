#include <doctest/doctest.h>

#include <cstddef>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "game/ai/aggressive.hpp"
#include "game/ai/human.hpp"
#include "game/ai/legal.hpp"
#include "game/flow/turn.hpp"
#include "test_game.hpp"

namespace
{
    using tkw::test::TestGame;
}

TEST_CASE("aggressive: choose_play only returns legal_actions")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");
    g.give("a", "tao", "t#1");
    g.give("a", "wuzhong", "w#1");
    g.give("a", "nanman", "n#1");
    g.give("b", "shan", "j#1");

    tkw::game::AggressiveAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto chosen = ai.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());

    // 输出三元组须逐字命中 legal_actions 某动作（合法性硬契约）
    const auto legal = tkw::game::legal_actions(g.ctx, "a", turn);
    bool found = false;
    for (const auto &a : legal)
        if (a.card.instance_id == chosen.unwrap().instance_id &&
            a.targets == chosen.unwrap().targets)
            found = true;
    CHECK(found);
}

TEST_CASE("aggressive: plays damage before draw in hand order")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "wuzhong", "w#1");
    g.give("a", "sha", "s#1");

    tkw::game::AggressiveAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto chosen = ai.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    // 贪心档按手牌序会先摸牌；攻击优先档伤害先行
    CHECK(chosen.unwrap().instance_id == "s#1");
    CHECK(chosen.unwrap().targets == std::vector<std::string>{"b"});
}

TEST_CASE("aggressive: zhangba pair outranks other cards")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");  // 无真杀：两张当杀 pair 进入候选

    tkw::game::AggressiveAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto chosen = ai.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "x#1");
    CHECK(chosen.unwrap().second_instance_id == "x#2");
    CHECK(chosen.unwrap().targets == std::vector<std::string>{"b"});
}

TEST_CASE("aggressive: fangtian multi-target when sha is the whole hand")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    auto *c = g.add_player("c", 2, 4);
    auto *d = g.add_player("d", 3, 4);
    g.equip("a", "fangtian", "e#0");
    g.give("a", "sha", "s#1");  // 唯一手牌，杀天然消耗完全部手牌

    tkw::game::AggressiveAI ai;
    auto r = tkw::game::execute_turn(g.ctx, ai, "a");
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);
    CHECK(c->get_hp() == 3);
    CHECK(d->get_hp() == 3);  // 三目标各中一刀
    CHECK(g.cards.hand_size("a") == 0);
}

TEST_CASE("aggressive: fangtian stays single-target when the sha is not the whole hand")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    auto *c = g.add_player("c", 2, 4);
    auto *d = g.add_player("d", 3, 4);
    g.equip("a", "fangtian", "e#0");
    g.give("a", "guohe", "g#1");
    g.give("a", "sha", "s#1");
    g.give("d", "shan", "ds#1");  // 贪心档按手牌序先拆（杀成最后手牌触发多目标）

    tkw::game::AggressiveAI ai;
    auto r = tkw::game::execute_turn(g.ctx, ai, "a");
    REQUIRE(r.is_ok());
    // 攻击优先档先打杀：手牌 2 张 > 消耗 1，无多目标枚举，集火最低血 b
    CHECK(b->get_hp() == 3);
    CHECK(c->get_hp() == 4);
    CHECK(d->get_hp() == 4);
    // 再打过河：d 是唯一有牌目标，拆走闪
    CHECK(g.cards.hand_size("d") == 0);
    CHECK(g.cards.hand_size("a") == 0);
}

TEST_CASE("aggressive: skip healing at full hp")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "tao", "t#1");

    tkw::game::AggressiveAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    CHECK(ai.choose_play(g.ctx, turn).is_none());
}

TEST_CASE("aggressive: focuses the lowest hp target")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 1);
    g.add_player("c", 2, 4);
    g.equip("a", "qinglong", "e#0");  // 攻击范围 3，b/c 均在距
    g.give("a", "sha", "s#1");

    tkw::game::AggressiveAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto chosen = ai.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().targets == std::vector<std::string>{"b"});  // b 1 血
}

TEST_CASE("aggressive: pick_card_from_target prefers the highest value")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("b", "shan", "j#1");  // 闪 35
    g.give("b", "tao", "t#2");   // 桃 50

    tkw::game::AggressiveAI ai;
    const auto picked = ai.pick_card_from_target(g.ctx, "a", "b");
    REQUIRE(picked.is_some());
    CHECK(picked.unwrap().instance_id == "t#2");  // 贪心档取首张（闪）
}

TEST_CASE("aggressive: pick_from_revealed prefers the highest value")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);

    const std::vector<tkw::card::Card> revealed = {
        tkw::card::Card{"r#1", "shan", tkw::card::Suit::Heart, 2},
        tkw::card::Card{"r#2", "tao", tkw::card::Suit::Heart, 3}};

    tkw::game::AggressiveAI ai;
    const auto picked = ai.pick_from_revealed(g.ctx, "a", revealed);
    REQUIRE(picked.is_some());
    CHECK(picked.unwrap().instance_id == "r#2");  // 贪心档取亮牌首张
}

TEST_CASE("aggressive: discard keeps lowest value first")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "tao", "t#1");    // 桃 50
    g.give("a", "shan", "j#2");  // 闪 35
    g.give("a", "sha", "s#3");   // 杀 40
    g.give("a", "wuxie", "w#4");  // 无懈可击 55

    tkw::game::AggressiveAI ai;
    const auto chosen = ai.choose_discards(
        g.ctx, "a", 2, tkw::game::DiscardReason::TurnLimit);
    REQUIRE(chosen.size() == 2);
    CHECK(chosen[0] == "j#2");  // 最低价值先弃
    CHECK(chosen[1] == "s#3");
}

TEST_CASE("aggressive: trigger respects the discard cost")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    tkw::game::AggressiveAI ai;
    CHECK_FALSE(ai.trigger_effect(
        g.ctx, "a", tkw::card::Ability::DiscardTwoForceDamage));
    g.give("a", "sha", "s#9");
    CHECK(ai.trigger_effect(
        g.ctx, "a", tkw::card::Ability::DiscardTwoForceDamage));
    CHECK(ai.trigger_effect(
        g.ctx, "a", tkw::card::Ability::ExtraShaAfterJink));
}

TEST_CASE("aggressive: routed ai falls back to the injected tier")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("b", "wuzhong", "w#1");
    g.give("b", "sha", "s#1");

    // 4 参构造注入攻击优先档回落；b 非真人座位，不消费输入流
    std::istringstream in("sentinel\n");
    std::ostringstream out;
    tkw::game::ai::RoutedAI routed(
        {"a"}, in, out, std::make_unique<tkw::game::AggressiveAI>());

    const tkw::game::TurnContext tb{"b", 0, 1};
    const auto choice = routed.choose_play(g.ctx, tb);
    REQUIRE(choice.is_some());
    CHECK(choice.unwrap().instance_id == "s#1");  // 攻击优先：杀先于摸
    CHECK(in.str() == "sentinel\n");  // 回落不读真人输入
}
