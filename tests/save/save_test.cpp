#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "card/catalog.hpp"
#include "config/resource.hpp"
#include "entity/hp.hpp"
#include "game/ai/simple.hpp"
#include "game/flow/loop.hpp"
#include "game/flow/table.hpp"
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
    bad.replace(pos, 11, "\"version\":2");

    auto b = make_game(1);
    GameSession sb;
    auto r = save::read(bad, *b, sb);
    REQUIRE(r.is_err());
    CHECK(r.unwrap_err().kind == save::SaveErrorKind::VersionMismatch);
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
