#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "game/core/context.hpp"
#include "game/core/roles.hpp"
#include "game/flow/factory.hpp"
#include "util/rng.hpp"

namespace
{
    using namespace tkw::game;

    /** 用固定 seed 建一局（牌表取测试资源目录）。 */
    std::unique_ptr<Game> build(const GameMode mode, int players, std::uint32_t seed)
    {
        BuildOptions opt;
        opt.deck = TKW_TEST_RESOURCE_DIR;
        opt.players = players;
        opt.seed = seed;
        opt.mode = mode;
        auto r = GameFactory::build(opt);
        REQUIRE(r.is_ok());
        return std::move(r).unwrap();
    }
}

TEST_CASE("game: identity role counts sum to players with one lord")
{
    for (int n = 4; n <= 8; ++n)
    {
        const auto counts = roles_for_count(n);
        REQUIRE(counts.is_some());
        const auto c = counts.unwrap();
        CHECK(c.loyalist + c.rebel + c.traitor + 1 == n);
    }
}

TEST_CASE("game: identity role counts reject unsupported player counts")
{
    for (int n : {0, 2, 3, 9, 10})
        CHECK(roles_for_count(n).is_none());
}

TEST_CASE("game: unbound context falls back to brawl and no role")
{
    GameContext ctx;
    CHECK(mode_of(ctx) == GameMode::Brawl);
    CHECK(role_of(ctx, "P0") == Role::None);
    CHECK(role_of(static_cast<const RoleTable *>(nullptr), "P0") == Role::None);
}

TEST_CASE("game: identity build assigns lord to P0 and covers every seat")
{
    const int players = 5;
    auto game = build(GameMode::Identity, players, 7);

    CHECK(game->mode == GameMode::Identity);
    CHECK(role_of(&game->roles, "P0") == Role::Lord);
    CHECK(game->roles.size() == static_cast<std::size_t>(players));

    const auto counts = roles_for_count(players);
    REQUIRE(counts.is_some());
    const auto c = counts.unwrap();

    int lord = 0, loyalist = 0, rebel = 0, traitor = 0;
    for (int seat = 0; seat < players; ++seat)
    {
        const std::string id = "P" + std::to_string(seat);
        CHECK(game->roles.count(id) == 1);
        switch (role_of(&game->roles, id))
        {
        case Role::Lord:
            ++lord;
            break;
        case Role::Loyalist:
            ++loyalist;
            break;
        case Role::Rebel:
            ++rebel;
            break;
        case Role::Traitor:
            ++traitor;
            break;
        default:
            break;
        }
    }
    CHECK(lord == 1);
    CHECK(loyalist == c.loyalist);
    CHECK(rebel == c.rebel);
    CHECK(traitor == c.traitor);

    // GameContext 只读可见：模式与角色经 context() 指针暴露
    auto ctx = game->context();
    CHECK(mode_of(ctx) == GameMode::Identity);
    CHECK(role_of(ctx, "P0") == Role::Lord);
}

TEST_CASE("game: brawl build does not consume rng")
{
    tkw::SeededRng fresh(11);

    auto brawl = build(GameMode::Brawl, 4, 11);
    CHECK(brawl->mode == GameMode::Brawl);
    CHECK(brawl->roles.empty());
    CHECK(brawl->rng->save_state() == fresh.save_state());

    // 身份局用同一随机源洗牌分配角色，状态必然推进
    auto identity = build(GameMode::Identity, 4, 11);
    CHECK(identity->mode == GameMode::Identity);
    CHECK(identity->rng->save_state() != fresh.save_state());
}

TEST_CASE("game: identity build rejects unsupported player counts")
{
    BuildOptions opt;
    opt.deck = TKW_TEST_RESOURCE_DIR;
    opt.mode = GameMode::Identity;

    for (int players : {2, 3})
    {
        opt.players = players;
        auto r = GameFactory::build(opt);
        REQUIRE(r.is_err());
        CHECK(r.unwrap_err().kind == BuildError::Kind::IdentityPlayerCount);
        CHECK(r.unwrap_err().player_index == players);
    }
}
