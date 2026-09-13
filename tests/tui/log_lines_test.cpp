#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>

#include "card/catalog.hpp"
#include "cli/log_lines.hpp"
#include "config/resource.hpp"
#include "entity/event.hpp"
#include "game/ai/simple.hpp"
#include "game/core/card_event.hpp"
#include "game/flow/factory.hpp"
#include "game/flow/loop.hpp"
#include "game/flow/table.hpp"
#include "tui/log_lines.hpp"

namespace
{
    std::unique_ptr<tkw::game::Game> make_game(int players, std::uint32_t seed)
    {
        tkw::game::BuildOptions opt;
        opt.deck = TKW_TEST_RESOURCE_DIR;
        opt.players = players;
        opt.seed = seed;
        auto r = tkw::game::build_game(opt);
        REQUIRE(r.is_ok());
        return std::move(r).unwrap();
    }

    tkw::card::CardDefCatalog load_catalog()
    {
        tkw::config::ResourceStore store(TKW_TEST_RESOURCE_DIR);
        auto r = tkw::card::CardDefCatalog::load(store, "deck");
        REQUIRE(r.is_ok());
        return std::move(r).unwrap();
    }
}

TEST_CASE("tui: log formatters render labels verbatim")
{
    const auto catalog = load_catalog();

    tkw::CardPlayedEvent played;
    played.user = "P0";
    played.def_id = "sha";
    CHECK(tkw::cli::detail::card_played_line(catalog, played) == "[打出] P0 杀");

    tkw::CardDiscardedEvent discarded;
    discarded.entity = "P1";
    discarded.def_id = "sha";
    discarded.kind = tkw::DiscardKind::Judgement;
    CHECK(tkw::cli::detail::card_discarded_line(catalog, discarded) ==
          "[判定] P1 杀");
    discarded.kind = tkw::DiscardKind::Response;
    CHECK(tkw::cli::detail::card_discarded_line(catalog, discarded) ==
          "[打出] P1 杀");
    discarded.kind = tkw::DiscardKind::Normal;
    CHECK(tkw::cli::detail::card_discarded_line(catalog, discarded) ==
          "[弃置] P1 杀");
    discarded.entity = "";
    CHECK(tkw::cli::detail::card_discarded_line(catalog, discarded) ==
          "[弃置] (无) 杀");

    tkw::CardDrawnEvent drawn;
    drawn.entity = "P1";
    drawn.def_id = "sha";
    drawn.kind = tkw::DrawKind::KillReward;
    CHECK(tkw::cli::detail::card_drawn_line(catalog, drawn) ==
          "[击杀奖励] P1 杀");
    drawn.kind = tkw::DrawKind::Normal;
    CHECK(tkw::cli::detail::card_drawn_line(catalog, drawn) == "[摸牌] P1 杀");
    CHECK(tkw::cli::detail::card_drawn_line(catalog, drawn, false) ==
          "[摸牌] P1 未知牌");

    tkw::CardMovedEvent moved;
    moved.from_entity = "";
    moved.to_entity = "P1";
    moved.def_id = "sha";
    moved.from = tkw::Zone::Limbo;
    moved.to = tkw::Zone::Hand;
    CHECK(tkw::cli::detail::card_moved_line(catalog, moved) ==
          "[移牌] (无)(临时区) -> P1(手牌) 杀");

    tkw::EntityDamagedEvent damaged;
    damaged.source = "";
    damaged.target = "P1";
    damaged.amount = 1;
    CHECK(tkw::cli::detail::entity_damaged_line(damaged) ==
          "[伤害] (无来源) -> P1 1");

    tkw::EntityHpChangedEvent hp;
    hp.entity_id = "P1";
    hp.old_cur = 4;
    hp.new_cur = 3;
    hp.max = 4;
    CHECK(tkw::cli::detail::entity_hp_changed_line(hp) == "[体力] P1 4->3/4");

    tkw::EntityDiedEvent died;
    died.entity_id = "P1";
    CHECK(tkw::cli::detail::entity_died_line(died) == "[阵亡] P1");
}

TEST_CASE("tui: log buffer captures subscribed events and survives unbind")
{
    auto game = make_game(2, 1);
    tkw::tui::LogBuffer buffer;
    buffer.bind(*game);

    auto ctx = game->context();
    tkw::game::GameSession state;
    REQUIRE(tkw::game::start_session(ctx, state, "P0", 2).is_ok());
    REQUIRE_FALSE(buffer.lines().empty());
    CHECK(buffer.lines().front().find("[摸牌]") == 0);

    tkw::game::SimpleAI ai;
    REQUIRE(tkw::game::step_session(ctx, ai, state).is_ok());
    CHECK_FALSE(buffer.lines().empty());

    // 先退订再销毁 Game：句柄生命周期 ⊆ 总线生命周期。
    buffer.unbind();
    game.reset();
    CHECK_FALSE(buffer.lines().empty());
}

TEST_CASE("tui: log buffer hides card names of non-human seats")
{
    auto game = make_game(2, 1);
    tkw::tui::LogBuffer buffer;
    buffer.bind(*game, {"P0"});

    auto ctx = game->context();
    tkw::game::GameSession state;
    REQUIRE(tkw::game::start_session(ctx, state, "P0", 2).is_ok());

    bool human_named = false;
    bool other_hidden = false;
    for (const auto &line : buffer.lines())
    {
        if (line.rfind("[摸牌] P0 ", 0) == 0 &&
            line.find("未知牌") == std::string::npos)
            human_named = true;
        if (line.rfind("[摸牌] P1 未知牌", 0) == 0)
            other_hidden = true;
    }
    CHECK(human_named);
    CHECK(other_hidden);
}

TEST_CASE("tui: log buffer keeps only the newest capacity lines")
{
    tkw::tui::LogBuffer buffer(3);
    buffer.push("1");
    buffer.push("2");
    buffer.push("3");
    buffer.push("4");
    buffer.push("5");

    REQUIRE(buffer.lines().size() == 3);
    CHECK(buffer.lines().front() == "3");
    CHECK(buffer.lines().back() == "5");
    CHECK(buffer.capacity() == 3);
}
