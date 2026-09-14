/**
 * @file   context.cpp
 * @brief  对局上下文取值函数的定义（模式与角色）。
 * @details `mode_of` 的两个重载与 `role_of(GameContext)` 的回落/转发在此实现；
 *          规则数值读取与只读转换保留在 `context.hpp` 内联。
 * @ingroup tkw_game_core
 */

#include "game/core/context.hpp"

#include <string>

namespace tkw
{
    namespace game
    {
        GameMode mode_of(const GameContext &ctx)
        {
            return ctx.mode ? *ctx.mode : GameMode::Brawl;
        }

        GameMode mode_of(const ReadOnlyContext &ctx)
        {
            return ctx.mode ? *ctx.mode : GameMode::Brawl;
        }

        Role role_of(const GameContext &ctx, const std::string &id)
        {
            return role_of(ctx.roles, id);
        }
    }
}
