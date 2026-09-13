#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <string>

#include <pjh_json/document.hpp>
#include <pjh_platform/fs.hpp>

#include "card/catalog.hpp"
#include "card/def.hpp"
#include "config/error.hpp"
#include "config/resource.hpp"
#include "io/file.hpp"

namespace
{
    using namespace tkw::card;
    using tkw::card::detail::parse_card_def;
    using tkw::config::ConfigError;
    using tkw::config::ConfigErrorKind;

    std::filesystem::path temp_dir(const char *name)
    {
        const auto dir = pjh::platform::Fs::temp_directory() / name;
        std::filesystem::remove_all(dir);
        REQUIRE(pjh::platform::Fs::create_directories(dir).is_ok());
        return dir;
    }

    /** 解析内联 JSON 为 Document（move-only，按值返回）。 */
    pjh::json::Document doc(const std::string &text)
    {
        return pjh::json::parse_copy(text);
    }
}

TEST_CASE("card: parse_card_def basic card")
{
    const auto d = doc(R"({
        "id": "sha", "name": "杀", "type": "basic", "subtype": "attack",
        "copies": [ {"suit": "spade", "number": 7}, {"suit": "heart", "number": 10} ],
        "text": "出牌阶段……",
        "effect": {"kind": "damage", "amount": 1, "scope": "one_other", "response": "jink"}
    })");
    auto r = parse_card_def(d.root(), "sha");
    REQUIRE(r.is_ok());
    const auto &def = r.unwrap();
    CHECK(def.id == "sha");
    CHECK(def.name == "杀");
    CHECK(def.type == CardType::Basic);
    CHECK(def.subtype == "attack");
    CHECK(def.copies.size() == 2);
    CHECK(def.copies[0] == CardCopy{Suit::Spade, 7});
    REQUIRE(def.effect.is_some());
    CHECK(def.effect.unwrap().kind == CardEffectKind::Damage);
    CHECK(def.effect.unwrap().amount == 1);
    CHECK(def.effect.unwrap().scope.contains(Scope::OneOther));
    CHECK(def.effect.unwrap().response.contains(ResponseKind::Jink));
    CHECK(def.equip.is_none());
}

TEST_CASE("card: parse_card_def equipment with horse direction")
{
    const auto d = doc(R"({
        "id": "chitu", "name": "赤兔", "type": "equipment", "subtype": "horse",
        "copies": [ {"suit": "heart", "number": 5} ],
        "text": "坐骑·-1。锁定技，你与其他角色的距离-1。",
        "equip": {"slot": "offensive_horse"}
    })");
    auto r = parse_card_def(d.root(), "chitu");
    REQUIRE(r.is_ok());
    CHECK(r.unwrap().effect.is_none());
    REQUIRE(r.unwrap().equip.is_some());
    const auto &eq = r.unwrap().equip.unwrap();
    CHECK(eq.slot == EquipSlot::OffensiveHorse);
    CHECK(eq.range == 0);
}

TEST_CASE("card: parse_card_def weapon carries range and ability")
{
    const auto d = doc(R"({
        "id": "liangnu", "name": "诸葛连弩", "type": "equipment", "subtype": "weapon",
        "copies": [ {"suit": "club", "number": 1} ],
        "equip": {"slot": "weapon", "range": 1},
        "abilities": ["no_sha_limit"]
    })");
    auto r = parse_card_def(d.root(), "liangnu");
    REQUIRE(r.is_ok());
    REQUIRE(r.unwrap().equip.is_some());
    CHECK(r.unwrap().equip.unwrap().slot == EquipSlot::Weapon);
    CHECK(r.unwrap().equip.unwrap().range == 1);
    CHECK(r.unwrap().effect.is_none());
    REQUIRE(r.unwrap().abilities.size() == 1);
    CHECK(r.unwrap().abilities[0] == Ability::NoShaLimit);
}

TEST_CASE("card: judge descriptor parses")
{
    auto d = parse_card_def(
        doc(R"({
            "id": "shandian", "name": "闪电", "type": "trick", "subtype": "delayed",
            "copies": [ {"suit": "spade", "number": 1} ],
            "judge": {"trigger": "spade_2_9", "success": "damage",
                      "failure": "pass_to_next", "amount": 3,
                      "scope": "one_other"}
        })").root(), "shandian");
    REQUIRE(d.is_ok());
    REQUIRE(d.unwrap().judge.is_some());
    const auto &j = d.unwrap().judge.unwrap();
    CHECK(j.trigger == JudgeTrigger::Spade2to9);
    CHECK(j.success == JudgeAction::Damage);
    CHECK(j.failure == JudgeAction::PassToNext);
    CHECK(j.amount == 3);
    CHECK(j.scope.contains(Scope::OneOther));
}

TEST_CASE("card: bingliang judge descriptor parses not_club and skip_draw")
{
    auto d = parse_card_def(
        doc(R"({
            "id": "bingliang", "name": "兵粮寸断", "type": "trick", "subtype": "delayed",
            "copies": [ {"suit": "spade", "number": 10}, {"suit": "club", "number": 4} ],
            "judge": {"trigger": "not_club", "success": "skip_draw",
                      "scope": "one_other", "range": 1}
        })").root(), "bingliang");
    REQUIRE(d.is_ok());
    REQUIRE(d.unwrap().judge.is_some());
    const auto &j = d.unwrap().judge.unwrap();
    CHECK(j.trigger == JudgeTrigger::NotClub);
    CHECK(j.success == JudgeAction::SkipDraw);
    CHECK(j.failure == JudgeAction::Nothing);
    CHECK(j.amount == 0);
    CHECK(j.scope.contains(Scope::OneOther));
    CHECK(j.range == 1);
}

TEST_CASE("card: parse_card_def default set / missing optionals")
{
    const auto d = doc(R"({
        "id": "a", "name": "a", "type": "basic",
        "copies": [ {"suit": "diamond", "number": 13} ]
    })");
    auto r = parse_card_def(d.root(), "a");
    REQUIRE(r.is_ok());
    CHECK(r.unwrap().subtype.empty());
    CHECK(r.unwrap().effect.is_none());
    CHECK(r.unwrap().equip.is_none());
}

TEST_CASE("card: parse errors carry field paths")
{
    auto miss = parse_card_def(doc(R"({"name": "x"})").root(), "x");
    REQUIRE(miss.is_err());
    CHECK(miss.unwrap_err() == ConfigError{ConfigErrorKind::MissingField, "x.id"});

    auto bad_kind = parse_card_def(
        doc(R"({"id": "a", "name": "a", "type": "basic", "copies": [],
                 "effect": {"kind": "explode"}})").root(), "a");
    REQUIRE(bad_kind.is_err());
    CHECK(bad_kind.unwrap_err() == ConfigError{ConfigErrorKind::InvalidValue, "a.effect.kind"});

    auto bad_num = parse_card_def(
        doc(R"({"id": "a", "name": "a", "type": "basic",
                 "copies": [ {"suit": "spade", "number": 14} ]})").root(), "a");
    REQUIRE(bad_num.is_err());
    CHECK(bad_num.unwrap_err() == ConfigError{ConfigErrorKind::InvalidValue, "a.copies[0].number"});

    auto bad_suit = parse_card_def(
        doc(R"({"id": "a", "name": "a", "type": "basic",
                 "copies": [ {"suit": "star", "number": 1} ]})").root(), "a");
    REQUIRE(bad_suit.is_err());
    CHECK(bad_suit.unwrap_err() == ConfigError{ConfigErrorKind::InvalidValue, "a.copies[0].suit"});

    auto bad_scope = parse_card_def(
        doc(R"({"id": "a", "name": "a", "type": "basic", "copies": [],
                 "effect": {"kind": "damage", "amount": 1, "scope": "everyone"}})").root(), "a");
    REQUIRE(bad_scope.is_err());
    CHECK(bad_scope.unwrap_err() ==
          ConfigError{ConfigErrorKind::InvalidValue, "a.effect.scope"});
}

TEST_CASE("card: opt default must not swallow type mismatch")
{
    auto r = parse_card_def(
        doc(R"({"id": "a", "name": "a", "type": "basic", "copies": [],
                 "effect": {"kind": "damage", "amount": "x"}})").root(), "a");
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err() == ConfigError{ConfigErrorKind::TypeMismatch, "a.effect.amount"});
}

TEST_CASE("card: damage_type parses and defaults normal")
{
    auto fire = parse_card_def(
        doc(R"({"id": "h", "name": "火杀", "type": "basic", "copies": [],
                 "effect": {"kind": "damage", "amount": 1, "damage_type": "fire"}})")
            .root(),
        "h");
    REQUIRE(fire.is_ok());
    REQUIRE(fire.unwrap().effect.is_some());
    CHECK(fire.unwrap().effect.unwrap().damage_type == DamageType::Fire);

    auto dft = parse_card_def(
        doc(R"({"id": "s", "name": "杀", "type": "basic", "copies": [],
                 "effect": {"kind": "damage", "amount": 1}})").root(),
        "s");
    REQUIRE(dft.is_ok());
    CHECK(dft.unwrap().effect.unwrap().damage_type == DamageType::Normal);

    auto bad = parse_card_def(
        doc(R"({"id": "x", "name": "x", "type": "basic", "copies": [],
                 "effect": {"kind": "damage", "amount": 1, "damage_type": "ice"}})")
            .root(),
        "x");
    REQUIRE(bad.is_err());
    CHECK(bad.unwrap_err() ==
          ConfigError{ConfigErrorKind::InvalidValue, "x.effect.damage_type"});

    auto judge = parse_card_def(
        doc(R"({"id": "l", "name": "雷", "type": "trick", "subtype": "delayed",
                 "copies": [ {"suit": "spade", "number": 1} ],
                 "judge": {"trigger": "spade_2_9", "success": "damage", "amount": 3,
                           "damage_type": "thunder"}})").root(),
        "l");
    REQUIRE(judge.is_ok());
    REQUIRE(judge.unwrap().judge.is_some());
    CHECK(judge.unwrap().judge.unwrap().damage_type == DamageType::Thunder);
}

TEST_CASE("card: rescue/counter flags parse with false default")
{
    auto r = parse_card_def(
        doc(R"({"id": "tao", "name": "桃", "type": "basic", "subtype": "heal",
                 "copies": [ {"suit": "heart", "number": 3} ],
                 "effect": {"kind": "heal", "amount": 1, "scope": "self"},
                 "rescue": true})").root(),
        "tao");
    REQUIRE(r.is_ok());
    CHECK(r.unwrap().rescue);
    CHECK_FALSE(r.unwrap().counter);
    CHECK_FALSE(r.unwrap().self_rescue);

    auto jiu = parse_card_def(
        doc(R"({"id": "jiu", "name": "酒", "type": "basic", "subtype": "heal",
                 "copies": [ {"suit": "diamond", "number": 9} ],
                 "effect": {"kind": "analeptic", "scope": "self"},
                 "self_rescue": true})").root(),
        "jiu");
    REQUIRE(jiu.is_ok());
    CHECK(jiu.unwrap().self_rescue);
    CHECK_FALSE(jiu.unwrap().rescue);

    auto c = parse_card_def(
        doc(R"({"id": "wuxie", "name": "无懈可击", "type": "trick", "subtype": "instant",
                 "copies": [ {"suit": "spade", "number": 11} ],
                 "counter": true})").root(),
        "wuxie");
    REQUIRE(c.is_ok());
    CHECK(c.unwrap().counter);
    CHECK_FALSE(c.unwrap().rescue);
    CHECK(c.unwrap().effect.is_none());
}

TEST_CASE("card: effect field invariants and subtype are enforced")
{
    auto dmg = parse_card_def(
        doc(R"({"id": "a", "name": "a", "type": "basic", "copies": [],
                 "effect": {"kind": "damage"}})").root(), "a");
    REQUIRE(dmg.is_err());
    CHECK(dmg.unwrap_err() ==
          ConfigError{ConfigErrorKind::InvalidValue, "a.effect.amount"});

    auto draw = parse_card_def(
        doc(R"({"id": "a", "name": "a", "type": "trick", "copies": [],
                 "effect": {"kind": "draw"}})").root(), "a");
    REQUIRE(draw.is_err());
    CHECK(draw.unwrap_err() ==
          ConfigError{ConfigErrorKind::InvalidValue, "a.effect.count"});

    auto steal = parse_card_def(
        doc(R"({"id": "a", "name": "a", "type": "trick", "copies": [],
                 "effect": {"kind": "steal", "count": 1}})").root(), "a");
    REQUIRE(steal.is_err());
    CHECK(steal.unwrap_err() ==
          ConfigError{ConfigErrorKind::InvalidValue, "a.effect.range"});

    auto sub = parse_card_def(
        doc(R"({"id": "a", "name": "a", "type": "basic", "subtype": "nope",
                 "copies": []})").root(), "a");
    REQUIRE(sub.is_err());
    CHECK(sub.unwrap_err() == ConfigError{ConfigErrorKind::InvalidValue, "a.subtype"});
}

TEST_CASE("card: judge damage action requires a positive amount")
{
    // 成功动作为伤害但缺 amount（opt 回落 0）→ 不静默按 0 结算，加载期拒载
    auto miss = parse_card_def(
        doc(R"({"id": "x", "name": "x", "type": "trick", "subtype": "delayed",
                 "copies": [ {"suit": "spade", "number": 1} ],
                 "judge": {"trigger": "spade_2_9", "success": "damage"}})")
            .root(),
        "x");
    REQUIRE(miss.is_err());
    CHECK(miss.unwrap_err() ==
          ConfigError{ConfigErrorKind::InvalidValue, "x.judge.amount"});

    // amount 显式负数同样拒载
    auto neg = parse_card_def(
        doc(R"({"id": "x", "name": "x", "type": "trick", "subtype": "delayed",
                 "copies": [ {"suit": "spade", "number": 1} ],
                 "judge": {"trigger": "spade_2_9", "success": "damage",
                           "amount": -1}})")
            .root(),
        "x");
    REQUIRE(neg.is_err());
    CHECK(neg.unwrap_err() ==
          ConfigError{ConfigErrorKind::InvalidValue, "x.judge.amount"});
}

TEST_CASE("card: out-of-range numeric fields are rejected instead of narrowed")
{
    // 超出 int 可表示范围的 int64（收窄后会变成合法小数）必须在加载期拒载
    auto amount = parse_card_def(
        doc(R"({"id": "a", "name": "a", "type": "basic", "copies": [],
                 "effect": {"kind": "damage", "amount": 4294967297}})").root(), "a");
    REQUIRE(amount.is_err());
    CHECK(amount.unwrap_err() ==
          ConfigError{ConfigErrorKind::InvalidValue, "a.effect.amount"});

    auto count = parse_card_def(
        doc(R"({"id": "a", "name": "a", "type": "trick", "copies": [],
                 "effect": {"kind": "draw", "count": 4294967297}})").root(), "a");
    REQUIRE(count.is_err());
    CHECK(count.unwrap_err() ==
          ConfigError{ConfigErrorKind::InvalidValue, "a.effect.count"});

    auto range = parse_card_def(
        doc(R"({"id": "a", "name": "a", "type": "trick", "copies": [],
                 "effect": {"kind": "steal", "count": 1, "range": 4294967297}})")
            .root(),
        "a");
    REQUIRE(range.is_err());
    CHECK(range.unwrap_err() ==
          ConfigError{ConfigErrorKind::InvalidValue, "a.effect.range"});

    auto equip_range = parse_card_def(
        doc(R"({"id": "a", "name": "a", "type": "equipment", "subtype": "horse",
                 "copies": [],
                 "equip": {"slot": "weapon", "range": 4294967297}})").root(), "a");
    REQUIRE(equip_range.is_err());
    CHECK(equip_range.unwrap_err() ==
          ConfigError{ConfigErrorKind::InvalidValue, "a.equip.range"});

    auto judge_amount = parse_card_def(
        doc(R"({"id": "a", "name": "a", "type": "trick", "subtype": "delayed",
                 "copies": [],
                 "judge": {"trigger": "spade_2_9", "success": "damage",
                           "amount": 4294967297}})").root(), "a");
    REQUIRE(judge_amount.is_err());
    CHECK(judge_amount.unwrap_err() ==
          ConfigError{ConfigErrorKind::InvalidValue, "a.judge.amount"});
}

TEST_CASE("card: catalog loads deck + card files")
{
    const auto dir = temp_dir("tkw_card_catalog");
    REQUIRE(pjh::platform::Fs::create_directories(dir / "cards").is_ok());

    CHECK(tkw::io::write_text(dir / "deck.json",
                              R"({"name": "mini", "cards": ["sha", "chitu"]})")
              .is_ok());
    CHECK(tkw::io::write_text(dir / "cards" / "sha.json", R"({
        "id": "sha", "name": "杀", "type": "basic", "subtype": "attack",
        "copies": [ {"suit": "spade", "number": 7} ],
        "effect": {"kind": "damage", "amount": 1, "scope": "one_other"}
    })").is_ok());
    CHECK(tkw::io::write_text(dir / "cards" / "chitu.json", R"({
        "id": "chitu", "name": "赤兔", "type": "equipment", "subtype": "horse",
        "copies": [ {"suit": "heart", "number": 5} ],
        "equip": {"slot": "offensive_horse"}
    })").is_ok());

    tkw::config::ResourceStore store(dir);
    auto r = CardDefCatalog::load(store, "deck");
    REQUIRE(r.is_ok());
    CHECK(r.unwrap().size() == 2);
    CHECK(r.unwrap().total_copies() == 2);

    auto sha = r.unwrap().find("sha");
    REQUIRE(sha.is_some());
    CHECK(sha.unwrap()->name == "杀");
    CHECK(sha.unwrap()->copies.size() == 1);
    CHECK(r.unwrap().find("nope").is_none());

    // 迭代顺序 = deck.json 引用顺序（["sha", "chitu"]），不得退化为无序
    const auto &cat = r.unwrap();
    auto it = cat.begin();
    CHECK(it->id == "sha");
    ++it;
    CHECK(it->id == "chitu");
    CHECK(std::next(it) == cat.end());
}

TEST_CASE("card: catalog load error paths")
{
    const auto dir = temp_dir("tkw_card_catalog_err");
    REQUIRE(pjh::platform::Fs::create_directories(dir / "cards").is_ok());

    CHECK(tkw::io::write_text(dir / "deck.json",
                              R"({"name": "mini", "cards": ["ghost"]})")
              .is_ok());

    // 引用的卡牌文件缺失 → FileNotFound（完整路径）
    {
        tkw::config::ResourceStore store(dir);
        auto r = CardDefCatalog::load(store, "deck");
        REQUIRE(r.is_err());
        CHECK(r.unwrap_err().kind == ConfigErrorKind::FileNotFound);
        CHECK(r.unwrap_err().detail == (dir / "cards" / "ghost.json").string());
    }

    // 文件内 id 与文件名不一致 → InvalidValue
    CHECK(tkw::io::write_text(dir / "cards" / "ghost.json", R"({
        "id": "tao", "name": "桃", "type": "basic",
        "copies": [ {"suit": "heart", "number": 3} ]
    })").is_ok());
    {
        tkw::config::ResourceStore store(dir);
        auto r = CardDefCatalog::load(store, "deck");
        REQUIRE(r.is_err());
        CHECK(r.unwrap_err().kind == ConfigErrorKind::InvalidValue);
        CHECK(r.unwrap_err().detail == "cards/ghost.json.id");
    }

    // deck 重复引用同一卡牌 → InvalidValue
    CHECK(tkw::io::write_text(dir / "deck.json",
                              R"({"name": "mini", "cards": ["tao", "tao"]})")
              .is_ok());
    CHECK(tkw::io::write_text(dir / "cards" / "tao.json", R"({
        "id": "tao", "name": "桃", "type": "basic",
        "copies": [ {"suit": "heart", "number": 3} ]
    })").is_ok());
    {
        tkw::config::ResourceStore store(dir);
        auto r = CardDefCatalog::load(store, "deck");
        REQUIRE(r.is_err());
        CHECK(r.unwrap_err().kind == ConfigErrorKind::InvalidValue);
        CHECK(r.unwrap_err().detail.find("重复引用") != std::string::npos);
    }
}

TEST_CASE("card: catalog rejects an empty deck at load time")
{
    const auto dir = temp_dir("tkw_card_catalog_empty");
    REQUIRE(pjh::platform::Fs::create_directories(dir / "cards").is_ok());

    // 空引用列表：牌堆无张，不得建成可跑局的空目录
    CHECK(tkw::io::write_text(dir / "deck.json",
                               R"({"name": "mini", "cards": []})")
        .is_ok());
    {
        tkw::config::ResourceStore store(dir);
        auto r = CardDefCatalog::load(store, "deck");
        REQUIRE(r.is_err());
        CHECK(r.unwrap_err().kind == ConfigErrorKind::InvalidValue);
        CHECK(r.unwrap_err().detail.find("牌堆为空") != std::string::npos);
    }

    // 引用了卡但该卡副本数为 0：总张数仍为 0，同型拒载
    CHECK(tkw::io::write_text(dir / "deck.json",
                               R"({"name": "mini", "cards": ["sha"]})")
        .is_ok());
    CHECK(tkw::io::write_text(
              dir / "cards" / "sha.json",
              R"({"id": "sha", "name": "杀", "type": "basic", "copies": []})")
        .is_ok());
    {
        tkw::config::ResourceStore store(dir);
        auto r = CardDefCatalog::load(store, "deck");
        REQUIRE(r.is_err());
        CHECK(r.unwrap_err().kind == ConfigErrorKind::InvalidValue);
        CHECK(r.unwrap_err().detail.find("牌堆为空") != std::string::npos);
    }
}

#ifdef TKW_TEST_RESOURCE_DIR
TEST_CASE("card: real standard deck loads to 108 copies")
{
    tkw::config::ResourceStore store(TKW_TEST_RESOURCE_DIR);
    auto r = CardDefCatalog::load(store, "deck");
    REQUIRE(r.is_ok());
    const auto &cat = r.unwrap();
    CHECK(cat.size() == 32);
    CHECK(cat.total_copies() == 108);

    // 迭代顺序 = deck.json 引用顺序（首张 sha，末张 zhuahuang）
    CHECK(cat.begin()->id == "sha");
    std::string last_id;
    for (const auto &def : cat)
        last_id = def.id;
    CHECK(last_id == "zhuahuang");

    auto sha = cat.find("sha");
    REQUIRE(sha.is_some());
    CHECK(sha.unwrap()->copies.size() == 30);
    CHECK(sha.unwrap()->type == CardType::Basic);
    REQUIRE(sha.unwrap()->effect.is_some());
    CHECK(sha.unwrap()->effect.unwrap().kind == CardEffectKind::Damage);
    CHECK(sha.unwrap()->effect.unwrap().scope.contains(Scope::OneOther));
    CHECK(sha.unwrap()->effect.unwrap().response.contains(ResponseKind::Jink));
    // 标准杀不显式标注属性：默认普通，deck_hash 不受新字段影响
    CHECK(sha.unwrap()->effect.unwrap().damage_type == DamageType::Normal);

    auto liangnu = cat.find("liangnu");
    REQUIRE(liangnu.is_some());
    REQUIRE(liangnu.unwrap()->equip.is_some());
    CHECK(liangnu.unwrap()->equip.unwrap().slot == EquipSlot::Weapon);
    CHECK(liangnu.unwrap()->equip.unwrap().range == 1);

    auto taoyuan = cat.find("taoyuan");
    REQUIRE(taoyuan.is_some());
    REQUIRE(taoyuan.unwrap()->effect.is_some());
    CHECK(taoyuan.unwrap()->effect.unwrap().kind == CardEffectKind::Heal);
    CHECK(taoyuan.unwrap()->effect.unwrap().scope.contains(Scope::All));
}

TEST_CASE("card: junzheng skeleton deck loads elemental slashes")
{
    // 扩展牌表落在 resources/ 的子目录：既有 --deck 目录选择直接生效
    tkw::config::ResourceStore store(TKW_TEST_RESOURCE_DIR "/junzheng");
    auto r = CardDefCatalog::load(store, "deck");
    REQUIRE(r.is_ok());
    const auto &cat = r.unwrap();
    CHECK(cat.size() == 12);
    // 杀 30 + 火杀 5 + 雷杀 9 + 藤甲 2 + 丈八 1 + 青釭 1 + 南蛮 3 + 万箭 1 + 无中 4 + 桃 8 + 酒 5 + 兵粮寸断 2
    CHECK(cat.total_copies() == 71);

    auto huosha = cat.find("huosha");
    REQUIRE(huosha.is_some());
    CHECK(huosha.unwrap()->name == "火杀");
    REQUIRE(huosha.unwrap()->effect.is_some());
    CHECK(huosha.unwrap()->effect.unwrap().kind == CardEffectKind::Damage);
    CHECK(huosha.unwrap()->effect.unwrap().damage_type == DamageType::Fire);

    auto leisha = cat.find("leisha");
    REQUIRE(leisha.is_some());
    CHECK(leisha.unwrap()->name == "雷杀");
    REQUIRE(leisha.unwrap()->effect.is_some());
    CHECK(leisha.unwrap()->effect.unwrap().kind == CardEffectKind::Damage);
    CHECK(leisha.unwrap()->effect.unwrap().damage_type == DamageType::Thunder);

    // 藤甲：防具能力名经封闭表解析，可审计、可展示
    auto tengjia = cat.find("tengjia");
    REQUIRE(tengjia.is_some());
    CHECK(tengjia.unwrap()->name == "藤甲");
    CHECK(tengjia.unwrap()->type == CardType::Equipment);
    REQUIRE(tengjia.unwrap()->equip.is_some());
    CHECK(tengjia.unwrap()->equip.unwrap().slot == EquipSlot::Armor);
    REQUIRE(tengjia.unwrap()->abilities.size() == 1);
    CHECK(tengjia.unwrap()->abilities[0] == Ability::VineArmor);

    // 酒：出牌阶段主动增伤 + 仅自救标记；花色点数与权威牌表一致
    auto jiu = cat.find("jiu");
    REQUIRE(jiu.is_some());
    CHECK(jiu.unwrap()->name == "酒");
    CHECK(jiu.unwrap()->type == CardType::Basic);
    REQUIRE(jiu.unwrap()->effect.is_some());
    CHECK(jiu.unwrap()->effect.unwrap().kind == CardEffectKind::Analeptic);
    CHECK(jiu.unwrap()->effect.unwrap().scope.contains(Scope::Self));
    CHECK(jiu.unwrap()->self_rescue);
    CHECK_FALSE(jiu.unwrap()->rescue);
    REQUIRE(jiu.unwrap()->copies.size() == 5);
    CHECK(jiu.unwrap()->copies[0] == CardCopy{Suit::Diamond, 9});

    // 兵粮寸断：延时锦囊，非梅花跳摸牌；距离 1 限制进数据
    auto bingliang = cat.find("bingliang");
    REQUIRE(bingliang.is_some());
    CHECK(bingliang.unwrap()->name == "兵粮寸断");
    CHECK(bingliang.unwrap()->type == CardType::Trick);
    REQUIRE(bingliang.unwrap()->judge.is_some());
    CHECK(bingliang.unwrap()->judge.unwrap().trigger == JudgeTrigger::NotClub);
    CHECK(bingliang.unwrap()->judge.unwrap().success == JudgeAction::SkipDraw);
    CHECK(bingliang.unwrap()->judge.unwrap().scope.contains(Scope::OneOther));
    CHECK(bingliang.unwrap()->judge.unwrap().range == 1);
    REQUIRE(bingliang.unwrap()->copies.size() == 2);
    CHECK(bingliang.unwrap()->copies[0] == CardCopy{Suit::Spade, 10});
    CHECK(bingliang.unwrap()->copies[1] == CardCopy{Suit::Club, 4});

    // 容错扫描与严格加载同源：十二卡均为已知机制名
    auto raws = scan_mechanisms(store, "deck");
    REQUIRE(raws.is_ok());
    CHECK(raws.unwrap().size() == 12);
}

TEST_CASE("card: scan_mechanisms reads raw names and shares the name table")
{
    tkw::config::ResourceStore store(TKW_TEST_RESOURCE_DIR);
    auto r = scan_mechanisms(store, "deck");
    REQUIRE(r.is_ok());
    const auto &raws = r.unwrap();
    CHECK(raws.size() == 32);
    CHECK(raws.front().id == "sha");

    const auto sha = std::find_if(raws.begin(), raws.end(),
                                  [](const RawMechanisms &m) { return m.id == "sha"; });
    REQUIRE(sha != raws.end());
    CHECK(sha->name == "杀");
    REQUIRE(sha->effect_kind.is_some());
    CHECK(sha->effect_kind.unwrap() == "damage");
    CHECK(sha->abilities.empty());

    // 名称查询与严格解析同表：已知名命中、未知名回落 None
    CHECK(effect_kind_from_name("damage").unwrap() == CardEffectKind::Damage);
    CHECK(effect_kind_from_name("summon").is_none());
    CHECK(ability_from_name("cixiong").unwrap() == Ability::Cixiong);
    CHECK(ability_from_name("super_power").is_none());
}

TEST_CASE("card: scan_mechanisms tolerates unknown mechanism names")
{
    const auto dir = temp_dir("tkw_card_scan_unknown");
    REQUIRE(pjh::platform::Fs::create_directories(dir / "cards").is_ok());

    CHECK(tkw::io::write_text(dir / "deck.json",
                              R"({"name": "mini", "cards": ["ghost"]})")
              .is_ok());
    CHECK(tkw::io::write_text(dir / "cards" / "ghost.json", R"({
        "id": "ghost", "name": "幽魂", "type": "basic",
        "copies": [ {"suit": "spade", "number": 1} ],
        "effect": {"kind": "summon", "amount": 1, "scope": "one_other"},
        "abilities": ["super_power"]
    })").is_ok());

    tkw::config::ResourceStore store(dir);
    auto r = scan_mechanisms(store, "deck");
    REQUIRE(r.is_ok());
    REQUIRE(r.unwrap().size() == 1);
    CHECK(r.unwrap()[0].id == "ghost");
    CHECK(r.unwrap()[0].name == "幽魂");
    REQUIRE(r.unwrap()[0].effect_kind.is_some());
    CHECK(r.unwrap()[0].effect_kind.unwrap() == "summon");
    CHECK(r.unwrap()[0].abilities == std::vector<std::string>{"super_power"});

    // 审计容错不放松严格加载：同一牌堆仍被拒载
    CHECK(CardDefCatalog::load(store, "deck").is_err());
}
#endif  // TKW_TEST_RESOURCE_DIR
