/**
 * @file format.hpp
 * @brief 存档格式常量与枚举 ↔ 文本映射。
 */

#ifndef INCLUDE_TKW_SAVE_FORMAT_HPP
#define INCLUDE_TKW_SAVE_FORMAT_HPP

#include <string_view>

#include "card/def.hpp"

namespace tkw
{
    namespace save
    {
        inline constexpr std::string_view kFormat = "tkw-save";
        inline constexpr int kVersion = 1;

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
    }
}

#endif  // INCLUDE_TKW_SAVE_FORMAT_HPP
