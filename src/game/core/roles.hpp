/**
 * @file roles.hpp
 * @brief 身份局的对局模式与角色值类型：模式/角色/阵营枚举、人数配比与只读查表。
 * @note 纯值类型，不引 context/table；角色是对局状态，按玩家 id 键控存放于 Game，
 *       不进 Entity（支撑域不承载游戏规则）。
 */

#ifndef INCLUDE_TKW_GAME_ROLES_HPP
#define INCLUDE_TKW_GAME_ROLES_HPP

#include <cstdint>
#include <map>
#include <string>

#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 对局模式。 */
        enum class GameMode : std::uint8_t
        {
            Brawl,    /**< 乱斗：唯一存活者胜 */
            Identity, /**< 身份局：按阵营判定胜负 */
        };

        /** @brief 身份局角色。 */
        enum class Role : std::uint8_t
        {
            None,     /**< 未分配 / 查不到 */
            Lord,     /**< 主公 */
            Loyalist, /**< 忠臣 */
            Rebel,    /**< 反贼 */
            Traitor,  /**< 内奸 */
        };

        /** @brief 胜负阵营。 */
        enum class WinCamp : std::uint8_t
        {
            None,        /**< 未结束 */
            LordCamp,    /**< 主公阵营（主公 + 忠臣） */
            RebelCamp,   /**< 反贼阵营 */
            TraitorCamp, /**< 内奸阵营 */
            Draw,        /**< 同归于尽 */
        };

        /** @brief 玩家 id → 角色表（std::map 迭代序 = id 序，写出确定）。 */
        using RoleTable = std::map<std::string, Role>;

        /**
         * @brief 查角色。
         * @param roles 角色表（可为空指针）。
         * @param id 玩家 id。
         * @return 命中返回对应角色；空表或未命中返回 Role::None。
         */
        inline Role role_of(const RoleTable *roles, const std::string &id)
        {
            if (roles == nullptr)
                return Role::None;
            const auto it = roles->find(id);
            return it == roles->end() ? Role::None : it->second;
        }

        /** @brief 身份局角色配比（主公恒 1 名，其余为忠臣/反贼/内奸）。 */
        struct RoleCounts
        {
            int loyalist = 0; /**< 忠臣数 */
            int rebel = 0;    /**< 反贼数 */
            int traitor = 0;  /**< 内奸数 */
        };

        /**
         * @brief 按人数取身份局角色配比。
         * @param n 玩家数。
         * @return 4→(1,1,1)、5→(1,2,1)、6→(1,3,1)、7→(2,3,1)、8→(2,4,1)；
         *         其余人数返回 None（无标准配比）。
         * @note 主公恒 1 名，故 1 + loyalist + rebel + traitor == n。
         */
        inline Option<RoleCounts> roles_for_count(int n)
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

#endif  // INCLUDE_TKW_GAME_ROLES_HPP
