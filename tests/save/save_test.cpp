#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "card/catalog.hpp"
#include "config/resource.hpp"
#include "entity/hp.hpp"
#include "game/core/roles.hpp"
#include "game/ai/simple.hpp"
#include "game/flow/loop.hpp"
#include "game/flow/table.hpp"
#include "game/resolve/combat.hpp"
#include "io/file.hpp"
#include "save/error.hpp"
#include "save/reader.hpp"
#include "save/session_meta.hpp"
#include "save/writer.hpp"
#include "util/rng.hpp"

namespace
{
    using namespace tkw;
    using namespace tkw::game;

    std::unique_ptr<Game> make_game(std::uint32_t seed)
    {
        config::ResourceStore store(TKW_TEST_RESOURCE_DIR);
        auto cat = card::CardDefCatalog::load(store, "deck");
        REQUIRE(cat.is_ok());
        auto g = std::make_unique<Game>(
            std::move(cat).unwrap(), std::make_unique<SeededRng>(seed));
        for (int i = 0; i < 4; ++i)
            REQUIRE(
                g->add_player("P" + std::to_string(i), i, entity::Hp::make(4)).is_ok());
        return g;
    }

    /** 混合性别夹具：P1/P2 为女，其余为男（性别值不再与默认回退重合）。 */
    std::unique_ptr<Game> make_game_mixed(std::uint32_t seed)
    {
        config::ResourceStore store(TKW_TEST_RESOURCE_DIR);
        auto cat = card::CardDefCatalog::load(store, "deck");
        REQUIRE(cat.is_ok());
        auto g = std::make_unique<Game>(
            std::move(cat).unwrap(), std::make_unique<SeededRng>(seed));
        const entity::Gender genders[] = {
            entity::Gender::Male, entity::Gender::Female,
            entity::Gender::Female, entity::Gender::Male};
        for (int i = 0; i < 4; ++i)
            REQUIRE(g->add_player("P" + std::to_string(i), i,
                                  entity::Hp::make(4), genders[i]).is_ok());
        return g;
    }

    /** 在 4 人夹具上置身份局模式与一套标准角色（P0 主公）。 */
    void make_identity(Game &g)
    {
        g.mode = GameMode::Identity;
        g.roles = {{"P0", Role::Lord},
                   {"P1", Role::Loyalist},
                   {"P2", Role::Rebel},
                   {"P3", Role::Traitor}};
    }

    /** 整段擦除 ",\"roles":{...}"（roles 对象无嵌套）；无该段时原样返回。 */
    std::string strip_roles(std::string text)
    {
        const auto pos = text.find(",\"roles\":");
        if (pos == std::string::npos)
            return text;
        const auto close = text.find('}', pos);
        if (close != std::string::npos)
            text.erase(pos, close - pos + 1);
        return text;
    }
}

TEST_CASE("save: round-trips a mid-game state and continues identically")
{
    auto a = make_game(42);
    auto ctxa = a->context();
    GameSession sa;
    SimpleAI ai;
    REQUIRE(start_session(ctxa, sa, "P0").is_ok());
    for (int i = 0; i < 7 && !session_over(ctxa); ++i)
        REQUIRE(step_session(ctxa, ai, sa).is_ok());

    const std::string text = save::write(*a, sa, "deck");

    auto b = make_game(999);  // 不同种子，应被存档覆盖
    auto ctxb = b->context();
    GameSession sb;
    REQUIRE(save::read(text, *b, sb).is_ok());
    CHECK(sb.current == sa.current);
    CHECK(sb.turns == sa.turns);
    CHECK(save::write(*b, sb, "deck") == text);  // 规范化往返稳定

    // 续跑：RNG 与全量状态一致 → 后续状态逐字一致
    SimpleAI ai2;
    SimpleAI ai3;
    for (int i = 0; i < 5 && !session_over(ctxa); ++i)
        REQUIRE(step_session(ctxa, ai2, sa).is_ok());
    for (int i = 0; i < 5 && !session_over(ctxb); ++i)
        REQUIRE(step_session(ctxb, ai3, sb).is_ok());
    CHECK(save::write(*a, sa, "deck") == save::write(*b, sb, "deck"));
}

TEST_CASE("save: legacy saves without gender load as all male")
{
    auto a = make_game(42);
    auto ctxa = a->context();
    GameSession sa;
    SimpleAI ai;
    REQUIRE(start_session(ctxa, sa, "P0").is_ok());
    for (int i = 0; i < 3 && !session_over(ctxa); ++i)
        REQUIRE(step_session(ctxa, ai, sa).is_ok());

    const std::string text = save::write(*a, sa, "deck");
    REQUIRE(text.find("\"gender\"") != std::string::npos);

    // 去掉 gender 字段，模拟无性别字段年代的旧档
    std::string legacy = text;
    for (const std::string &field : {",\"gender\":\"male\"",
                                      ",\"gender\":\"female\""})
    {
        std::size_t pos = 0;
        while ((pos = legacy.find(field, pos)) != std::string::npos)
            legacy.erase(pos, field.size());
    }
    CHECK(legacy.find("gender") == std::string::npos);

    auto b = make_game(999);  // 不同种子，应被存档覆盖
    GameSession sb;
    REQUIRE(save::read(legacy, *b, sb).is_ok());
    for (int i = 0; i < 4; ++i)
        CHECK(b->entities.find("P" + std::to_string(i)).unwrap()->get_gender() ==
              entity::Gender::Male);
    // 规范化往返：旧档加载后再存 = 新格式原文
    CHECK(save::write(*b, sb, "deck") == text);
}

TEST_CASE("save: female gender survives the round trip")
{
    auto a = make_game_mixed(42);
    auto ctxa = a->context();
    GameSession sa;
    REQUIRE(start_session(ctxa, sa, "P0").is_ok());

    const std::string text = save::write(*a, sa, "deck");
    REQUIRE(text.find("\"gender\":\"female\"") != std::string::npos);
    REQUIRE(text.find("\"gender\":\"male\"") != std::string::npos);

    // 目标先建全男占位：读档后性别事实源必须是存档，而非缺字段时的 Male 回退
    auto b = make_game(999);
    GameSession sb;
    REQUIRE(save::read(text, *b, sb).is_ok());
    const entity::Gender expect[] = {
        entity::Gender::Male, entity::Gender::Female,
        entity::Gender::Female, entity::Gender::Male};
    for (int i = 0; i < 4; ++i)
        CHECK(b->entities.find("P" + std::to_string(i)).unwrap()->get_gender() ==
              expect[i]);

    // 规范化往返：读档后再存 = 原文
    CHECK(save::write(*b, sb, "deck") == text);
}

TEST_CASE("save: version mismatch is rejected")
{
    auto a = make_game(1);
    GameSession s;
    const std::string text = save::write(*a, s, "deck");
    std::string bad = text;
    const auto pos = bad.find("\"version\":1");
    REQUIRE(pos != std::string::npos);
    bad.replace(pos, 11, "\"version\":3");

    auto b = make_game(1);
    GameSession sb;
    auto r = save::read(bad, *b, sb);
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err().kind == save::SaveErrorKind::VersionMismatch);

    // version 2 是含连环状态的合法格式，读取端须接受
    auto a2 = make_game(1);
    a2->entities.find("P0").unwrap()->set_chained(true);
    GameSession s2;
    const std::string v2 = save::write(*a2, s2, "deck");
    REQUIRE(v2.find("\"version\":2") != std::string::npos);
    auto b2 = make_game(999);
    GameSession sb2;
    CHECK(save::read(v2, *b2, sb2).is_ok());
    CHECK(b2->entities.find("P0").unwrap()->get_chained());
}

TEST_CASE("save: chained state round-trips and bumps version")
{
    auto a = make_game(1);
    a->entities.find("P0").unwrap()->set_chained(true);
    GameSession sa;
    const std::string text = save::write(*a, sa, "deck");
    CHECK(text.find("\"version\":2") != std::string::npos);
    CHECK(text.find("\"chained\":true") != std::string::npos);

    auto b = make_game(999);
    GameSession sb;
    REQUIRE(save::read(text, *b, sb).is_ok());
    CHECK(b->entities.find("P0").unwrap()->get_chained());
    CHECK_FALSE(b->entities.find("P1").unwrap()->get_chained());
    // 规范化往返：同状态二次写出逐字节一致
    CHECK(save::write(*b, sb, "deck") == text);
}

TEST_CASE("save: chainless save keeps version 1 and omits chained")
{
    auto a = make_game(1);
    GameSession sa;
    const std::string text = save::write(*a, sa, "deck");
    CHECK(text.find("\"version\":1") != std::string::npos);
    CHECK(text.find("chained") == std::string::npos);

    // 旧格式（v1、无 chained 字段）新二进制可读，连环回落未横置
    auto b = make_game(999);
    GameSession sb;
    REQUIRE(save::read(text, *b, sb).is_ok());
    for (int i = 0; i < 4; ++i)
        CHECK_FALSE(
            b->entities.find("P" + std::to_string(i)).unwrap()->get_chained());
}

TEST_CASE("save: out-of-range integers are rejected instead of narrowed")
{
    auto a = make_game(1);
    GameSession s;
    const std::string base = save::write(*a, s, "deck");

    auto mutate = [](std::string text, std::string_view from, std::string_view to)
    {
        const auto pos = text.find(from);
        REQUIRE(pos != std::string::npos);
        text.replace(pos, from.size(), to);
        return text;
    };

    // version 4294967297 收窄后本为 1，须与错误版本同路径失败
    {
        auto b = make_game(1);
        GameSession sb;
        auto r = save::read(
            mutate(base, "\"version\":1", "\"version\":4294967297"), *b, sb);
        REQUIRE(r.is_err());
        CHECK(r.unwrap_err().kind == save::SaveErrorKind::VersionMismatch);
        CHECK(r.unwrap_err().detail == "version");
    }

    // rules 字段超界：收窄处失败，与类型不符同节级路径
    {
        auto b = make_game(1);
        GameSession sb;
        auto r = save::read(
            mutate(base, "\"wuxie_rounds\":32", "\"wuxie_rounds\":4294967297"),
            *b, sb);
        REQUIRE(r.is_err());
        CHECK(r.unwrap_err().kind == save::SaveErrorKind::StructureError);
        CHECK(r.unwrap_err().detail == "rules");
    }

    // stats 计数（read_int_map）超界：与类型不符同路径
    {
        save::SessionMeta meta;
        meta.stats.damage_dealt["P0"] = 3;
        const std::string text = save::write(*a, s, "deck", meta);
        auto b = make_game(1);
        GameSession sb;
        auto r = save::read(
            mutate(text, "\"damage_dealt\":{\"P0\":3}",
                   "\"damage_dealt\":{\"P0\":4294967297}"),
            *b, sb);
        REQUIRE(r.is_err());
        CHECK(r.unwrap_err().kind == save::SaveErrorKind::StructureError);
        CHECK(r.unwrap_err().detail == "session.stats");
    }

    // 负数 instance_seq：不得回绕成巨 uint64
    {
        auto b = make_game(1);
        GameSession sb;
        auto r = save::read(
            mutate(base, "\"instance_seq\":0", "\"instance_seq\":-1"), *b, sb);
        REQUIRE(r.is_err());
        CHECK(r.unwrap_err().kind == save::SaveErrorKind::StructureError);
        CHECK(r.unwrap_err().detail == "cards.instance_seq");
    }
}

TEST_CASE("save: session meta round-trips ai and stats")
{
    auto a = make_game(42);
    auto ctxa = a->context();
    GameSession sa;
    SimpleAI ai;
    REQUIRE(start_session(ctxa, sa, "P0").is_ok());
    for (int i = 0; i < 4 && !session_over(ctxa); ++i)
        REQUIRE(step_session(ctxa, ai, sa).is_ok());

    save::SessionMeta meta;
    meta.ai = "aggressive";
    meta.stats.damage_dealt["P0"] = 5;
    meta.stats.healing["P1"] = 2;
    meta.stats.kills["P0"] = 1;
    meta.stats.last_hit_source["P1"] = "P0";
    meta.stats.died.insert("P1");

    const std::string text = save::write(*a, sa, "deck", meta);
    CHECK(text.find("\"ai\":\"aggressive\"") != std::string::npos);
    CHECK(text.find("\"stats\":") != std::string::npos);
    // 规范化：同状态 + 同 meta 二次写出逐字节一致（std::map/set 有序）
    CHECK(save::write(*a, sa, "deck", meta) == text);

    auto b = make_game(999);
    GameSession sb;
    save::SessionMeta back;
    REQUIRE(save::read(text, *b, sb, &back).is_ok());
    CHECK(back.ai == "aggressive");
    CHECK(back.stats.damage_dealt == meta.stats.damage_dealt);
    CHECK(back.stats.healing == meta.stats.healing);
    CHECK(back.stats.kills == meta.stats.kills);
    CHECK(back.stats.last_hit_source == meta.stats.last_hit_source);
    CHECK(back.stats.died == meta.stats.died);
    // 读档后再存（无 meta 参数）不残留元数据，与默认 meta 原文一致
    CHECK(save::write(*b, sb, "deck") == save::write(*a, sa, "deck"));
}

TEST_CASE("save: legacy save without session meta loads defaults")
{
    auto a = make_game(42);
    GameSession sa;
    const std::string text = save::write(*a, sa, "deck");
    // 默认元数据不写 ai/stats，等价于旧档
    REQUIRE(text.find("\"ai\"") == std::string::npos);
    REQUIRE(text.find("\"stats\"") == std::string::npos);

    auto b = make_game(999);
    GameSession sb;
    save::SessionMeta meta;
    meta.ai = "placeholder";
    meta.stats.kills["P9"] = 3;
    REQUIRE(save::read(text, *b, sb, &meta).is_ok());
    // 旧档缺失字段回落默认；出参被重置而非保留调用前数值
    CHECK(meta.ai.empty());
    CHECK(meta.stats.kills.empty());
    CHECK(meta.stats.damage_dealt.empty());
    // 旧档无元数据读回后仍规范化为原文
    save::SessionMeta empty;
    CHECK(save::write(*b, sb, "deck", empty) == text);
}

TEST_CASE("save: session ai wrong type is a structure error")
{
    auto a = make_game(42);
    GameSession sa;
    save::SessionMeta meta;
    meta.ai = "simple";
    std::string text = save::write(*a, sa, "deck", meta);
    const auto pos = text.find("\"ai\":\"simple\"");
    REQUIRE(pos != std::string::npos);
    text.replace(pos, 13, "\"ai\":12345");

    auto b = make_game(1);
    GameSession sb;
    auto r = save::read(text, *b, sb);
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err().kind == save::SaveErrorKind::StructureError);
    CHECK(r.unwrap_err().detail.find("session.ai") != std::string::npos);
}

TEST_CASE("save: malformed JSON is rejected")
{
    auto a = make_game(1);
    GameSession s;
    auto r = save::read("{not json", *a, s);
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err().kind == save::SaveErrorKind::ParseError);
}

TEST_CASE("save: atomic write round-trips through a file")
{
    const auto dir =
        std::filesystem::temp_directory_path() / "tkw_save_atomic_test";
    std::filesystem::remove_all(dir);
    REQUIRE(std::filesystem::create_directories(dir));
    const auto path = dir / "save.json";

    REQUIRE(tkw::io::write_text_atomic(path, "{\"a\":1}").is_ok());
    auto text = tkw::io::read_text(path);
    REQUIRE(text.is_ok());
    CHECK(text.unwrap() == "{\"a\":1}");

    // 覆盖已有目标（真实存档内容）
    auto a = make_game(42);
    GameSession s;
    const std::string big = save::write(*a, s, "deck");
    REQUIRE(tkw::io::write_text_atomic(path, big).is_ok());
    auto back = tkw::io::read_text(path);
    REQUIRE(back.is_ok());
    CHECK(back.unwrap() == big);

    std::filesystem::remove_all(dir);
}

TEST_CASE("save: failed rng restore leaves the target untouched")
{
    // 源档：合法存档文本
    auto a = make_game(42);
    auto ctxa = a->context();
    GameSession sa;
    SimpleAI ai;
    REQUIRE(start_session(ctxa, sa, "P0").is_ok());
    for (int i = 0; i < 7 && !session_over(ctxa); ++i)
        REQUIRE(step_session(ctxa, ai, sa).is_ok());
    std::string bad = save::write(*a, sa, "deck");

    // 破坏 rng 状态首字符（十进制数字变 x）→ load_state 必失败
    const auto pos = bad.find("\"rng\":{\"data\":\"");
    REQUIRE(pos != std::string::npos);
    bad.replace(pos + 15, 1, "x");

    // 目标先置非默认状态以便观测（rules + session），基线一次覆盖五段
    auto b = make_game(7);
    b->rules.draw_per_turn = 9;
    GameSession sb{"P0", 5, true};
    const std::string before = save::write(*b, sb, "deck");

    auto r = save::read(bad, *b, sb);
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err().kind == save::SaveErrorKind::RngError);
    CHECK(save::write(*b, sb, "deck") == before);
}

TEST_CASE("save: a high-bit fingerprint round-trips through the int64 wire form")
{
    // 牌表指纹是 uint64，但 JSON 数值只有 int64 精确域；高位指纹（≥2^63）
    // 必须按 int64 位型写出（负十进制）并在读取端逐位还原，否则存档写出后
    // 读不回。小牌表选定一个高位指纹做端到端钉子。
    const auto dir =
        std::filesystem::temp_directory_path() / "tkw_save_high_bit_hash";
    std::filesystem::remove_all(dir);
    REQUIRE(std::filesystem::create_directories(dir / "cards"));

    auto write_deck = [&](const std::string &card_id)
    {
        REQUIRE(tkw::io::write_text(
                    dir / "deck.json",
                    std::string("{\"name\":\"hi\",\"cards\":[\"") + card_id + "\"]}")
                    .is_ok());
        REQUIRE(tkw::io::write_text(
                    dir / "cards" / (card_id + ".json"),
                    std::string("{\"id\":\"") + card_id +
                        "\",\"name\":\"测\",\"type\":\"basic\","
                        "\"copies\":[{\"suit\":\"spade\",\"number\":7}]}")
                    .is_ok());
    };

    // 自发现首个 ≥2^63 指纹：FNV-1a 确定，循环上限保证终止
    std::string id;
    for (int i = 0; id.empty() && i < 64; ++i)
    {
        const std::string cand = "h" + std::to_string(i);
        write_deck(cand);
        tkw::config::ResourceStore store(dir);
        auto cat = card::CardDefCatalog::load(store, "deck");
        REQUIRE(cat.is_ok());
        if (save::deck_hash(cat.unwrap()) >= (std::uint64_t{1} << 63))
            id = cand;
    }
    REQUIRE(!id.empty());

    tkw::config::ResourceStore store(dir);
    auto cat_a = card::CardDefCatalog::load(store, "deck").unwrap();
    const std::uint64_t h = save::deck_hash(cat_a);
    REQUIRE(h >= (std::uint64_t{1} << 63));

    Game a(std::move(cat_a), std::make_unique<SeededRng>(1));
    REQUIRE(a.add_player("P0", 0, entity::Hp::make(4)).is_ok());
    GameSession sa;
    const std::string text = save::write(a, sa, "deck");

    // 高位指纹以 int64 位型的负十进制承载（而非超出 int64 的正整数）
    const auto wire = std::to_string(static_cast<std::int64_t>(h));
    CHECK(text.find("\"hash\":" + wire) != std::string::npos);
    CHECK(text.find("\"hash\":" + std::to_string(h)) == std::string::npos);

    auto cat_b = card::CardDefCatalog::load(store, "deck").unwrap();
    Game b(std::move(cat_b), std::make_unique<SeededRng>(99));
    REQUIRE(b.add_player("P0", 0, entity::Hp::make(4)).is_ok());
    GameSession sb;
    REQUIRE(save::read(text, b, sb).is_ok());
    CHECK(save::write(b, sb, "deck") == text);

    std::filesystem::remove_all(dir);
}

TEST_CASE("save: a low-bit fingerprint stays a positive integer for legacy readers")
{
    // 标准牌表指纹 <2^63：写出与旧版逐字节一致的正整数，旧存档与新存档
    // 双向可读。
    auto a = make_game(1);
    GameSession sa;
    const std::uint64_t h = save::deck_hash(a->catalog);
    REQUIRE(h < (std::uint64_t{1} << 63));
    const std::string text = save::write(*a, sa, "deck");
    CHECK(text.find("\"hash\":" + std::to_string(h)) != std::string::npos);

    auto b = make_game(999);
    GameSession sb;
    REQUIRE(save::read(text, *b, sb).is_ok());
    CHECK(save::write(*b, sb, "deck") == text);
}

TEST_CASE("save: standard deck fingerprint stays pinned")
{
    // 闪电数据化为雷电属性后标准指纹更新；判定属性变更仍参与哈希，
    // 显式改写属性会被检出；历史标准档由降级模式重算并经 reader 兼容。
    auto a = make_game(1);
    CHECK(save::deck_hash(a->catalog) == 4373990461741036563ULL);
}

TEST_CASE("save: legacy fingerprint without judge damage_type still loads")
{
    // 判定属性数据化前写出的标准档指纹等于「降级模式」重算值：reader 双指纹
    // 接受，旧档可读；再次写出用当前指纹（兼容只在读侧，写出不回退）。
    auto a = make_game(1);
    GameSession sa;
    const std::uint64_t legacy = save::deck_hash(a->catalog, false);
    CHECK(legacy == 5176414080095780405ULL);
    const std::uint64_t current = save::deck_hash(a->catalog);
    REQUIRE(current != legacy);

    std::string text = save::write(*a, sa, "deck");
    const std::string from = "\"hash\":" + std::to_string(current);
    const auto pos = text.find(from);
    REQUIRE(pos != std::string::npos);
    text.replace(pos, from.size(), "\"hash\":" + std::to_string(legacy));

    auto b = make_game(999);
    GameSession sb;
    REQUIRE(save::read(text, *b, sb).is_ok());
    CHECK(save::write(*b, sb, "deck").find(from) != std::string::npos);
}

TEST_CASE("save: deck hash errors distinguish structure from mismatch")
{
    auto a = make_game(1);
    GameSession s;
    const std::string text = save::write(*a, s, "deck");
    const auto key = text.find("\"hash\":");
    REQUIRE(key != std::string::npos);
    const auto val_begin = key + 7;
    auto val_end = val_begin;
    while (val_end < text.size() && text[val_end] != ',' && text[val_end] != '}')
        ++val_end;
    REQUIRE(val_end > val_begin);
    const auto with_hash = [&](std::string_view v)
    {
        std::string t = text;
        t.replace(val_begin, val_end - val_begin, v);
        return t;
    };

    // 合法 int64 位型但指纹不符（含 UINT64_MAX = -1、2^63 = INT64_MIN）→ DeckMismatch
    for (std::string_view v : {"0", "-1", "-9223372036854775808"})
    {
        auto b = make_game(1);
        GameSession sb;
        auto r = save::read(with_hash(v), *b, sb);
        REQUIRE(r.is_err());
        CHECK(r.unwrap_err().kind == save::SaveErrorKind::DeckMismatch);
        CHECK(r.unwrap_err().detail == "deck.hash");
    }
    // 超出 int64 范围的正整数退化为 double、非数值类型 → StructureError
    for (std::string_view v : {"18446744073709551615", "\"abc\"", "true"})
    {
        auto b = make_game(1);
        GameSession sb;
        auto r = save::read(with_hash(v), *b, sb);
        REQUIRE(r.is_err());
        CHECK(r.unwrap_err().kind == save::SaveErrorKind::StructureError);
        CHECK(r.unwrap_err().detail == "deck.hash");
    }

    // 缺失 hash 字段 → StructureError（连同前导逗号一并删除）
    std::string missing = text;
    REQUIRE(key > 0);
    REQUIRE(text[key - 1] == ',');
    missing.erase(key - 1, val_end - key + 1);
    auto c = make_game(1);
    GameSession sc;
    auto rm = save::read(missing, *c, sc);
    REQUIRE(rm.is_err());
    CHECK(rm.unwrap_err().kind == save::SaveErrorKind::StructureError);
    CHECK(rm.unwrap_err().detail == "deck.hash");
}

TEST_CASE("save: per-section structure errors carry their field path")
{
    // 源档：标准牌表、未开局 → writer 紧凑模板（各区空、session 全默认）
    auto a = make_game(1);
    GameSession s;
    const std::string base = save::write(*a, s, "deck");

    // 只替换首个命中子串；找不到即 REQUIRE 红（writer 输出格式漂移的哨兵）
    auto mutate = [](std::string text, std::string_view from, std::string_view to)
    {
        const auto pos = text.find(from);
        REQUIRE(pos != std::string::npos);
        text.replace(pos, from.size(), to);
        return text;
    };

    // 读入独立目标并断言 kind/detail；同时钉「失败不污染目标状态」
    auto expect = [&](const std::string &text, save::SaveErrorKind kind,
                      std::string_view detail)
    {
        auto b = make_game(7);
        b->rules.draw_per_turn = 9;  // 非默认值，使任何污染都可见
        GameSession sb{"P0", 5, true};
        const std::string before = save::write(*b, sb, "deck");

        auto r = save::read(text, *b, sb);
        REQUIRE(r.is_err());
        CHECK(r.unwrap_err().kind == kind);
        CHECK(r.unwrap_err().detail == detail);
        // rules/rng/session/cards/entities 与失败前逐字节一致（先全量校验后落子）
        CHECK(save::write(*b, sb, "deck") == before);
    };

    // 根与信封
    expect("[]", save::SaveErrorKind::StructureError, "root");
    expect(mutate(base, "\"format\":\"tkw-save\",", ""),
           save::SaveErrorKind::VersionMismatch, "format");
    expect(mutate(base, "\"version\":1,", ""),
           save::SaveErrorKind::VersionMismatch, "version");

    // rules
    expect(mutate(base, "\"wuxie_rounds\":32", "\"wuxie_rounds\":\"x\""),
           save::SaveErrorKind::StructureError, "rules");

    // rng
    expect(mutate(base, ",\"rng\":{\"data\":", ",\"rngx\":{\"data\":"),
           save::SaveErrorKind::StructureError, "rng");
    expect(mutate(base, "\"rng\":{\"data\":", "\"rng\":{\"datax\":"),
           save::SaveErrorKind::StructureError, "rng.data");

    // session
    expect(mutate(base, "\"current\":\"\"", "\"current\":123"),
           save::SaveErrorKind::StructureError, "session");
    expect(mutate(base, "\"started\":false", "\"started\":false,\"stats\":123"),
           save::SaveErrorKind::StructureError, "session.stats");

    // cards
    expect(mutate(base, "\"instance_seq\":0", "\"instance_seq\":\"x\""),
           save::SaveErrorKind::StructureError, "cards.instance_seq");
    expect(mutate(base, ",\"cards\":{\"instance_seq\":", ",\"cardsx\":{\"instance_seq\":"),
           save::SaveErrorKind::StructureError, "cards");
    expect(mutate(base, "\"draw\":[]", "\"drawx\":[]"),
           save::SaveErrorKind::StructureError, "cards");

    // entities
    expect(mutate(base, ",\"entities\":[", ",\"entitiesx\":["),
           save::SaveErrorKind::StructureError, "entities");
    expect(mutate(base, "\"seat\":0,", ""),
           save::SaveErrorKind::StructureError, "entities");
    expect(mutate(base, "\"gender\":\"male\"", "\"gender\":\"x\""),
           save::SaveErrorKind::StructureError, "entities.gender");
}

TEST_CASE("save: identity mode and roles round-trip")
{
    auto a = make_game(42);
    make_identity(*a);
    GameSession sa;
    const std::string text = save::write(*a, sa, "deck");

    CHECK(text.find("\"mode\":\"identity\"") != std::string::npos);
    CHECK(text.find("\"roles\":{\"P0\":\"lord\",\"P1\":\"loyalist\","
                    "\"P2\":\"rebel\",\"P3\":\"traitor\"}") != std::string::npos);

    auto b = make_game(999);  // 不同种子，应被存档覆盖
    GameSession sb;
    REQUIRE(save::read(text, *b, sb).is_ok());
    CHECK(b->mode == GameMode::Identity);
    CHECK(b->roles == a->roles);
    CHECK(save::write(*b, sb, "deck") == text);  // 规范化往返稳定
}

TEST_CASE("save: identity save round-trips after a player death")
{
    // 阵亡只移出实体、角色表保留阵亡者：存读必须接受「角色表 ⊇ 存活实体」
    auto a = make_game(42);
    make_identity(*a);
    auto ctxa = a->context();

    // P0 主公与 P2 反贼阵亡 → 主公主张落败，反贼阵营代表仍取角色表首个反贼 P2
    declare_death(ctxa, "P0");
    declare_death(ctxa, "P2");
    REQUIRE(a->entities.find("P2").is_none());

    const WinCamp camp = session_camp(ctxa);
    const std::string winner = session_winner(ctxa);
    REQUIRE(camp == WinCamp::RebelCamp);
    REQUIRE(winner == "P2");  // 阵营代表允许已阵亡

    GameSession sa;
    const std::string text = save::write(*a, sa, "deck");
    // 阵亡者角色仍在写出文本中（写全量角色表）
    CHECK(text.find("\"P2\":\"rebel\"") != std::string::npos);

    auto b = make_game(999);
    GameSession sb;
    REQUIRE(save::read(text, *b, sb).is_ok());
    auto ctxb = b->context();

    // 阵亡者角色读回后仍在表中，且全表与写档前一致
    CHECK(b->roles == a->roles);
    CHECK(b->roles.count("P2") == 1);

    // 阵营代表稳定：读档前后终局口径与代表 id 均不变
    CHECK(session_camp(ctxb) == camp);
    CHECK(session_winner(ctxb) == winner);
    CHECK(save::write(*b, sb, "deck") == text);  // 规范化往返稳定
}

TEST_CASE("save: brawl saves contain no mode key")
{
    auto a = make_game(1);  // 默认乱斗
    GameSession sa;
    const std::string text = save::write(*a, sa, "deck");
    // 乱斗不写新键：默认路径输出与旧格式逐字节一致
    CHECK(text.find("\"mode\"") == std::string::npos);
    CHECK(text.find("\"roles\"") == std::string::npos);
}

TEST_CASE("save: legacy saves without mode load as brawl")
{
    auto a = make_game(1);
    GameSession sa;
    const std::string text = save::write(*a, sa, "deck");  // 等价无 mode 的旧档

    // 目标预置身份局，验证旧档缺失字段回落并重置脏状态
    auto b = make_game(999);
    make_identity(*b);
    GameSession sb;
    REQUIRE(save::read(text, *b, sb).is_ok());
    CHECK(b->mode == GameMode::Brawl);
    CHECK(b->roles.empty());
    CHECK(save::write(*b, sb, "deck") == text);
}

TEST_CASE("save: unknown mode value is a structure error")
{
    auto a = make_game(42);
    make_identity(*a);
    GameSession sa;
    std::string text = save::write(*a, sa, "deck");
    const auto pos = text.find("\"mode\":\"identity\"");
    REQUIRE(pos != std::string::npos);
    text.replace(pos, 17, "\"mode\":\"x\"");

    auto b = make_game(7);
    GameSession sb;
    auto r = save::read(text, *b, sb);
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err().kind == save::SaveErrorKind::StructureError);
    CHECK(r.unwrap_err().detail == "mode");
}

TEST_CASE("save: identity mode without roles is a structure error")
{
    auto a = make_game(42);
    make_identity(*a);
    GameSession sa;
    const std::string text = strip_roles(save::write(*a, sa, "deck"));
    REQUIRE(text.find("\"roles\"") == std::string::npos);

    auto b = make_game(7);
    GameSession sb;
    auto r = save::read(text, *b, sb);
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err().kind == save::SaveErrorKind::StructureError);
    CHECK(r.unwrap_err().detail == "roles");
}

TEST_CASE("save: invalid role value is a structure error")
{
    auto a = make_game(42);
    make_identity(*a);
    GameSession sa;
    std::string text = save::write(*a, sa, "deck");
    const auto pos = text.find("\"P0\":\"lord\"");
    REQUIRE(pos != std::string::npos);
    text.replace(pos, 11, "\"P0\":\"king\"");

    auto b = make_game(7);
    GameSession sb;
    auto r = save::read(text, *b, sb);
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err().kind == save::SaveErrorKind::StructureError);
    CHECK(r.unwrap_err().detail == "roles.P0");
}

TEST_CASE("save: identity roles must cover every living entity")
{
    auto a = make_game(42);
    make_identity(*a);
    GameSession sa;
    const std::string base = save::write(*a, sa, "deck");

    // 缺 P3：存活实体未被角色表覆盖
    {
        std::string text = base;
        const auto pos = text.find(",\"P3\":\"traitor\"");
        REQUIRE(pos != std::string::npos);
        text.erase(pos, 15);
        auto b = make_game(7);
        GameSession sb;
        auto r = save::read(text, *b, sb);
        REQUIRE(r.is_err());
        CHECK(r.unwrap_err().kind == save::SaveErrorKind::StructureError);
        CHECK(r.unwrap_err().detail == "roles");
    }

    // 多 P9：角色分配对整场固定，表内允许保留已阵亡玩家的条目；存档无法
    // 区分「阵亡者」与「未知键」，故一并接受并原样保留
    {
        std::string text = base;
        const auto pos = text.find("\"P3\":\"traitor\"}");
        REQUIRE(pos != std::string::npos);
        text.replace(pos, 15, "\"P3\":\"traitor\",\"P9\":\"rebel\"}");
        auto b = make_game(7);
        GameSession sb;
        REQUIRE(save::read(text, *b, sb).is_ok());
        CHECK(b->roles.count("P9") == 1);
        CHECK(b->roles.at("P9") == Role::Rebel);
        CHECK(save::write(*b, sb, "deck") == text);  // 多余条目规范化往返不丢
    }
}

TEST_CASE("save: identity roles must contain exactly one lord")
{
    auto a = make_game(42);
    make_identity(*a);
    GameSession sa;
    const std::string base = save::write(*a, sa, "deck");

    // 0 主公：唯一主公被改成反贼，覆盖仍完整
    {
        std::string text = base;
        const auto pos = text.find("\"P0\":\"lord\"");
        REQUIRE(pos != std::string::npos);
        text.replace(pos, 11, "\"P0\":\"rebel\"");

        auto b = make_game(7);
        GameSession sb;
        auto r = save::read(text, *b, sb);
        REQUIRE(r.is_err());
        CHECK(r.unwrap_err().kind == save::SaveErrorKind::StructureError);
        CHECK(r.unwrap_err().detail == "roles");
    }

    // 2 主公：忠臣被改成主公
    {
        std::string text = base;
        const auto pos = text.find("\"P1\":\"loyalist\"");
        REQUIRE(pos != std::string::npos);
        text.replace(pos, 15, "\"P1\":\"lord\"");

        auto b = make_game(7);
        GameSession sb;
        auto r = save::read(text, *b, sb);
        REQUIRE(r.is_err());
        CHECK(r.unwrap_err().kind == save::SaveErrorKind::StructureError);
        CHECK(r.unwrap_err().detail == "roles");
    }
}

TEST_CASE("save: failed mode or roles validation leaves the target untouched")
{
    auto a = make_game(42);
    make_identity(*a);
    GameSession sa;
    std::string bad = save::write(*a, sa, "deck");
    const auto pos = bad.find("\"P1\":\"loyalist\"");
    REQUIRE(pos != std::string::npos);
    bad.replace(pos, 15, "\"P1\":\"spy\"");

    auto b = make_game(7);
    make_identity(*b);  // 目标预置身份局状态，任何半写都可见
    GameSession sb;
    const std::string before = save::write(*b, sb, "deck");

    auto r = save::read(bad, *b, sb);
    REQUIRE(r.is_err());
    CHECK(save::write(*b, sb, "deck") == before);
}

TEST_CASE("save: mode and roles do not change the deck hash")
{
    auto a = make_game(1);  // 乱斗
    auto b = make_game(1);  // 同牌表、同种子
    make_identity(*b);
    GameSession sa;
    GameSession sb;

    CHECK(save::deck_hash(a->catalog) == save::deck_hash(b->catalog));

    const std::string ba = save::write(*a, sa, "deck");
    const std::string bb = save::write(*b, sb, "deck");
    const auto hash_span = [](const std::string &t)
    {
        const auto k = t.find("\"hash\":");
        REQUIRE(k != std::string::npos);
        const auto begin = k + 7;
        auto end = begin;
        while (end < t.size() && t[end] != ',' && t[end] != '}')
            ++end;
        return t.substr(begin, end - begin);
    };
    CHECK(hash_span(ba) == hash_span(bb));

    // 身份局文本可读，证明指纹未随模式变化
    auto c = make_game(1);
    GameSession sc;
    CHECK(save::read(bb, *c, sc).is_ok());
}

TEST_CASE("save: brawl save with a roles object is a structure error")
{
    auto a = make_game(1);  // 乱斗
    GameSession sa;
    const std::string base = save::write(*a, sa, "deck");

    std::string text = base;
    const auto pos = text.find(",\"entities\":[");
    REQUIRE(pos != std::string::npos);
    text.replace(pos, 13, ",\"roles\":{\"P0\":\"lord\"},\"entities\":[");

    auto b = make_game(7);
    GameSession sb;
    auto r = save::read(text, *b, sb);
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err().kind == save::SaveErrorKind::StructureError);
    CHECK(r.unwrap_err().detail == "roles");
}

TEST_CASE("save: explicit brawl mode loads as brawl")
{
    auto a = make_game(42);
    make_identity(*a);
    GameSession sa;
    std::string text = save::write(*a, sa, "deck");
    const auto pos = text.find("\"mode\":\"identity\"");
    REQUIRE(pos != std::string::npos);
    text.replace(pos, 17, "\"mode\":\"brawl\"");
    text = strip_roles(text);

    auto b = make_game(7);
    GameSession sb;
    REQUIRE(save::read(text, *b, sb).is_ok());
    CHECK(b->mode == GameMode::Brawl);
    CHECK(b->roles.empty());
}
