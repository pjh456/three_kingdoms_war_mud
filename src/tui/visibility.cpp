/**
 * @file   visibility.cpp
 * @brief  TUI 可见性红线实现：按 viewer 视角抽取牌区，对手手牌只出数量。
 * @ingroup tkw_tui
 */
#include "tui/visibility.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace tkw
{
    namespace tui
    {
        std::string zone_names(const ZoneView &zone)
        {
            if (zone.cards.empty())
                return "无";

            std::string out;
            for (std::size_t i = 0; i < zone.cards.size(); ++i)
            {
                if (i > 0)
                    out += "/";
                out += zone.cards[i].display_name;
            }
            return out;
        }

        bool role_visible(const std::vector<std::string> &humans, bool over,
                          const std::string &id, tkw::game::Role role)
        {
            if (humans.empty() || over)
                return true;
            if (role == tkw::game::Role::Lord)
                return true;
            return std::find(humans.begin(), humans.end(), id) != humans.end();
        }

        ZoneView visible_hand(const tkw::game::ReadOnlyContext &ctx,
                              const std::string &viewer,
                              const std::string &target)
        {
            ZoneView view;
            if (ctx.cards == nullptr)
                return view;

            view.count = ctx.cards->hand_size(target);
            if (viewer != target)
                return view;

            view.revealed = true;
            view.cards.reserve(view.count);
            for (const auto &c : ctx.cards->hand(target))
                view.cards.push_back(CardRow{c.instance_id, c.def_id,
                                             tkw::card::display_name(
                                                 ctx.catalog, c.def_id),
                                             c.suit, c.number});
            return view;
        }

        ZoneView public_zone(const tkw::game::ReadOnlyContext &ctx,
                             const std::vector<tkw::card::Card> &zone)
        {
            ZoneView view;
            view.count = zone.size();
            view.revealed = true;
            view.cards.reserve(zone.size());
            for (const auto &c : zone)
                view.cards.push_back(CardRow{c.instance_id, c.def_id,
                                             tkw::card::display_name(
                                                 ctx.catalog, c.def_id),
                                             c.suit, c.number});
            return view;
        }
    }  // namespace tui
}  // namespace tkw
