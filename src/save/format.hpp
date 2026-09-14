/**
 * @file   format.hpp
 * @brief  存档格式常量与枚举 ↔ 文本映射。
 * @details 版本常量按实际用到的格式特性递增；枚举与文本的双向映射必须保持稳定，
 *          否则旧存档无法读入。
 * @ingroup tkw_save
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
        /** @brief 存档格式标识；与读取端不一致即拒绝。 */
        inline constexpr std::string_view kFormat = "tkw-save";

        /** @brief 基础存档版本号（无任何扩展规则状态时使用）。 */
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

        /**
         * @brief  性别 → 存档文本。
         * @param[in] g 待转换的性别。
         * @return 稳定文本：`"male"` 或 `"female"`。
         */
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

        /**
         * @brief  存档文本 → 性别。
         * @param[in]  s   存档文本。
         * @param[out] out 解析结果；仅在返回 `true` 时写入。
         * @return 是否解析成功。
         * @retval true  `s` 为 `"male"` 或 `"female"`，`out` 已写入。
         * @retval false `s` 非法，`out` 不被修改。
         */
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

        /**
         * @brief  花色 → 存档文本。
         * @param[in] s 待转换的花色。
         * @return 稳定文本：`"spade"`/`"club"`/`"heart"`/`"diamond"`。
         */
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

        /**
         * @brief  存档文本 → 花色。
         * @param[in]  s   存档文本。
         * @param[out] out 解析结果；仅在返回 `true` 时写入。
         * @return 是否解析成功。
         * @retval true  `s` 为四种合法花色文本之一，`out` 已写入。
         * @retval false `s` 非法，`out` 不被修改。
         */
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

        /**
         * @brief  对局模式 → 存档文本。
         * @param[in] m 待转换的对局模式。
         * @return 稳定文本：`"brawl"` 或 `"identity"`。
         */
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

        /**
         * @brief  存档文本 → 对局模式。
         * @param[in]  s   存档文本。
         * @param[out] out 解析结果；仅在返回 `true` 时写入。
         * @return 是否解析成功。
         * @retval true  `s` 为 `"brawl"` 或 `"identity"`，`out` 已写入。
         * @retval false `s` 非法，`out` 不被修改。
         */
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

        /**
         * @brief  角色 → 存档文本。
         * @param[in] r 待转换的角色。
         * @return 稳定文本；`game::Role::None` 返回 `"none"` 作为防御性兜底，
         *         合法存档不写出该值。
         */
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

        /**
         * @brief  存档文本 → 角色。
         * @param[in]  s   存档文本。
         * @param[out] out 解析结果；仅在返回 `true` 时写入。
         * @return 是否解析成功。
         * @retval true  `s` 为四个合法角色文本之一，`out` 已写入。
         * @retval false `s` 未知或为 `"none"`，`out` 不被修改。
         */
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
