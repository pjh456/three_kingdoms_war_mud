#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>

#include "card/card.hpp"
#include "card/catalog.hpp"
#include "card/def.hpp"
#include "game/core/context.hpp"
#include "game/flow/factory.hpp"
#include "game/flow/loop.hpp"
#include "game/flow/table.hpp"
#include "tui/visibility.hpp"

namespace
{
    std::unique_ptr<tkw::game::Game> make_game(int players, std::uint32_t seed)
    {
        tkw::game::BuildOptions opt;
        opt.deck = TKW_TEST_RESOURCE_DIR;
        opt.players = players;
        opt.seed = seed;
        auto r = tkw::game::GameFactory::build(opt);
        REQUIRE(r.is_ok());
        return std::move(r).unwrap();
    }
}

TEST_CASE("tui: visible_hand hides opponent hand but reveals own")
{
    auto game = make_game(2, 1);
    auto ctx = game->context();
    tkw::game::GameSession state;
    REQUIRE(tkw::game::GameSetup(ctx).start_session(state, "P0", 2).is_ok());

    // 额外给 P1 一张，验证数量口径取自真实手牌而非展开副本。
    game->cards.add_to_hand("P1", tkw::card::Card{
        "sha#extra", "sha", tkw::card::Suit::Spade, 7});

    const tkw::game::ReadOnlyContext ro = ctx;

    const auto opponent = tkw::tui::visible_hand(ro, "P0", "P1");
    CHECK(opponent.cards.empty());
    CHECK(opponent.count == game->cards.hand_size("P1"));
    CHECK(opponent.count == 3);
    CHECK_FALSE(opponent.revealed);

    const auto own = tkw::tui::visible_hand(ro, "P0", "P0");
    CHECK(own.revealed);
    CHECK(own.cards.size() == game->cards.hand_size("P0"));
    CHECK(own.count == game->cards.hand_size("P0"));
    REQUIRE_FALSE(own.cards.empty());
    CHECK_FALSE(own.cards.front().display_name.empty());
}

TEST_CASE("tui: public_zone reveals equipment and judge zones")
{
    auto game = make_game(2, 1);
    auto ctx = game->context();
    tkw::game::GameSession state;
    REQUIRE(tkw::game::GameSetup(ctx).start_session(state, "P0", 2).is_ok());

    game->cards.add_to_equip("P0", tkw::card::Card{
        "eq#0", "sha", tkw::card::Suit::Spade, 1});
    const tkw::game::ReadOnlyContext ro = ctx;

    const auto equip = tkw::tui::public_zone(ro, ro.cards->equip("P0"));
    CHECK(equip.revealed);
    CHECK(equip.count == ro.cards->equip_size("P0"));
    CHECK(equip.cards.size() == equip.count);
    REQUIRE_FALSE(equip.cards.empty());
    CHECK(equip.cards.front().display_name == "杀");

    const auto judge = tkw::tui::public_zone(ro, ro.cards->judge("P0"));
    CHECK(judge.revealed);
    CHECK(judge.count == 0);
    CHECK(judge.cards.empty());

    // 数据 → 文本接缝：目录展示名以斜杠连接，空区回落「无」。
    CHECK(tkw::tui::zone_names(equip) == "杀");
    CHECK(tkw::tui::zone_names(judge) == "无");
}

TEST_CASE("tui: zone_names joins revealed cards and falls back to none")
{
    tkw::tui::ZoneView named;
    named.revealed = true;
    named.count = 2;
    named.cards.push_back(tkw::tui::CardRow{
        "i", "sha", "杀", tkw::card::Suit::Spade, 1});
    named.cards.push_back(tkw::tui::CardRow{
        "j", "shan", "闪", tkw::card::Suit::Heart, 2});
    CHECK(tkw::tui::zone_names(named) == "杀/闪");

    const tkw::tui::ZoneView empty;
    CHECK(tkw::tui::zone_names(empty) == "无");
}

TEST_CASE("tui: visible_hand tolerates unknown viewer without leaking")
{
    auto game = make_game(2, 1);
    auto ctx = game->context();
    tkw::game::GameSession state;
    REQUIRE(tkw::game::GameSetup(ctx).start_session(state, "P0", 2).is_ok());
    const tkw::game::ReadOnlyContext ro = ctx;

    const auto view = tkw::tui::visible_hand(ro, "PX", "P0");
    CHECK_FALSE(view.revealed);
    CHECK(view.cards.empty());
    CHECK(view.count == game->cards.hand_size("P0"));
}
