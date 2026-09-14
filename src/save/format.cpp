/**
 * @file   format.cpp
 * @brief  存档枚举文本 → 枚举值的定义。
 * @ingroup tkw_save
 */

#include "save/format.hpp"

#include <string_view>

namespace tkw
{
    namespace save
    {
        bool gender_from(std::string_view s, entity::Gender &out)
        {
            if (s == "male")
                out = entity::Gender::Male;
            else if (s == "female")
                out = entity::Gender::Female;
            else
                return false;
            return true;
        }

        bool suit_from(std::string_view s, card::Suit &out)
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

        bool mode_from(std::string_view s, game::GameMode &out)
        {
            if (s == "brawl")
                out = game::GameMode::Brawl;
            else if (s == "identity")
                out = game::GameMode::Identity;
            else
                return false;
            return true;
        }

        bool role_from(std::string_view s, game::Role &out)
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
