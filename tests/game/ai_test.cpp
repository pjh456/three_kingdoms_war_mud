#include <doctest/doctest.h>

#include <cstddef>
#include <string>

#include "game/ai/evaluator.hpp"
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

        DecisionChoice decide(const DecisionRequest &req) override
        {
            last = req.kind;
            legal_count = req.legal.size();
            DecisionChoice out;
            if (req.kind == DecisionKind::Play && !req.legal.empty())
            {
                out.instance_id = tkw::Option<std::string>::Some(
                    req.legal.front().card.instance_id);
                out.targets = req.legal.front().targets;
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

TEST_CASE("ai: evaluator scores cards and enemies")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);

    CHECK(card_value(*g.catalog.find("sha").unwrap()) > 0);
    CHECK(card_value(*g.catalog.find("tao").unwrap()) >
          card_value(*g.catalog.find("sha").unwrap()));

    const auto v = make_view(g.ctx, "a");
    REQUIRE(v.others.size() == 1);
    CHECK(threat_score(v.others[0]) > 0);
    CHECK(kill_priority(v.others[0]) == 1);
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
