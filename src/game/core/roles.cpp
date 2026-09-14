/**
 * @file   roles.cpp
 * @brief  身份局角色查表与人数配比的定义。
 * @details 实现 `role_of(RoleTable *)` 的空表/未命中回落与 `roles_for_count`
 *          的标准配比表；枚举与值类型保留在 `roles.hpp`。
 * @ingroup tkw_game_core
 */

#include "game/core/roles.hpp"

#include <string>

namespace tkw
{
    namespace game
    {
        Role role_of(const RoleTable *roles, const std::string &id)
        {
            if (roles == nullptr)
                return Role::None;
            const auto it = roles->find(id);
            return it == roles->end() ? Role::None : it->second;
        }

        Option<RoleCounts> roles_for_count(int n)
        {
            switch (n)
            {
            case 4:
                return Option<RoleCounts>::Some(RoleCounts{1, 1, 1});
            case 5:
                return Option<RoleCounts>::Some(RoleCounts{1, 2, 1});
            case 6:
                return Option<RoleCounts>::Some(RoleCounts{1, 3, 1});
            case 7:
                return Option<RoleCounts>::Some(RoleCounts{2, 3, 1});
            case 8:
                return Option<RoleCounts>::Some(RoleCounts{2, 4, 1});
            default:
                return Option<RoleCounts>::None();
            }
        }
    }
}
