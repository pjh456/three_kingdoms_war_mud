#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "card/catalog.hpp"
#include "config/resource.hpp"
#include "entity/hp.hpp"
#include "game/ai/simple.hpp"
#include "game/flow/loop.hpp"
#include "game/flow/table.hpp"
#include "io/file.hpp"
#include "save/error.hpp"
#include "save/reader.hpp"
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
