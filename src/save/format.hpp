/**
 * @file format.hpp
 * @brief 存档格式常量与枚举 ↔ 文本映射。
 */

#ifndef INCLUDE_TKW_SAVE_FORMAT_HPP
#define INCLUDE_TKW_SAVE_FORMAT_HPP

#include <string_view>

#include "card/def.hpp"
#include "entity/base.hpp"
#include "game/core/roles.hpp"

namespace tkw
{
    namespace save
    {
        inline constexpr std::string_view kFormat = "tkw-save";
        inline constexpr int kVersion = 1;

        /**
         * @brief 含连环状态时使用的存档版本。
         * @note 版本号表示实际用到的格式特性：无任何实体处于连环时写 kVersion，
         *       标准档逐字节不变；存在连环实体时写本值，旧二进制显式拒绝而非
         *       静默丢弃规则状态。读取端同时接受 kVersion 与本值。
         */
        inline constexpr int kVersionChained = 2;

        /**
         * @brief 含武将选择时使用的存档版本。
         * @note 武将选择为规则状态（影响技能行为），与连环同口径显式拒绝旧二进制
         *       的静默丢弃；实际特性同时出现时取最高版本。读取端接受 kVersion、
         *       kVersionChained 与本值。
         */
        inline constexpr int kVersionHeroes = 3;

        /** @brief 性别 → 存档文本。 */
        inline constexpr const char *gender_name(entity::Gender g)
        {
            switch (g)
            {
            case entity::Gender::Male:
                return "male";
            case entity::Gender::Female:
                return "female";
            }
            return "male";
        }

        /** @brief 存档文本 → 性别；未知返回 false。 */
        inline bool gender_from(std::string_view s, entity::Gender &out)
        {
            if (s == "male")
                out = entity::Gender::Male;
            else if (s == "female")
                out = entity::Gender::Female;
            else
                return false;
            return true;
        }

        /** @brief 花色 → 存档文本。 */
        inline constexpr const char *suit_name(card::Suit s)
        {
            switch (s)
            {
            case card::Suit::Spade:
                return "spade";
            case card::Suit::Club:
                return "club";
            case card::Suit::Heart:
                return "heart";
            case card::Suit::Diamond:
                return "diamond";
            }
            return "spade";
        }

        /** @brief 存档文本 → 花色；未知返回 false。 */
        inline bool suit_from(std::string_view s, card::Suit &out)
        {
            if (s == "spade")
                out = card::Suit::Spade;
            else if (s == "club")
                out = card::Suit::Club;
            else if (s == "heart")
                out = card::Suit::Heart;
            else if (s == "diamond")
                out = card::Suit::Diamond;
            else
                return false;
            return true;
        }

        /** @brief 对局模式 → 存档文本。 */
        inline constexpr const char *mode_name(game::GameMode m)
        {
            switch (m)
            {
            case game::GameMode::Brawl:
                return "brawl";
            case game::GameMode::Identity:
                return "identity";
            }
            return "brawl";
        }

        /** @brief 存档文本 → 对局模式；未知返回 false。 */
        inline bool mode_from(std::string_view s, game::GameMode &out)
        {
            if (s == "brawl")
                out = game::GameMode::Brawl;
            else if (s == "identity")
                out = game::GameMode::Identity;
            else
                return false;
            return true;
        }

        /** @brief 角色 → 存档文本（None 为防御性兜底，合法存档不写出）。 */
        inline constexpr const char *role_name(game::Role r)
        {
            switch (r)
            {
            case game::Role::Lord:
                return "lord";
            case game::Role::Loyalist:
                return "loyalist";
            case game::Role::Rebel:
                return "rebel";
            case game::Role::Traitor:
                return "traitor";
            case game::Role::None:
                return "none";
            }
            return "none";
        }

        /** @brief 存档文本 → 角色；仅四个合法角色文本，未知与 "none" 返回 false。 */
        inline bool role_from(std::string_view s, game::Role &out)
        {
            if (s == "lord")
                out = game::Role::Lord;
            else if (s == "loyalist")
                out = game::Role::Loyalist;
            else if (s == "rebel")
                out = game::Role::Rebel;
            else if (s == "traitor")
                out = game::Role::Traitor;
            else
                return false;
            return true;
        }
    }
}

#endif  // INCLUDE_TKW_SAVE_FORMAT_HPP
