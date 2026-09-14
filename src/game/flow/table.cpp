/**
 * @file   table.cpp
 * @brief  `Game` 运行时容器 `context()` 的定义。
 * @ingroup tkw_game_flow
 */

#include "game/flow/table.hpp"

namespace tkw
{
    namespace game
    {
        GameContext Game::context()
        {
            GameContext ctx;
            ctx.bus = &bus;
            ctx.entities = &entities;
            ctx.cards = &cards;
            ctx.catalog = &catalog;
            ctx.rng = rng.get();
            ctx.rules = &rules;
            ctx.mode = &mode;
            ctx.roles = &roles;
            ctx.heroes = &hero_catalog;
            return ctx;
        }
    }
}
