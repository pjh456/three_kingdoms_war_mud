#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "card/card.hpp"
#include "card/catalog.hpp"
#include "card/def.hpp"
#include "card/manager.hpp"
#include "config/resource.hpp"
#include "entity/hp.hpp"
#include "entity/manager.hpp"
#include "event/event_bus.hpp"
#include "game/core/card_event.hpp"
#include "game/core/context.hpp"
#include "game/core/decision.hpp"
#include "game/query/distance.hpp"
#include "game/query/judge.hpp"
#include "game/ai/legal.hpp"
#include "game/flow/loop.hpp"
#include "game/resolve/resolver.hpp"
#include "game/flow/table.hpp"
#include "game/flow/turn.hpp"
#include "game/ai/simple.hpp"
#include "game/resolve/audit.hpp"
#include "util/rng.hpp"
#include "test_game.hpp"

namespace
{
    using namespace tkw::game;
    using tkw::EntityManager;
    using tkw::Option;
    using tkw::card::Ability;
    using tkw::card::Card;
    using tkw::card::CardDef;
    using tkw::card::CardDefCatalog;
    using tkw::card::CardManager;
    using tkw::card::ResponseKind;
    using tkw::card::Suit;
    using tkw::entity::Entity;
    using tkw::entity::Gender;
    using tkw::entity::Hp;

    using tkw::test::TestDecider;
    using tkw::test::TestGame;
}

TEST_CASE("game: seat distance on a circle")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.add_player("d", 3, 4);

    CHECK(seat_distance(g.ctx, "a", "a") == 0);
    CHECK(seat_distance(g.ctx, "a", "b") == 1);
    CHECK(seat_distance(g.ctx, "a", "c") == 2);
    CHECK(seat_distance(g.ctx, "a", "d") == 1);
    CHECK(seat_distance(g.ctx, "b", "d") == 2);
}

TEST_CASE("game: attack range base and weapon")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.add_player("d", 3, 4);

    // 无武器：基础攻击距离 1
    CHECK(in_attack_range(g.ctx, "a", "b"));
    CHECK(in_attack_range(g.ctx, "a", "d"));
    CHECK(!in_attack_range(g.ctx, "a", "c"));

    // 青龙偃月刀 range 3
    g.equip("a", "qinglong", "e#0");
    CHECK(in_attack_range(g.ctx, "a", "c"));
}

TEST_CASE("game: horses adjust attack distance")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.add_player("d", 3, 4);

    // a 装 -1马（赤兔）：a 到 c 距离 2-1=1 ≤ 1
    g.equip("a", "chitu", "h#1");
    CHECK(in_attack_range(g.ctx, "a", "c"));

    // b 装 +1马（绝影）：c 到 b 距离 1+1=2 > 1
    g.equip("b", "jueying", "h#2");
    CHECK(!in_attack_range(g.ctx, "c", "b"));
    // a→b 仍为 1（-1 与 +1 相抵）
    CHECK(in_attack_range(g.ctx, "a", "b"));
}

TEST_CASE("game: offensive and defensive horses coexist")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.add_player("d", 3, 4);
    g.cards.build_deck(g.catalog);
    g.give("a", "chitu", "h#1");  // -1马
    g.give("a", "dilu", "h#2");   // +1马

    TestDecider decider;
    decider.plays = {PlayAction{"h#1", {}}, PlayAction{"h#2", {}}};
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(g.cards.equip_size("a") == 2);      // 两个坐骑槽互不替换
    CHECK(in_attack_range(g.ctx, "a", "c"));  // a→c: 2-1=1

    g.equip("c", "jueying", "h#3");           // c 的 +1马
    CHECK(!in_attack_range(g.ctx, "a", "c")); // 1+1=2
}

TEST_CASE("game: same-direction horse replaces previous")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.cards.build_deck(g.catalog);
    g.give("a", "chitu", "h#1");  // -1马
    g.give("a", "dawan", "h#2");  // 也是 -1马

    TestDecider decider;
    decider.plays = {PlayAction{"h#1", {}}, PlayAction{"h#2", {}}};
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(g.cards.equip_size("a") == 1);              // 同方向坐骑互相替换
    CHECK(g.cards.equip("a")[0].def_id == "dawan");
}

TEST_CASE("game: valid_targets by scope and range")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.add_player("d", 3, 4);

    auto sha = g.catalog.find("sha").unwrap();
    auto wuzhong = g.catalog.find("wuzhong").unwrap();
    auto taoyuan = g.catalog.find("taoyuan").unwrap();
    auto nanman = g.catalog.find("nanman").unwrap();

    auto t_sha = valid_targets(g.ctx, "a", *sha);          // 攻击距离 1 → b, d
    CHECK(t_sha == std::vector<std::string>({"b", "d"}));
    auto t_wu = valid_targets(g.ctx, "a", *wuzhong);       // 仅自己
    CHECK(t_wu == std::vector<std::string>({"a"}));
    auto t_ty = valid_targets(g.ctx, "a", *taoyuan);       // 全部
    CHECK(t_ty == std::vector<std::string>({"a", "b", "c", "d"}));
    auto t_nm = valid_targets(g.ctx, "a", *nanman);        // 其他全部
    CHECK(t_nm == std::vector<std::string>({"b", "c", "d"}));
}

TEST_CASE("game: sha hits when no jink")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.give("a", "sha", "s#0");

    TestDecider decider;  // 不响应
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);
    CHECK(g.cards.hand_size("a") == 0);
    CHECK(g.cards.discard_size() == 1);  // 打出的杀已弃置
}

TEST_CASE("game: sha blocked by jink")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.give("a", "sha", "s#0");
    g.give("b", "shan", "s#1");

    TestDecider decider;
    decider.respond = true;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 4);                    // 未受伤
    CHECK(g.cards.hand_size("b") == 0);         // 闪已消耗
    CHECK(g.cards.discard_size() == 2);         // 杀 + 闪
}

TEST_CASE("game: response uses the exact card chosen by decision source")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.give("a", "sha", "s#0");
    g.give("b", "shan", "j#1");
    g.give("b", "shan", "j#2");

    TestDecider decider;
    decider.response_id = "j#2";  // 指定打出第二张闪
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 4);                             // 闪住
    CHECK(g.cards.hand_size("b") == 1);
    CHECK(g.cards.hand("b")[0].instance_id == "j#1");    // 留下的是第一张
}

TEST_CASE("game: sha out of range is rejected and card kept")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.add_player("d", 3, 4);
    g.give("a", "sha", "s#0");

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"c"});  // 距离 2
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::OutOfRange);
    CHECK(g.cards.hand_size("a") == 1);  // 牌未消耗
}

TEST_CASE("game: sha rejects multiple targets")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.add_player("d", 3, 4);
    g.give("a", "sha", "s#0");

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b", "d"});  // 杀只能指定一名目标
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::InvalidTarget);
    CHECK(g.cards.hand_size("a") == 1);
}

TEST_CASE("game: one-other card rejects self as target")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#0");

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"a"});
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::InvalidTarget);
    CHECK(g.cards.hand_size("a") == 1);
}

TEST_CASE("game: self card rejects other target")
{
    TestGame g("deck");
    auto *a = g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    a->take_damage("b", 1, false);  // a: 3
    g.give("a", "tao", "t#0");

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});  // 桃只能对自己
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::InvalidTarget);
    CHECK(g.cards.hand_size("a") == 1);
}

TEST_CASE("game: aoe rejects partial target list")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.give("a", "nanman", "n#0");

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});  // 南蛮须覆盖所有其他角色
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::InvalidTarget);
    CHECK(g.cards.hand_size("a") == 1);
}

TEST_CASE("game: aoe rejects duplicate targets")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.add_player("d", 3, 4);
    g.give("a", "nanman", "n#0");

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    // 南蛮须覆盖其他角色一次且仅一次：{b,b,d} 数量对上（3==legal 3）但 b 重复
    auto r = resolve_play(g.ctx, decider, "a", played, {"b", "b", "d"});
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::InvalidTarget);
    CHECK(g.cards.hand_size("a") == 1);  // 牌未消耗
}

TEST_CASE("game: card not in hand is rejected without effect")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);

    TestDecider decider;
    Card ghost{"ghost#0", "sha", Suit::Spade, 7};  // 不属于任何区域
    auto r = resolve_play(g.ctx, decider, "a", ghost, {"b"});
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::CardNotOwned);
    CHECK(b->get_hp() == 4);           // 未结算
    CHECK(g.cards.hand_size("a") == 0);
}

TEST_CASE("game: card owned by another player cannot be played")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.give("b", "sha", "s#1");

    TestDecider decider;
    const auto stolen = g.cards.hand("b")[0];
    auto r = resolve_play(g.ctx, decider, "a", stolen, {"b"});
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::CardNotOwned);
    CHECK(b->get_hp() == 4);
    CHECK(g.cards.hand_size("b") == 1);  // b 的牌还在
}

TEST_CASE("game: tao heals")
{
    TestGame g("deck");
    auto *a = g.add_player("a", 0, 3);
    g.add_player("b", 1, 4);
    a->take_damage("b", 1, false);
    CHECK(a->get_hp() == 2);
    g.give("a", "tao", "t#0");

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"a"});
    REQUIRE(r.is_ok());
    CHECK(a->get_hp() == 3);
}

TEST_CASE("game: wuzhong draws two")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.cards.build_deck(g.catalog);
    g.give("a", "wuzhong", "w#0");
    const auto before = g.cards.hand_size("a");

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"a"});
    REQUIRE(r.is_ok());
    CHECK(g.cards.hand_size("a") == before - 1 + 2);  // 打出 1 张 + 摸 2 张
    CHECK(g.cards.draw_size() == 108 - 2);
}

TEST_CASE("game: wugu reveals one card per player and deals one each")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.cards.build_deck(g.catalog);
    g.give("a", "wugu", "w#0");

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"a", "b"});
    REQUIRE(r.is_ok());
    CHECK(g.cards.hand_size("a") == 1);  // 打出 1 张 -1，又选得 1 张
    CHECK(g.cards.hand_size("b") == 1);  // 选得 1 张
    CHECK(g.cards.draw_size() == 108 - 2);
}

TEST_CASE("game: guohe discards target card")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "guohe", "g#0");
    g.give("b", "sha", "s#1");
    g.give("b", "shan", "s#2");

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(g.cards.hand_size("b") == 1);   // 弃了一张
    CHECK(g.cards.discard_size() == 2);   // 过河拆桥 + 被弃的牌
}

TEST_CASE("game: failed pick rolls back the played card")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "guohe", "g#0");  // b 无牌 → pick 返回 None

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::InvalidChoice);
    CHECK(g.cards.hand_size("a") == 1);  // 打出的过河拆桥被取回
    CHECK(g.cards.discard_size() == 0);
}

TEST_CASE("game: invalid pick is rejected without partial effect")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "guohe", "g#0");
    g.give("b", "sha", "s#1");

    TestDecider decider;
    decider.bogus_pick = true;  // 决策源返回一张不存在的牌
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::InvalidChoice);
    CHECK(g.cards.hand_size("a") == 1);  // 打出的牌已回滚
    CHECK(g.cards.hand_size("b") == 1);  // 目标牌未被动
    CHECK(g.cards.discard_size() == 0);
}

TEST_CASE("game: shunshou steals target card to hand")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "shunshou", "ss#0");
    g.give("b", "tao", "t#1");

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(g.cards.hand_size("b") == 0);
    CHECK(g.cards.hand_size("a") == 1);   // 顺来的桃
    CHECK(g.cards.hand("a")[0].def_id == "tao");
}

TEST_CASE("game: nanman hits all others without sha")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    auto *c = g.add_player("c", 2, 4);
    g.give("a", "nanman", "n#0");
    g.give("b", "sha", "s#1");  // b 有杀

    TestDecider decider;  // 不响应
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b", "c"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);
    CHECK(c->get_hp() == 3);

    // 响应：b 打出杀免伤，c 无杀受伤
    TestDecider yes;
    yes.respond = true;
    g.give("a", "nanman", "n#1");
    const auto played2 = g.cards.hand("a")[0];
    auto r2 = resolve_play(g.ctx, yes, "a", played2, {"b", "c"});
    REQUIRE(r2.is_ok());
    CHECK(b->get_hp() == 3);  // 第二次 b 已无杀，仍受伤
    CHECK(c->get_hp() == 2);
}

TEST_CASE("game: juedou target without sha takes damage")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.give("a", "juedou", "j#0");

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);  // 目标先开始，不出杀 → 受 a 造成的 1 点伤害
}

TEST_CASE("game: juedou exchange of sha")
{
    TestGame g("deck");
    auto *a = g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.give("a", "juedou", "j#0");
    g.give("a", "sha", "s#1");
    g.give("b", "sha", "s#2");

    TestDecider decider;
    decider.respond = true;
    const auto played = g.cards.hand("a")[0];  // 决斗
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(a->get_hp() == 4);
    CHECK(b->get_hp() == 3);  // b 的杀耗尽后受 1 点伤害
}

TEST_CASE("game: juedou round exhaustion settles as draw")
{
    TestGame g("deck");
    g.rules.duel_rounds = 2;  // 注入小保险上限使耗尽可达
    auto *a = g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.give("a", "juedou", "j#0");
    g.give("a", "sha", "s#1");
    g.give("b", "sha", "s#2");

    TestDecider decider;
    decider.respond = true;  // 双方每轮都出手牌中第一张杀 → 耗尽轮次
    const auto played = g.cards.hand("a")[0];  // 决斗
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(a->get_hp() == 4);   // 无人「先不出」→ 平局，不再造成伤害
    CHECK(b->get_hp() == 4);
    CHECK(g.cards.hand_size("a") == 0);
    CHECK(g.cards.hand_size("b") == 0);
    CHECK(g.cards.discard_size() == 3);  // 决斗牌 + 双方杀全部正常消耗
}

TEST_CASE("game: jiedao forces holder to use sha")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.equip("b", "qinglong", "e#0");  // b 持武器
    g.give("a", "jiedao", "j#0");
    g.give("b", "sha", "s#1");

    TestDecider decider;
    decider.respond = true;  // b 选择出杀
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b", "b"});  // A=b, B=b
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);             // b 对自己出杀，命中
    CHECK(g.cards.hand_size("b") == 0);  // 杀已消耗
    CHECK(g.cards.equip_size("b") == 1); // 武器仍在
}

TEST_CASE("game: jiedao takes weapon when holder does not respond")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("b", "qinglong", "e#0");
    g.give("a", "jiedao", "j#0");
    g.give("b", "sha", "s#1");

    TestDecider decider;  // 不出杀
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b", "b"});
    REQUIRE(r.is_ok());
    CHECK(g.cards.equip_size("b") == 0);  // 武器被拿走
    CHECK(g.cards.hand_size("a") == 1);
    CHECK(g.cards.hand("a")[0].def_id == "qinglong");
}

TEST_CASE("game: jiedao requires holder to have a weapon")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "jiedao", "j#0");

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b", "b"});
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::InvalidTarget);
    CHECK(g.cards.hand_size("a") == 1);  // 未消耗
}

TEST_CASE("game: validate_effect_targets pins the borrowed sword special case")
{
    // targets = {A(持武器者), B(A 攻击范围内角色)}
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.add_player("d", 3, 4);
    g.add_player("e", 4, 4);
    g.add_player("f", 5, 4);
    g.equip("b", "qinggang", "e#0");  // b 持武器，攻击范围 2
    const CardDef &jiedao = *g.catalog.find("jiedao").unwrap();

    // 持武器者 + 攻击范围内目标：合法
    CHECK(validate_effect_targets(g.ctx, "a", jiedao, {"b", "d"}).is_ok());

    // 目标数量不符 scope
    auto r = validate_effect_targets(g.ctx, "a", jiedao, {"b"});
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::InvalidTarget);

    // 目标超出持武器者攻击范围
    r = validate_effect_targets(g.ctx, "a", jiedao, {"b", "e"});
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::OutOfRange);

    // 持武器者无武器
    TestGame h("deck");
    h.add_player("a", 0, 4);
    h.add_player("b", 1, 4);
    const CardDef &jiedao2 = *h.catalog.find("jiedao").unwrap();
    r = validate_effect_targets(h.ctx, "a", jiedao2, {"b", "b"});
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::InvalidTarget);
}

TEST_CASE("game: validate_play_action pins sha limit and corner order")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.add_player("d", 3, 4);
    g.give("a", "sha", "s#1");
    g.give("a", "lesi", "L#0");
    g.cards.add_to_judge("b", Card{"L#9", "lesi", Suit::Spade, 6});

    const CardDef &sha = *g.catalog.find("sha").unwrap();
    const CardDef &lesi = *g.catalog.find("lesi").unwrap();
    const Card sha_card = g.cards.hand("a")[0];
    const Card lesi_card = g.cards.hand("a")[1];

    const TurnContext used_up{"a", 1, 1};  // 本回合杀次数已用尽
    const TurnContext fresh{"a", 0, 1};

    // 杀超次数：直接拒绝
    auto r = validate_play_action(g.ctx, "a", sha, sha_card, {"b"}, used_up);
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::ShaLimitExceeded);

    // 角顺序：杀超次数与目标出范围同现，仍先报次数
    r = validate_play_action(g.ctx, "a", sha, sha_card, {"c"}, used_up);
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::ShaLimitExceeded);

    // 未超限：目标出范围报目标错误
    r = validate_play_action(g.ctx, "a", sha, sha_card, {"c"}, fresh);
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::OutOfRange);

    // 延时：出 scope 的目标
    r = validate_play_action(g.ctx, "a", lesi, lesi_card, {"a"}, fresh);
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::InvalidTarget);

    // 延时：同名去重
    r = validate_play_action(g.ctx, "a", lesi, lesi_card, {"b"}, fresh);
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::DelayedDuplicate);
}

// ── 回合流程 ──────────────────────────────────────────────────────────

TEST_CASE("game: turn draws two and trims to hand limit")
{
    TestGame g("deck");
    g.add_player("a", 0, 3);  // 手牌上限 3
    g.add_player("b", 1, 4);
    g.cards.build_deck(g.catalog);
    g.give("a", "sha", "s#1");
    g.give("a", "sha", "s#2");
    g.give("a", "shan", "s#3");

    TestDecider decider;  // 不出牌
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(g.cards.hand_size("a") == 3);   // 3 + 摸2 = 5，弃到上限 3
    CHECK(g.cards.discard_size() == 2);   // 弃了 2 张
}

TEST_CASE("game: sha limit one per turn")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.cards.build_deck(g.catalog);
    g.give("a", "sha", "s#1");
    g.give("a", "sha", "s#2");

    TestDecider decider;
    decider.plays = {PlayAction{"s#1", {"b"}}, PlayAction{"s#2", {"b"}}};
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == TurnError::ShaLimitExceeded);
    CHECK(b->get_hp() == 3);            // 第一刀命中
    CHECK(g.cards.hand_size("a") == 3); // 2杀+摸2=4，打出1张剩3
}

TEST_CASE("game: liangnu lifts sha limit")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.cards.build_deck(g.catalog);
    g.equip("a", "liangnu", "e#0");
    g.give("a", "sha", "s#1");
    g.give("a", "sha", "s#2");

    TestDecider decider;
    decider.plays = {PlayAction{"s#1", {"b"}}, PlayAction{"s#2", {"b"}}};
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 2);            // 两刀全中
    CHECK(g.cards.hand_size("a") == 2); // 4 - 2 = 2（上限 4 不弃）
}

TEST_CASE("game: mid-turn liangnu allows further sha this turn")
{
    // 遵守回合上下文的决策源：引擎上报杀次数已用尽时不再提杀（真实 AI 的合法枚举行为）
    struct ContextBoundDecider : TestDecider
    {
        Option<PlayAction> choose_play(
            const GameContext &ctx, const TurnContext &turn) override
        {
            if (play_cursor >= plays.size())
                return Option<PlayAction>::None();
            const auto &next = plays[play_cursor];
            for (const auto &c : ctx.cards->hand(turn.player))
            {
                if (c.instance_id != next.instance_id)
                    continue;
                const auto def = ctx.catalog->find(c.def_id);
                if (def.is_some() && is_sha(*def.unwrap()) &&
                    turn.sha_played >= turn.sha_limit)
                    return Option<PlayAction>::None();
                break;
            }
            return Option<PlayAction>::Some(plays[play_cursor++]);
        }
    };

    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.cards.build_deck(g.catalog);
    g.give("a", "sha", "s#1");
    g.give("a", "liangnu", "e#0");
    g.give("a", "sha", "s#2");

    ContextBoundDecider decider;
    decider.plays = {
        PlayAction{"s#1", {"b"}},
        PlayAction{"e#0", {}},
        PlayAction{"s#2", {"b"}}
    };
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 2);            // 两刀全中：回合中途装连弩当回合生效
    CHECK(g.cards.hand_size("a") == 2); // 3 + 摸 2 - 打出 3 = 2
}

TEST_CASE("game: rules config drives draw and sha limit")
{
    TestGame g("deck");
    CHECK(g.ctx.rules == &g.rules);  // 对局上下文绑定本局规则
    g.rules.draw_per_turn = 3;
    g.rules.sha_limit = 2;

    g.add_player("a", 0, 5);
    auto *b = g.add_player("b", 1, 5);
    g.cards.build_deck(g.catalog);
    g.give("a", "sha", "s#1");
    g.give("a", "sha", "s#2");
    g.give("a", "sha", "s#3");

    TestDecider decider;
    decider.plays = {PlayAction{"s#1", {"b"}}, PlayAction{"s#2", {"b"}}};
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);             // 杀上限 2 → 两刀
    CHECK(g.cards.hand_size("a") == 4);  // 3 张杀打出 2 张 + 摸 3 张
}

TEST_CASE("game: simple ai obeys turn sha count without internal state")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    SimpleAI ai;
    TurnContext turn{"a", 0, 1};
    auto first = ai.choose_play(g.ctx, turn);
    REQUIRE(first.is_some());
    CHECK(first.unwrap().instance_id == "s#1");

    turn.sha_played = 1;  // 引擎已用尽本回合杀次数
    auto second = ai.choose_play(g.ctx, turn);
    CHECK(second.is_none());
}

TEST_CASE("game: equip weapon and replace same slot")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.cards.build_deck(g.catalog);
    g.give("a", "qinglong", "e#1");
    g.give("a", "qinggang", "e#2");

    TestDecider decider;
    decider.plays = {PlayAction{"e#1", {}}, PlayAction{"e#2", {}}};
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(g.cards.equip_size("a") == 1);              // 同槽位只留一件
    CHECK(g.cards.equip("a")[0].def_id == "qinggang");
    CHECK(g.cards.discard_size() >= 1);               // 被替换的武器已弃置
}

TEST_CASE("game: lesi non-heart skips play phase")
{
    TestGame g("deck");
    g.add_player("a", 0, 3);
    auto *b = g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");
    g.cards.add_to_judge("a", Card{"L#0", "lesi", Suit::Spade, 6});

    // 种牌堆：堆顶是判定牌（黑桃6 → 非红桃跳过出牌），其下两张供摸牌
    g.cards.add_to_draw(Card{"d#0", "sha", Suit::Club, 2});
    g.cards.add_to_draw(Card{"d#1", "shan", Suit::Diamond, 2});
    g.cards.add_to_draw(Card{"j#0", "sha", Suit::Spade, 6});

    TestDecider decider;
    decider.plays = {PlayAction{"s#1", {"b"}}};
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 4);            // 出牌阶段被跳过
    CHECK(g.cards.hand_size("a") == 3); // 杀 + 摸的 2 张
    CHECK(g.cards.judge_size("a") == 0);
}

TEST_CASE("game: lesi heart judge allows play")
{
    TestGame g("deck");
    g.add_player("a", 0, 3);
    auto *b = g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");
    g.cards.add_to_judge("a", Card{"L#0", "lesi", Suit::Spade, 6});

    g.cards.add_to_draw(Card{"d#0", "sha", Suit::Club, 2});
    g.cards.add_to_draw(Card{"d#1", "shan", Suit::Diamond, 2});
    g.cards.add_to_draw(Card{"j#0", "sha", Suit::Heart, 3});

    TestDecider decider;
    decider.plays = {PlayAction{"s#1", {"b"}}};
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);            // 杀正常打出
    CHECK(g.cards.hand_size("a") == 2); // 杀打出，剩摸的 2 张
}

TEST_CASE("game: lesi can be played onto another player's judge zone")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.cards.build_deck(g.catalog);
    g.give("a", "lesi", "L#0");

    TestDecider decider;
    decider.plays = {PlayAction{"L#0", {"b"}}};
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(g.cards.judge_size("b") == 1);
    CHECK(g.cards.judge("b")[0].def_id == "lesi");
    CHECK(g.cards.hand_size("a") == 2);  // 打出 1 张 + 摸 2 张
}

TEST_CASE("game: shandian is placed on self")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.cards.build_deck(g.catalog);
    g.give("a", "shandian", "S#0");

    TestDecider decider;
    decider.plays = {PlayAction{"S#0", {"a"}}};
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(g.cards.judge_size("a") == 1);
    CHECK(g.cards.judge("a")[0].def_id == "shandian");
}

TEST_CASE("game: duplicate delayed trick is rejected")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.cards.build_deck(g.catalog);
    g.give("a", "lesi", "L#0");
    g.cards.add_to_judge("b", Card{"L#9", "lesi", Suit::Spade, 6});

    TestDecider decider;
    decider.plays = {PlayAction{"L#0", {"b"}}};
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == TurnError::DelayedDuplicate);
}

TEST_CASE("game: delayed legal targets respect scope and dedup")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);

    const CardDef &shandian = *g.catalog.find("shandian").unwrap();
    CHECK((delayed_legal_targets(g.ctx, "a", shandian) ==
           std::vector<std::string>{"a"}));

    const CardDef &lesi = *g.catalog.find("lesi").unwrap();
    CHECK((delayed_legal_targets(g.ctx, "a", lesi) ==
           std::vector<std::string>{"b", "c"}));

    // 判定区已有同名延时锦囊的目标被排除
    g.cards.add_to_judge("b", Card{"L#0", "lesi", Suit::Spade, 6});
    CHECK((delayed_legal_targets(g.ctx, "a", lesi) ==
           std::vector<std::string>{"c"}));
}

TEST_CASE("game: delayed trick nullified at placement is discarded")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.cards.build_deck(g.catalog);
    g.give("a", "lesi", "L#0");
    g.give("b", "wuxie", "W#0");

    TestDecider decider;
    decider.counter = true;
    decider.plays = {PlayAction{"L#0", {"b"}}};
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(g.cards.judge_size("b") == 0);  // 未进入判定区
    CHECK(g.cards.hand_size("b") == 0);   // 无懈被消耗
}

TEST_CASE("game: delayed trick can be nullified at resolution")
{
    TestGame g("deck");
    auto *a = g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.cards.add_to_judge("a", Card{"L#0", "lesi", Suit::Spade, 6});
    g.give("a", "sha", "s#1");
    g.give("a", "wuxie", "W#0");
    g.cards.add_to_draw(Card{"d#0", "shan", Suit::Diamond, 2});
    g.cards.add_to_draw(Card{"d#1", "shan", Suit::Diamond, 3});

    TestDecider decider;
    decider.counter = true;  // 用无懈抵消自己的乐不思蜀
    decider.plays = {PlayAction{"s#1", {"b"}}};
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(a->get_hp() == 4);
    CHECK(b->get_hp() == 3);             // 出牌阶段未被跳过
    CHECK(g.cards.judge_size("a") == 0);
}

TEST_CASE("game: lightning strikes on spade 2-9")
{
    TestGame g("deck");
    auto *a = g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.cards.add_to_judge("a", Card{"L#0", "shandian", Suit::Spade, 1});

    g.cards.add_to_draw(Card{"d#0", "sha", Suit::Club, 2});
    g.cards.add_to_draw(Card{"d#1", "shan", Suit::Diamond, 2});
    g.cards.add_to_draw(Card{"j#0", "sha", Suit::Spade, 8});

    TestDecider decider;
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(a->get_hp() == 1);            // 雷伤 3
    CHECK(g.cards.judge_size("a") == 0);
    CHECK(g.cards.judge_size("b") == 0);
}

TEST_CASE("game: lightning damage has no source")
{
    TestGame g("deck");
    auto *a = g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.cards.add_to_judge("a", Card{"L#0", "shandian", Suit::Spade, 1});

    g.cards.add_to_draw(Card{"d#0", "sha", Suit::Club, 2});
    g.cards.add_to_draw(Card{"d#1", "shan", Suit::Diamond, 2});
    g.cards.add_to_draw(Card{"j#0", "sha", Suit::Spade, 8});

    std::vector<std::string> sources;
    auto h = g.bus.subscribe(tkw::Handler<tkw::EntityDamagedEvent>(
        [&](tkw::HandlerContext<tkw::EntityDamagedEvent> &c)
        { sources.push_back(c.event.source); }));

    TestDecider decider;
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    REQUIRE(sources.size() == 1);
    CHECK(sources[0].empty());  // 闪电为无来源伤害
    CHECK(a->get_hp() == 1);
}

TEST_CASE("game: lightning passes to next player")
{
    TestGame g("deck");
    auto *a = g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.cards.add_to_judge("a", Card{"L#0", "shandian", Suit::Spade, 1});

    g.cards.add_to_draw(Card{"d#0", "sha", Suit::Club, 2});
    g.cards.add_to_draw(Card{"d#1", "shan", Suit::Diamond, 2});
    g.cards.add_to_draw(Card{"j#0", "sha", Suit::Heart, 5});

    TestDecider decider;
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(a->get_hp() == 4);              // 未劈中
    CHECK(g.cards.judge_size("a") == 0);
    CHECK(g.cards.judge_size("b") == 1);  // 移到下家
    CHECK(g.cards.judge("b")[0].def_id == "shandian");
}

// ── 濒死与死亡 ──────────────────────────────────────────────────────

TEST_CASE("game: dying rescued by self peach")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    b->take_damage("a", 3, false);  // b: 4 → 1
    g.give("b", "tao", "t#0");
    g.give("a", "sha", "s#1");

    TestDecider decider;
    decider.save = true;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 1);                   // 0 → 桃救回 1
    CHECK(g.entities.find("b").is_some());     // 存活
    CHECK(g.cards.hand_size("b") == 0);        // 桃已消耗
}

TEST_CASE("game: dying rescued by another player's peach")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    b->take_damage("a", 3, false);  // b: 1
    g.give("c", "tao", "t#0");
    g.give("a", "sha", "s#1");

    TestDecider decider;
    decider.save = true;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 1);                   // 救回
    CHECK(g.entities.find("b").is_some());
    CHECK(g.cards.hand_size("c") == 0);        // c 的桃被消耗
}

TEST_CASE("game: unrescued death removes entity and rewards killer")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.cards.build_deck(g.catalog);
    auto b = g.entities.find("b").unwrap();
    b->take_damage("a", 3, false);  // b: 1
    g.give("b", "sha", "s#1");      // b 手牌，死后弃置
    g.give("a", "sha", "s#2");

    TestDecider decider;  // save=false
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(g.entities.find("b").is_none());        // 死亡移除
    CHECK(g.cards.hand_size("a") == 3);           // 击杀奖励摸 3
    CHECK(g.cards.discard_size() >= 2);           // a 的杀 + b 的手牌
}

TEST_CASE("game: death discards equipment and judgement zones")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    b->take_damage("a", 3, false);  // b: 1
    g.equip("b", "qinglong", "e#0");
    g.cards.add_to_judge("b", Card{"L#0", "lesi", Suit::Spade, 6});
    g.give("a", "sha", "s#2");

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(g.entities.find("b").is_none());
    CHECK(g.cards.equip_size("b") == 0);
    CHECK(g.cards.judge_size("b") == 0);
    CHECK(g.cards.discard_size() >= 2);  // 装备 + 判定牌
}

TEST_CASE("game: dying and died events published")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.entities.find("b").unwrap()->take_damage("a", 3, false);  // b: 1
    g.give("a", "sha", "s#2");

    int dying = 0;
    int died = 0;
    auto h1 = g.bus.subscribe(tkw::Handler<tkw::EntityDyingEvent>(
        [&](tkw::HandlerContext<tkw::EntityDyingEvent> &) { ++dying; }));
    auto h2 = g.bus.subscribe(tkw::Handler<tkw::EntityDiedEvent>(
        [&](tkw::HandlerContext<tkw::EntityDiedEvent> &) { ++died; }));

    TestDecider decider;  // 不救 → 死亡
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(dying == 1);
    CHECK(died == 1);

    // 救回时只发 Dying，不发 Died
    dying = 0;
    died = 0;
    g.add_player("c", 2, 4);
    g.entities.find("c").unwrap()->take_damage("a", 3, false);  // c: 1
    g.give("c", "tao", "t#0");
    g.give("a", "sha", "s#3");
    TestDecider saver;
    saver.save = true;
    const auto played2 = g.cards.hand("a")[0];
    auto r2 = resolve_play(g.ctx, saver, "a", played2, {"c"});
    REQUIRE(r2.is_ok());
    CHECK(dying == 1);
    CHECK(died == 0);
}

// ── 无懈可击 ─────────────────────────────────────────────────────────

TEST_CASE("game: wuxie cancels aoe effect on one target only")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    auto *c = g.add_player("c", 2, 4);
    g.give("a", "nanman", "n#0");
    g.give("b", "wuxie", "w#0");

    TestDecider decider;
    decider.counter = true;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b", "c"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 4);   // b 被无懈抵消
    CHECK(c->get_hp() == 3);   // c 无无懈 → 受伤
    CHECK(g.cards.hand_size("b") == 0);  // 无懈已消耗
}

TEST_CASE("game: wuxie cancels guohe so target keeps cards")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.give("a", "guohe", "g#0");
    g.give("b", "sha", "s#1");
    g.give("b", "wuxie", "w#0");

    TestDecider decider;
    decider.counter = true;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(g.cards.hand_size("b") == 1);  // 过拆被抵消，杀还在（无懈已打）
}

TEST_CASE("game: wuxie chain flips outcome (second wuxie counters first)")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.give("a", "guohe", "g#0");
    g.give("b", "sha", "s#1");
    g.give("b", "wuxie", "w#0");  // b 出无懈抵消过拆
    g.give("c", "wuxie", "w#1");  // c 再出无懈抵消 b 的无懈 → 过拆生效
    g.give("c", "tao", "t#0");

    TestDecider decider;
    decider.counter = true;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(g.cards.hand_size("b") == 0);  // b 的无懈与杀都被拆掉
    CHECK(g.cards.hand_size("c") == 1);  // c 只剩桃
}

TEST_CASE("game: tao is a basic card and cannot be countered")
{
    TestGame g("deck");
    auto *a = g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    a->take_damage("b", 2, false);  // a: 2
    g.give("a", "tao", "t#0");
    g.give("a", "wuxie", "w#0");

    TestDecider decider;
    decider.counter = true;
    const auto played = g.cards.hand("a")[0];  // 桃
    auto r = resolve_play(g.ctx, decider, "a", played, {"a"});
    REQUIRE(r.is_ok());
    CHECK(a->get_hp() == 3);   // 桃生效（基本牌不可无懈）
    CHECK(g.cards.hand_size("a") == 1);  // 无懈未被消耗
}

TEST_CASE("game: simple ai counters a delayed trick during judgement")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.cards.add_to_judge("a", Card{"L#0", "lesi", Suit::Spade, 6});
    g.give("a", "sha", "s#1");
    g.give("a", "wuxie", "w#0");

    // 判定牌（未被无懈时才会被抽）+ 摸牌两张，均非红桃
    g.cards.add_to_draw(Card{"d#0", "shan", Suit::Diamond, 3});
    g.cards.add_to_draw(Card{"d#1", "shan", Suit::Diamond, 2});
    g.cards.add_to_draw(Card{"d#2", "sha", Suit::Spade, 6});

    SimpleAI ai;
    auto r = execute_turn(g.ctx, ai, "a");
    REQUIRE(r.is_ok());
    CHECK(g.cards.judge_size("a") == 0);  // 无懈抵消，乐不思蜀被弃置
    CHECK(b->get_hp() == 3);             // 出牌阶段未跳过，杀命中
    CHECK(g.cards.hand_size("a") == 2);  // 无懈已消费，仅剩摸的 2 张
}

// ── 对局主循环 ───────────────────────────────────────────────────────

TEST_CASE("game: next_player wraps and skips removed players")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.add_player("d", 3, 4);
    CHECK(next_player(g.ctx, "a") == "b");
    CHECK(next_player(g.ctx, "d") == "a");
    g.entities.remove("b");
    CHECK(next_player(g.ctx, "a") == "c");  // b 已移除 → 跳过
    CHECK(next_player(g.ctx, "d") == "a");
}

TEST_CASE("game: prepare_game deals four initial cards to each")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    prepare_game(g.ctx, 4);
    CHECK(g.cards.hand_size("a") == 4);
    CHECK(g.cards.hand_size("b") == 4);
    CHECK(g.cards.draw_size() == 108 - 8);
    CHECK(alive_count(g.ctx) == 2);
}

TEST_CASE("game: same seed yields identical deal, different seed differs")
{
    auto setup = [](TestGame &g)
    {
        for (int s = 0; s < 4; ++s)
            g.add_player("p" + std::to_string(s), s, 4);
        prepare_game(g.ctx, 4);
    };

    TestGame a("deck", 7);
    TestGame b("deck", 7);
    TestGame c("deck", 8);
    setup(a);
    setup(b);
    setup(c);

    bool differs = false;
    for (int s = 0; s < 4; ++s)
    {
        const std::string id = "p" + std::to_string(s);
        CHECK(a.cards.hand(id) == b.cards.hand(id));  // 同 seed 逐张一致
        if (a.cards.hand(id) != c.cards.hand(id))
            differs = true;
    }
    CHECK(differs);  // 不同 seed 必须洗出不同结果
}

TEST_CASE("game: unsupported deck cards are reported")
{
    TestGame g("deck");
    // 标准牌堆全部卡（含装备能力）引擎已实现
    CHECK(unsupported_cards(g.catalog).empty());
}

TEST_CASE("game: effect traits are the single source of truth")
{
    using E = tkw::card::CardEffectKind;
    CHECK(is_settleable_kind(E::Damage));
    CHECK(is_settleable_kind(E::Heal));
    CHECK(is_settleable_kind(E::RevealPick));
    CHECK(is_settleable_kind(E::BorrowedSword));
    CHECK(!is_settleable_kind(E::Jink));  // 响应牌不可主动打出
    CHECK(!is_unimplemented_active_kind(E::BorrowedSword));
    CHECK(!is_unimplemented_active_kind(E::RevealPick));
    CHECK(!is_unimplemented_active_kind(E::Damage));
    CHECK(is_sha_kind(E::Damage));
    CHECK(!is_sha_kind(E::Duel));

    TestGame g("deck");
    auto sha = g.catalog.find("sha").unwrap();
    auto shan = g.catalog.find("shan").unwrap();
    auto juedou = g.catalog.find("juedou").unwrap();
    CHECK(is_sha(*sha));
    CHECK(!is_sha(*shan));
    CHECK(!is_sha(*juedou));
    CHECK(is_response_def(*sha, ResponseKind::Sha));
    CHECK(is_response_def(*shan, ResponseKind::Jink));
}

TEST_CASE("game: classify_action pins the play phase paths")
{
    TestGame g("deck");
    auto classify = [&](const char *id)
    {
        return classify_action(*g.catalog.find(id).unwrap());
    };
    CHECK(classify("sha") == PlayClass::Active);
    CHECK(classify("tao") == PlayClass::Active);
    CHECK(classify("wuzhong") == PlayClass::Active);
    CHECK(classify("shan") == PlayClass::Active);  // 有效果但不可主动结算
    CHECK(classify("liangnu") == PlayClass::Equipment);
    CHECK(classify("qinglong") == PlayClass::Equipment);
    CHECK(classify("lesi") == PlayClass::DelayedTrick);
    CHECK(classify("shandian") == PlayClass::DelayedTrick);
    CHECK(classify("wuxie") == PlayClass::None);
}

TEST_CASE("game: ability traits are the single source of truth")
{
    using A = tkw::card::Ability;
    CHECK(!is_unimplemented_ability(A::NoShaLimit));
    CHECK(!is_unimplemented_ability(A::IgnoreArmor));
    CHECK(!is_unimplemented_ability(A::ExtraShaAfterJink));
    CHECK(!is_unimplemented_ability(A::DiscardTwoForceDamage));
    CHECK(!is_unimplemented_ability(A::DiscardHorseOnDamage));
    CHECK(!is_unimplemented_ability(A::DamageAsDiscard));
    CHECK(!is_unimplemented_ability(A::JudgementJink));
    CHECK(!is_unimplemented_ability(A::BlackShaImmune));
    CHECK(!is_unimplemented_ability(A::MultiTargetSha));
    CHECK(!is_unimplemented_ability(A::Cixiong));
    CHECK(!is_unimplemented_ability(A::TwoCardsAsSha));
}

TEST_CASE("game: draw emits CardDrawn per card")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.cards.build_deck(g.catalog);

    int drawn = 0;
    auto h = g.bus.subscribe(tkw::Handler<tkw::CardDrawnEvent>(
        [&](tkw::HandlerContext<tkw::CardDrawnEvent> &) { ++drawn; }));

    apply_draw(g.ctx, "a", 3);
    CHECK(drawn == 3);
    CHECK(g.cards.hand_size("a") == 3);
}

TEST_CASE("game: turn emits draw then discard events in order")
{
    TestGame g("deck");
    g.add_player("a", 0, 3);  // 手牌上限 3
    g.add_player("b", 1, 4);
    g.cards.build_deck(g.catalog);
    g.give("a", "sha", "s#1");
    g.give("a", "sha", "s#2");
    g.give("a", "shan", "s#3");

    std::vector<std::string> log;
    auto h1 = g.bus.subscribe(tkw::Handler<tkw::CardDrawnEvent>(
        [&](tkw::HandlerContext<tkw::CardDrawnEvent> &) { log.push_back("draw"); }));
    auto h2 = g.bus.subscribe(tkw::Handler<tkw::CardDiscardedEvent>(
        [&](tkw::HandlerContext<tkw::CardDiscardedEvent> &) { log.push_back("discard"); }));
    auto h3 = g.bus.subscribe(tkw::Handler<tkw::CardPlayedEvent>(
        [&](tkw::HandlerContext<tkw::CardPlayedEvent> &) { log.push_back("play"); }));

    TestDecider decider;  // 不出牌
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(log == std::vector<std::string>{"draw", "draw", "discard", "discard"});
}

TEST_CASE("game: playing a card emits CardPlayed")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    std::vector<std::string> played;
    auto h = g.bus.subscribe(tkw::Handler<tkw::CardPlayedEvent>(
        [&](tkw::HandlerContext<tkw::CardPlayedEvent> &c)
        { played.push_back(c.event.def_id); }));

    TestDecider decider;
    auto r = resolve_play(g.ctx, decider, "a", g.cards.hand("a")[0], {"b"});
    REQUIRE(r.is_ok());
    CHECK(played == std::vector<std::string>{"sha"});
    CHECK(b->get_hp() == 3);
}

TEST_CASE("game: play_game ends when one player kills the other")
{
    TestGame g("deck");
    g.add_player("a", 0, 1);
    g.add_player("b", 1, 1);
    g.give("a", "sha", "s#1");  // 发牌前给一张杀

    TestDecider decider;
    decider.plays = {PlayAction{"s#1", {"b"}}};
    auto r = play_game(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(r.unwrap().winner == "a");
    CHECK(r.unwrap().turns >= 1);
    CHECK(g.entities.find("b").is_none());   // b 已死亡移除
    CHECK(g.entities.find("a").is_some());
}

TEST_CASE("game: play_game with no players is an error")
{
    TestGame g("deck");
    TestDecider decider;
    auto r = play_game(g.ctx, decider, "a");
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == LoopError::NoPlayers);
}

TEST_CASE("game: session starts, steps and reports over/winner")
{
    TestGame g("deck");
    g.add_player("a", 0, 1);
    g.add_player("b", 1, 1);
    g.give("a", "sha", "s#1");

    TestDecider decider;
    decider.plays = {PlayAction{"s#1", {"b"}}};

    GameSession session;
    REQUIRE(start_session(g.ctx, session, "a").is_ok());
    CHECK(session.started);
    CHECK(session.current == "a");
    CHECK(session.turns == 0);
    CHECK(!session_over(g.ctx));
    CHECK(session_winner(g.ctx).empty());

    auto r = step_session(g.ctx, decider, session);
    REQUIRE(r.is_ok());
    CHECK(session.turns == 1);
    CHECK(session_over(g.ctx));
    CHECK(session_winner(g.ctx) == "a");
}

TEST_CASE("game: player killed by lightning stops acting that turn")
{
    TestGame g("deck");
    g.add_player("a", 0, 3);
    auto *b = g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");  // 若未被终止，会打向 b
    g.cards.add_to_judge("a", Card{"L#0", "shandian", Suit::Spade, 1});
    g.cards.add_to_draw(Card{"j#0", "sha", Suit::Spade, 8});  // 判定：黑桃8

    TestDecider decider;
    decider.plays = {PlayAction{"s#1", {"b"}}};
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(g.entities.find("a").is_none());  // 雷伤 3 → 死亡
    CHECK(b->get_hp() == 4);                 // 未能继续出牌
    CHECK(g.cards.hand_size("a") == 0);      // 死亡清场
}

TEST_CASE("game: next_after_seat wraps and skips removed seats")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    CHECK(next_after_seat(g.ctx, 0) == "b");
    CHECK(next_after_seat(g.ctx, 1) == "c");
    CHECK(next_after_seat(g.ctx, 2) == "a");  // 环绕
    g.entities.remove("b");
    CHECK(next_after_seat(g.ctx, 0) == "c");  // 跳过已移除
    CHECK(next_after_seat(g.ctx, 2) == "a");
}

TEST_CASE("game: step_session advances past a player who died in their turn")
{
    TestGame g("deck");
    g.add_player("a", 0, 3);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.give("a", "sha", "s#1");
    g.cards.add_to_judge("a", Card{"L#0", "shandian", Suit::Spade, 1});
    g.cards.add_to_draw(Card{"j#0", "sha", Suit::Spade, 8});

    TestDecider decider;
    GameSession session;
    session.current = "a";
    session.started = true;
    auto r = step_session(g.ctx, decider, session);
    REQUIRE(r.is_ok());
    CHECK(g.entities.find("a").is_none());
    CHECK(session.current == "b");  // 下一位 = 座位 1
}

// ── 合法动作生成 ─────────────────────────────────────────────────────

TEST_CASE("game: legal_actions are all accepted by the engine")
{
    auto build = [](TestGame &g)
    {
        g.add_player("a", 0, 4);
        g.add_player("b", 1, 4);
        g.add_player("c", 2, 4);
        g.give("a", "sha", "s#1");
        g.give("a", "guohe", "g#1");
        g.give("a", "shunshou", "ss#1");
        g.give("a", "jiedao", "j#1");
        g.give("a", "lesi", "l#1");
        g.give("a", "liangnu", "e#1");
        g.equip("b", "qinglong", "eb#1");
        g.give("b", "sha", "bs#1");
    };

    TestGame g("deck");
    build(g);
    const TurnContext turn{"a", 0, 1};
    const auto acts = legal_actions(g.ctx, "a", turn);
    REQUIRE(!acts.empty());

    for (const auto &act : acts)
    {
        TestGame fresh("deck");
        build(fresh);
        TestDecider d;
        d.plays = {PlayAction{act.card.instance_id, act.targets}};
        auto r = execute_turn(fresh.ctx, d, "a");
        INFO("card=" << act.card.def_id);
        CHECK(r.is_ok());
    }

    // 方天画戟多目标场景：杀为最后一张手牌（未建牌堆、无摸牌，
    // 出牌阶段手牌数不变，多目标动作回放时仍合法）
    auto build_fangtian = [](TestGame &g)
    {
        g.add_player("a", 0, 4);
        g.add_player("b", 1, 4);
        g.add_player("c", 2, 4);
        g.equip("a", "fangtian", "e#0");
        g.give("a", "sha", "s#1");
    };

    TestGame f("deck");
    build_fangtian(f);
    const auto f_acts = legal_actions(f.ctx, "a", TurnContext{"a", 0, 1});
    bool saw_multi = false;
    for (const auto &act : f_acts)
    {
        TestGame fresh("deck");
        build_fangtian(fresh);
        TestDecider d;
        d.plays = {PlayAction{act.card.instance_id, act.targets}};
        auto r = execute_turn(fresh.ctx, d, "a");
        INFO("card=" << act.card.def_id << " targets=" << act.targets.size());
        CHECK(r.is_ok());
        if (act.targets.size() > 1)
            saw_multi = true;
    }
    CHECK(saw_multi);

    // 丈八蛇矛两张当杀场景：手牌两张非杀（未建牌堆、无摸牌，
    // 出牌阶段手牌数不变，虚拟杀回放时仍合法）
    auto build_zhangba = [](TestGame &g)
    {
        g.add_player("a", 0, 4);
        g.add_player("b", 1, 4);
        g.add_player("c", 2, 4);
        g.equip("a", "zhangba", "e#0");
        g.give("a", "wuzhong", "x#1");
        g.give("a", "tao", "x#2");
    };

    TestGame z("deck");
    build_zhangba(z);
    const auto z_acts = legal_actions(z.ctx, "a", TurnContext{"a", 0, 1});
    bool saw_virtual = false;
    for (const auto &act : z_acts)
    {
        TestGame fresh("deck");
        build_zhangba(fresh);
        TestDecider d;
        d.plays = {
            PlayAction{act.card.instance_id, act.targets, act.second_instance_id}};
        auto r = execute_turn(fresh.ctx, d, "a");
        INFO("card=" << act.card.def_id << " targets=" << act.targets.size());
        CHECK(r.is_ok());
        if (!act.second_instance_id.empty())
            saw_virtual = true;
    }
    CHECK(saw_virtual);
}

TEST_CASE("game: legal_actions excludes sha when turn limit reached")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    const TurnContext turn{"a", 1, 1};
    CHECK(legal_actions(g.ctx, "a", turn).empty());
}

TEST_CASE("game: legal_actions empty for empty hand")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    CHECK(legal_actions(g.ctx, "a", TurnContext{"a", 0, 1}).empty());
}

// ── 杀结算：装备效果 ─────────────────────────────────────────────────

TEST_CASE("game: renwang blocks black sha")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.equip("b", "renwang", "e#0");
    g.give("a", "sha", "s#1");  // copies[0] = 黑桃7（黑杀）

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 4);  // 黑杀无效
}

TEST_CASE("game: renwang does not block red sha")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.equip("b", "renwang", "e#0");
    g.cards.add_to_hand("a", Card{"s#1", "sha", Suit::Heart, 10});  // 红杀

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);  // 红杀命中
}

TEST_CASE("game: qinggang pierces renwang")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.equip("a", "qinggang", "e#0");
    g.equip("b", "renwang", "e#1");
    g.give("a", "sha", "s#1");  // 黑杀

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);  // 青釭剑无视防具
}

TEST_CASE("game: bagua red judge counts as jink")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.equip("b", "bagua", "e#0");
    g.give("a", "sha", "s#1");
    g.cards.add_to_draw(Card{"j#0", "sha", Suit::Heart, 3});  // 判定：红桃

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 4);  // 红判定视为闪，未受伤
}

TEST_CASE("game: bagua black judge does not dodge")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.equip("b", "bagua", "e#0");
    g.give("a", "sha", "s#1");
    g.cards.add_to_draw(Card{"j#0", "sha", Suit::Spade, 6});  // 判定：黑桃

    TestDecider decider;  // 不打闪
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);  // 黑判定无效，又无闪 → 受伤
}

TEST_CASE("game: guanshi discards two cards to force the sha")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.equip("a", "guanshi", "e#0");
    g.give("a", "sha", "s#1");
    g.give("a", "sha", "s#2");
    g.give("a", "shan", "s#3");
    g.give("b", "shan", "s#4");

    TestDecider decider;
    decider.respond = true;    // 目标打出闪
    decider.triggers = {Ability::DiscardTwoForceDamage};
    const auto played = g.cards.hand("a")[0];  // 杀
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);  // 贯石斧令杀依然命中
    CHECK(g.cards.hand_size("a") == 0);  // 弃了两张手牌
}

TEST_CASE("game: guanshi does not fire when the attacker holds fewer than two cards")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.equip("a", "guanshi", "e#0");
    g.give("a", "sha", "s#1");  // 仅一张杀，弃牌代价付不起
    g.give("b", "shan", "s#4");

    TestDecider decider;
    decider.respond = true;   // 目标打出闪
    decider.triggers = {Ability::DiscardTwoForceDamage};
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 4);          // 贯石斧不发动，闪生效
    CHECK(g.cards.hand_size("a") == 0);  // 未弃任何牌
}

TEST_CASE("game: qilin discards target horse after damage")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.equip("a", "qilin", "e#0");
    g.equip("b", "chitu", "e#1");
    g.give("a", "sha", "s#1");

    TestDecider decider;
    decider.triggers = {Ability::DiscardHorseOnDamage};
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);              // 命中
    CHECK(g.cards.equip_size("b") == 0);  // 坐骑被弃
}

TEST_CASE("game: hanbing converts damage into discarding two cards")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.equip("a", "hanbing", "e#0");
    g.give("a", "sha", "s#1");
    g.give("b", "sha", "s#2");
    g.give("b", "shan", "s#3");

    TestDecider decider;
    decider.triggers = {Ability::DamageAsDiscard};
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 4);         // 防止了伤害
    CHECK(g.cards.hand_size("b") == 0);  // 改为弃两张牌
}

TEST_CASE("game: hanbing does not prevent damage when the target holds fewer than two cards")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.equip("a", "hanbing", "e#0");
    g.give("a", "sha", "s#1");
    g.give("b", "sha", "s#2");  // 目标仅一张牌，弃牌代价付不起

    TestDecider decider;
    decider.triggers = {Ability::DamageAsDiscard};
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);          // 免伤不成立，伤害照常
    CHECK(g.cards.hand_size("b") == 1);  // 未弃任何牌
}

TEST_CASE("game: hanbing does not prevent damage when the cost cannot be fully discarded")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.equip("a", "hanbing", "e#0");
    g.give("a", "sha", "s#1");
    g.give("b", "shan", "s#2");
    g.cards.add_to_judge("b", Card{"j#1", "shan", Suit::Spade, 5});  // 可选总数 2，预检通过

    TestDecider decider;  // pick_card_from_target 只看手牌：第 2 次选不满
    decider.triggers = {Ability::DamageAsDiscard};
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);            // 只弃了 1 张，不免伤
    CHECK(g.cards.hand_size("b") == 0);  // 手牌已弃
    CHECK(g.cards.judge_size("b") == 1);  // 判定区未动（部分代价不回滚）
}

TEST_CASE("game: qinglong follows up with another sha after jink")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.equip("a", "qinglong", "e#0");
    g.give("a", "sha", "s#1");
    g.give("a", "sha", "s#2");
    g.give("b", "shan", "s#3");  // 目标只有一张闪

    TestDecider decider;
    decider.respond = true;   // 第一刀被闪，第二刀无闪可出
    decider.triggers = {Ability::ExtraShaAfterJink};
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);            // 续杀命中
    CHECK(g.cards.hand_size("a") == 0); // 两张杀都打出去了
    CHECK(g.cards.hand_size("b") == 0); // 闪已消耗
}

TEST_CASE("game: fangtian adds extra sha targets when it is the last hand card")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    auto *c = g.add_player("c", 2, 4);
    auto *d = g.add_player("d", 3, 4);
    g.equip("a", "fangtian", "e#0");
    g.give("a", "sha", "s#1");  // 唯一手牌，杀是最后一张

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b", "c", "d"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);
    CHECK(c->get_hp() == 3);
    CHECK(d->get_hp() == 3);
    CHECK(g.cards.hand_size("a") == 0);
}

TEST_CASE("game: fangtian does not add targets when the sha is not the last hand card")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    auto *c = g.add_player("c", 2, 4);
    g.equip("a", "fangtian", "e#0");
    g.give("a", "sha", "s#1");
    g.give("a", "shan", "s#2");  // 杀不是最后一张手牌

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b", "c"});
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::InvalidTarget);
    CHECK(b->get_hp() == 4);
    CHECK(c->get_hp() == 4);
    CHECK(g.cards.hand_size("a") == 2);  // 校验失败，杀未消耗

    // 单目标仍合法
    auto r2 = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r2.is_ok());
    CHECK(b->get_hp() == 3);
    CHECK(c->get_hp() == 4);
}

TEST_CASE("game: simple ai uses fangtian multi-target sha in its turn")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    auto *c = g.add_player("c", 2, 4);
    auto *d = g.add_player("d", 3, 4);
    g.equip("a", "fangtian", "e#0");
    g.give("a", "guohe", "g#1");
    g.give("a", "sha", "s#1");
    g.give("d", "shan", "ds#1");  // 让过拆先打出，杀成为最后一张手牌

    SimpleAI ai;
    auto r = execute_turn(g.ctx, ai, "a");
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);
    CHECK(c->get_hp() == 3);
    CHECK(d->get_hp() == 3);  // 三目标各中一刀
    CHECK(g.cards.hand_size("a") == 0);
}

TEST_CASE("game: fangtian rejects more targets than the extra allowance")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.add_player("d", 3, 4);
    g.add_player("e", 4, 4);
    g.equip("a", "fangtian", "e#0");
    g.give("a", "sha", "s#1");

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b", "c", "d", "e"});
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::InvalidTarget);
    CHECK(g.cards.hand_size("a") == 1);  // 校验失败，杀未消耗
}

TEST_CASE("game: fangtian does not relax non-damage one-other target count")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    auto *c = g.add_player("c", 2, 4);
    g.equip("a", "fangtian", "e#0");
    g.give("a", "guohe", "g#1");  // 唯一手牌，非杀类 OneOther（discard_target）
    g.give("b", "shan", "b#1");
    g.give("c", "shan", "c#1");

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b", "c"});
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == EffectError::InvalidTarget);
    CHECK(g.cards.hand_size("a") == 1);  // 校验失败，拆桥未消耗

    // 单目标仍合法：放宽只作用于杀，普通 OneOther 维持一目标上限
    auto r2 = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r2.is_ok());
    CHECK(g.cards.hand_size("a") == 0);
    CHECK(g.cards.hand_size("b") == 0);  // 目标被拆走一张
    CHECK(c->get_hp() == 4);
}

TEST_CASE("game: cixiong makes the opposite gender target discard")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4, Gender::Female);
    g.equip("a", "cixiong", "e#0");
    g.give("a", "sha", "s#1");
    g.give("b", "wuzhong", "b#1");  // 目标一张手牌（非闪）

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);             // 杀命中
    CHECK(g.cards.hand_size("b") == 0);  // 目标弃置一张
    CHECK(g.cards.hand_size("a") == 0);  // 使用者未摸牌
}

TEST_CASE("game: cixiong draws when the target cannot discard")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4, Gender::Female);
    g.equip("a", "cixiong", "e#0");
    g.give("a", "sha", "s#1");
    g.cards.build_deck(g.catalog);  // 摸牌堆有牌可摸

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);
    CHECK(g.cards.hand_size("a") == 1);  // 使用者摸一张
    CHECK(g.cards.hand_size("b") == 0);
}

TEST_CASE("game: cixiong does not trigger against the same gender")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);  // 与 a 同性（默认男）
    g.equip("a", "cixiong", "e#0");
    g.give("a", "sha", "s#1");
    g.give("b", "wuzhong", "b#1");

    TestDecider decider;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);
    CHECK(g.cards.hand_size("b") == 1);  // 不弃牌
    CHECK(g.cards.hand_size("a") == 0);  // 不摸牌
}

TEST_CASE("game: zhangba uses two hand cards as a sha")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");

    std::vector<std::string> played;
    auto h = g.bus.subscribe(tkw::Handler<tkw::CardPlayedEvent>(
        [&](tkw::HandlerContext<tkw::CardPlayedEvent> &c)
        { played.push_back(c.event.def_id); }));

    TestDecider decider;
    decider.plays = {PlayAction{"x#1", {"b"}, "x#2"}};
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);            // 虚拟杀命中
    CHECK(g.cards.hand_size("a") == 0); // 两张牌都消耗
    CHECK(g.cards.discard_size() == 2);
    CHECK(played == std::vector<std::string>{"wuzhong", "tao"});
}

TEST_CASE("game: zhangba virtual sha is not black")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.equip("b", "renwang", "e#1");
    g.cards.add_to_hand("a", Card{"x#1", "wuzhong", Suit::Spade, 5});  // 两张黑色牌
    g.cards.add_to_hand("a", Card{"x#2", "tao", Suit::Club, 6});

    TestDecider decider;
    decider.plays = {PlayAction{"x#1", {"b"}, "x#2"}};
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);  // 虚拟杀无花色，仁王盾黑杀判定不适用
}

TEST_CASE("game: zhangba virtual sha requires the equipped weapon")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");  // 未装备丈八蛇矛

    TestDecider decider;
    decider.plays = {PlayAction{"x#1", {"b"}, "x#2"}};
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == TurnError::PlayRejected);
    CHECK(b->get_hp() == 4);
    CHECK(g.cards.hand_size("a") == 2);  // 校验失败，不消耗
}

TEST_CASE("game: zhangba virtual sha counts toward the sha limit")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");
    g.give("a", "nanman", "x#3");
    g.give("a", "shan", "x#4");

    TestDecider decider;
    decider.plays = {
        PlayAction{"x#1", {"b"}, "x#2"},
        PlayAction{"x#3", {"b"}, "x#4"},  // 本回合第二个「杀」：超限
    };
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == TurnError::ShaLimitExceeded);
    CHECK(b->get_hp() == 3);         // 第一个虚拟杀命中
    CHECK(g.cards.hand_size("a") == 2);  // 第二个未消耗
}

TEST_CASE("game: zhangba plus fangtian allows multi-target when the pair is the whole hand")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    auto *c = g.add_player("c", 2, 4);
    auto *d = g.add_player("d", 3, 4);
    // 夹具直接叠两件武器（单槽约束属装备流程，非本用例关注点）
    g.equip("a", "zhangba", "e#0");
    g.equip("a", "fangtian", "e#1");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");  // pair 即最后两张手牌

    TestDecider decider;
    decider.plays = {PlayAction{"x#1", {"b", "c", "d"}, "x#2"}};
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);
    CHECK(c->get_hp() == 3);
    CHECK(d->get_hp() == 3);
    CHECK(g.cards.hand_size("a") == 0);
}

TEST_CASE("game: zhangba plus fangtian does not add targets when the pair is not the whole hand")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    auto *c = g.add_player("c", 2, 4);
    g.equip("a", "zhangba", "e#0");
    g.equip("a", "fangtian", "e#1");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");
    g.give("a", "shan", "x#3");  // pair 不是最后两张手牌

    TestDecider decider;
    decider.plays = {PlayAction{"x#1", {"b", "c"}, "x#2"}};
    auto r = execute_turn(g.ctx, decider, "a");
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == TurnError::InvalidTarget);
    CHECK(b->get_hp() == 4);
    CHECK(g.cards.hand_size("a") == 3);  // 校验失败，不消耗

    // 单目标仍合法
    TestDecider decider2;
    decider2.plays = {PlayAction{"x#1", {"b"}, "x#2"}};
    auto r2 = execute_turn(g.ctx, decider2, "a");
    REQUIRE(r2.is_ok());
    CHECK(b->get_hp() == 3);
    CHECK(c->get_hp() == 4);
    CHECK(g.cards.hand_size("a") == 1);
}

TEST_CASE("game: legal_actions enumerates zhangba pairs only without a real sha")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");

    const auto acts = legal_actions(g.ctx, "a", TurnContext{"a", 0, 1});
    bool saw_zhangba = false;
    for (const auto &a : acts)
        if (!a.second_instance_id.empty())
        {
            saw_zhangba = true;
            CHECK(a.card.instance_id == "x#1");
            CHECK(a.second_instance_id == "x#2");
            CHECK(a.targets.size() == 1);
        }
    CHECK(saw_zhangba);

    // 手牌含真杀 → 不产出两张当杀动作
    g.give("a", "sha", "s#1");
    const auto acts2 = legal_actions(g.ctx, "a", TurnContext{"a", 0, 1});
    for (const auto &a : acts2)
        CHECK(a.second_instance_id.empty());
}

TEST_CASE("game: simple ai uses zhangba before other tricks when holding no sha")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");  // 无中生有也可打，但杀优先
    g.give("a", "tao", "x#2");

    SimpleAI ai;
    auto r = execute_turn(g.ctx, ai, "a");
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);  // 两张牌当杀打出，而非先打无中生有
    CHECK(g.cards.hand_size("a") == 0);
}

// ── 杀响应窗口：丈八蛇矛两张手牌当杀（响应侧）──────────────────────────

TEST_CASE("game: zhangba answers a duel sha with two hand cards")
{
    TestGame g("deck");
    g.rules.duel_rounds = 3;  // 截断后续空轮
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.give("a", "juedou", "j#0");
    g.give("a", "sha", "s#1");
    g.equip("b", "zhangba", "e#0");
    g.give("b", "wuzhong", "x#1");
    g.give("b", "tao", "x#2");  // b 无真杀：两张手牌当杀

    TestDecider decider;
    decider.respond = true;
    const auto played = g.cards.hand("a")[0];  // 决斗
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);           // b 首张当杀免伤，次轮才受击
    CHECK(g.cards.hand_size("b") == 0); // 两张牌已消耗
    CHECK(g.cards.hand_size("a") == 0); // 决斗牌 + 真杀
}

TEST_CASE("game: zhangba answers a borrowed sword with a virtual sha")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    auto *c = g.add_player("c", 2, 4);
    g.equip("b", "zhangba", "e#0");  // b 的武器即丈八蛇矛
    g.give("a", "jiedao", "j#0");
    g.give("b", "wuzhong", "x#1");
    g.give("b", "tao", "x#2");  // b 无真杀：两张手牌当杀对 c

    TestDecider decider;
    decider.respond = true;
    const auto played = g.cards.hand("a")[0];  // 借刀杀人 {b, c}
    auto r = resolve_play(g.ctx, decider, "a", played, {"b", "c"});
    REQUIRE(r.is_ok());
    CHECK(c->get_hp() == 3);            // b 的虚拟杀命中 c
    CHECK(g.cards.hand_size("b") == 0);  // 两张牌已消耗
    CHECK(g.cards.equip_size("b") == 1); // 武器仍在
    CHECK(g.cards.hand_size("a") == 0);  // 未夺回武器
}

TEST_CASE("game: zhangba answers a borrowed sword on itself")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.equip("b", "zhangba", "e#0");  // b 的武器即丈八蛇矛
    g.give("a", "jiedao", "j#0");
    g.give("b", "wuzhong", "x#1");
    g.give("b", "tao", "x#2");  // b 无真杀：两张手牌当杀，目标即自己

    TestDecider decider;
    decider.respond = true;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b", "b"});  // A=B=b
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);             // 虚拟杀命中自身
    CHECK(g.cards.hand_size("b") == 0); // 两张牌已消耗
    CHECK(g.cards.equip_size("b") == 1); // 武器仍在
    CHECK(g.cards.hand_size("a") == 0);  // 未夺回武器
}

TEST_CASE("game: zhangba response sha can still be jinked")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    auto *c = g.add_player("c", 2, 4);
    g.equip("b", "zhangba", "e#0");
    g.give("a", "jiedao", "j#0");
    g.give("b", "wuzhong", "x#1");
    g.give("b", "tao", "x#2");
    g.give("c", "shan", "c#1");  // c 可闪

    TestDecider decider;
    decider.respond = true;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b", "c"});
    REQUIRE(r.is_ok());
    CHECK(c->get_hp() == 4);           // 虚拟杀被闪
    CHECK(g.cards.hand_size("c") == 0); // 闪已消耗
    CHECK(g.cards.hand_size("b") == 0); // 响应两张牌照消耗（被闪不回退）
}

TEST_CASE("game: zhangba response requires the equipped weapon")
{
    TestGame g("deck");
    g.rules.duel_rounds = 2;
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.give("a", "juedou", "j#0");
    g.give("a", "sha", "s#1");
    g.give("b", "wuzhong", "x#1");
    g.give("b", "tao", "x#2");  // 未装备丈八蛇矛：两张牌不是响应

    TestDecider decider;
    decider.respond = true;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 3);          // 无响应，受击
    CHECK(g.cards.hand_size("b") == 2);  // 两张牌未消耗
}

TEST_CASE("game: cixiong fires for the zhangba response virtual sha")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);  // 默认男
    auto *c = g.add_player("c", 2, 4, Gender::Female);
    // 夹具直接叠两件武器（单槽约束属装备流程，非本用例关注点）
    g.equip("b", "cixiong", "e#0");
    g.equip("b", "zhangba", "e#1");
    g.give("a", "jiedao", "j#0");
    g.give("b", "wuzhong", "x#1");
    g.give("b", "tao", "x#2");
    g.give("c", "wuzhong", "c#1");  // c 一张手牌（非闪）

    TestDecider decider;
    decider.respond = true;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b", "c"});
    REQUIRE(r.is_ok());
    CHECK(c->get_hp() == 3);            // 虚拟杀命中
    CHECK(g.cards.hand_size("c") == 0); // 雌雄令 c 弃一张（OnTarget 生效）
    CHECK(g.cards.hand_size("b") == 0); // 两张牌已消耗
}

TEST_CASE("game: zhangba answers a nanman sha with two hand cards")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    auto *b = g.add_player("b", 1, 4);
    g.equip("b", "zhangba", "e#0");
    g.give("a", "nanman", "n#0");
    g.give("b", "wuzhong", "x#1");
    g.give("b", "tao", "x#2");

    TestDecider decider;
    decider.respond = true;
    const auto played = g.cards.hand("a")[0];
    auto r = resolve_play(g.ctx, decider, "a", played, {"b"});
    REQUIRE(r.is_ok());
    CHECK(b->get_hp() == 4);           // 两张当杀免伤
    CHECK(g.cards.hand_size("b") == 0); // 两张牌已消耗
}

TEST_CASE("game: zhangba response pair candidates are all accepted by the engine")
{
    auto build = [](TestGame &g)
    {
        g.add_player("a", 0, 4);
        g.add_player("b", 1, 4);
        g.add_player("c", 2, 4);
        g.equip("b", "zhangba", "e#0");
        g.give("a", "jiedao", "j#0");
        g.give("b", "wuzhong", "x#1");
        g.give("b", "tao", "x#2");
        g.give("b", "shan", "x#3");
    };

    TestGame g("deck");
    build(g);
    const auto pairs = two_cards_as_sha_pairs(g.ctx, "b");
    REQUIRE(pairs.size() == 3);

    for (const auto &[first, second] : pairs)
    {
        TestGame fresh("deck");
        build(fresh);
        TestDecider d;
        d.response_id = first.instance_id;
        d.response_second_id = second.instance_id;
        const auto played = fresh.cards.hand("a")[0];
        auto r = resolve_play(fresh.ctx, d, "a", played, {"b", "c"});
        INFO("pair=" << first.instance_id << " + " << second.instance_id);
        CHECK(r.is_ok());
        CHECK(fresh.entities.find("c").unwrap()->get_hp() == 3);
        CHECK(fresh.cards.hand_size("b") == 1);
    }
}
