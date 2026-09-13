#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>

#include "cli/session.hpp"
#include "game/ai/simple.hpp"
#include "game/core/roles.hpp"
#include "game/flow/factory.hpp"
#include "game/flow/loop.hpp"
#include "game/flow/table.hpp"
#include "tui/snapshot.hpp"

namespace
{
    std::unique_ptr<tkw::game::Game> make_game(
        int players, std::uint32_t seed,
        tkw::game::GameMode mode = tkw::game::GameMode::Brawl)
    {
        tkw::game::BuildOptions opt;
        opt.deck = TKW_TEST_RESOURCE_DIR;
        opt.players = players;
        opt.seed = seed;
        opt.mode = mode;
        auto r = tkw::game::build_game(opt);
        REQUIRE(r.is_ok());
        return std::move(r).unwrap();
    }

    /** 建局并开局发牌的会话（viewer 视角测试用）。 */
    tkw::cli::Session started_session(
        int players, std::uint32_t seed, int hand,
        tkw::game::GameMode mode = tkw::game::GameMode::Brawl)
    {
        tkw::cli::Session s;
        s.active = true;
        s.deck = TKW_TEST_RESOURCE_DIR;
        s.game = make_game(players, seed, mode);
        auto ctx = s.game->context();
        REQUIRE(tkw::game::start_session(ctx, s.state, "P0", hand).is_ok());
        return s;
    }

    /** 只建局的会话（自行 play_game 跑完，避免重复开局发牌）。 */
    tkw::cli::Session raw_session(
        int players, std::uint32_t seed,
        tkw::game::GameMode mode = tkw::game::GameMode::Brawl)
    {
        tkw::cli::Session s;
        s.active = true;
        s.deck = TKW_TEST_RESOURCE_DIR;
        s.game = make_game(players, seed, mode);
        return s;
    }
}

TEST_CASE("tui: empty session yields inactive empty snapshot")
{
    tkw::cli::Session s;
    const auto snap = tkw::tui::make_snapshot(s, "P0");
    CHECK_FALSE(snap.active);
    CHECK(snap.players.empty());
    CHECK(snap.draw_size == 0);
    CHECK(snap.discard_size == 0);
}

TEST_CASE("tui: snapshot mirrors seats, hands and deck counts")
{
    auto s = started_session(2, 1, 2);
    const auto snap = tkw::tui::make_snapshot(s, "P0");

    REQUIRE(snap.active);
    CHECK(snap.mode == tkw::game::GameMode::Brawl);
    CHECK(snap.turns == 0);
    CHECK(snap.current == "P0");
    CHECK_FALSE(snap.over);
    CHECK(snap.alive == 2);
    CHECK(snap.draw_size == 108 - 2 * 2);
    CHECK(snap.discard_size == 0);

    REQUIRE(snap.players.size() == 2);
    CHECK(snap.players[0].id == "P0");
    CHECK(snap.players[1].id == "P1");
    CHECK(snap.players[0].seat == 0);
    CHECK(snap.players[1].seat == 1);

    // viewer = P0：己方展开且数量一致，对手只出数量。
    CHECK(snap.players[0].hand.revealed);
    CHECK(snap.players[0].hand.count == s.game->cards.hand_size("P0"));
    CHECK(snap.players[0].hand.cards.size() == snap.players[0].hand.count);
    CHECK_FALSE(snap.players[1].hand.revealed);
    CHECK(snap.players[1].hand.count == s.game->cards.hand_size("P1"));
    CHECK(snap.players[1].hand.cards.empty());
}

TEST_CASE("tui: snapshot advances turn progress after one step")
{
    auto s = started_session(2, 1, 2);
    tkw::game::SimpleAI ai;
    auto ctx = s.game->context();
    REQUIRE(tkw::game::step_session(ctx, ai, s.state).is_ok());

    const auto snap = tkw::tui::make_snapshot(s, "P0");
    CHECK(snap.turns == 1);
    CHECK(snap.current == "P1");
    CHECK_FALSE(snap.over);
}

TEST_CASE("tui: identity snapshot exposes per-seat roles")
{
    auto s = started_session(4, 1, 2, tkw::game::GameMode::Identity);
    const auto snap = tkw::tui::make_snapshot(s, "P0");

    CHECK(snap.mode == tkw::game::GameMode::Identity);
    REQUIRE(snap.players.size() == 4);
    for (const auto &row : snap.players)
    {
        CHECK(row.role != tkw::game::Role::None);
        CHECK(row.role == s.game->roles.at(row.id));
    }
}

TEST_CASE("tui: identity roles hidden for non-human seats until over")
{
    auto s = started_session(4, 1, 2, tkw::game::GameMode::Identity);
    s.humans = {"P1"};
    const auto snap = tkw::tui::make_snapshot(s, "P1");

    REQUIRE(snap.players.size() == 4);
    for (const auto &row : snap.players)
    {
        const auto real = s.game->roles.at(row.id);
        const bool visible =
            row.id == "P1" || real == tkw::game::Role::Lord;
        if (visible)
            CHECK(row.role == real);
        else
            CHECK(row.role == tkw::game::Role::None);
    }
}

TEST_CASE("tui: finished session reports over and a winner label")
{
    auto s = raw_session(2, 1);
    tkw::game::SimpleAI ai;
    auto ctx = s.game->context();
    auto r = tkw::game::play_game(ctx, ai, "P0", 2);
    REQUIRE(r.is_ok());

    const auto snap = tkw::tui::make_snapshot(s, "P0");
    CHECK(snap.over);
    CHECK_FALSE(snap.winner.empty());
    CHECK_FALSE(snap.winner_label.empty());
}
