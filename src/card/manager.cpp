/**
 * @file   manager.cpp
 * @brief  牌区与牌堆容器 `CardManager` 的实现。
 * @ingroup tkw_card
 */

#include "card/manager.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tkw
{
    namespace card
    {
        Option<Card> CardZone::remove(const std::string &instance_id)
        {
            const auto it = std::find_if(
                m_cards.begin(), m_cards.end(),
                [&](const Card &c) { return c.instance_id == instance_id; });
            if (it == m_cards.end())
                return Option<Card>::None();
            Card c = std::move(*it);
            m_cards.erase(it);
            return Option<Card>::Some(std::move(c));
        }

        CardManagerSnapshot CardManager::snapshot() const
        {
            CardManagerSnapshot s;
            s.instance_seq = instance_seq;
            s.draw = draw_pile.view();
            s.discard = discard_pile.view();
            s.hand = zone_snapshot(zones_of(Zone::Hand));
            s.equip = zone_snapshot(zones_of(Zone::Equip));
            s.judge = zone_snapshot(zones_of(Zone::Judge));
            return s;
        }

        void CardManager::restore(const CardManagerSnapshot &s)
        {
            clear();
            instance_seq = s.instance_seq;
            for (const auto &c : s.draw)
                draw_pile.push(c);
            for (const auto &c : s.discard)
                discard_pile.push(c);
            restore_zone(zones_of(Zone::Hand), s.hand);
            restore_zone(zones_of(Zone::Equip), s.equip);
            restore_zone(zones_of(Zone::Judge), s.judge);
        }

        void CardManager::clear()
        {
            instance_seq = 0;
            draw_pile = CardStack{};
            discard_pile = CardStack{};
            for (auto &zones : entity_zones)
                zones.clear();
        }

        void CardManager::refill_draw(Rng &rng)
        {
            while (true)
            {
                auto c = discard_pile.pop();
                if (c.is_none())
                    break;
                draw_pile.push(std::move(c).unwrap());
            }
            if (draw_pile.size() > 1)
                draw_pile.shuffle(rng);
        }

        Option<Card> CardManager::remove_from_any(
            const std::string &entity_id, const std::string &instance_id,
            Zone *from)
        {
            for (const Zone z : kSlotZones)
            {
                if (auto c = remove_from_zone(z, entity_id, instance_id);
                    c.is_some())
                {
                    if (from)
                        *from = z;
                    return c;
                }
            }
            return Option<Card>::None();
        }

        bool CardManager::has_card(
            const std::string &entity_id, const std::string &instance_id) const
        {
            for (const Zone z : kSlotZones)
            {
                const auto *zone = find_zone(z, entity_id);
                if (zone == nullptr)
                    continue;
                for (const auto &c : zone->view())
                    if (c.instance_id == instance_id)
                        return true;
            }
            return false;
        }

        std::vector<Card> CardManager::discard_all(const std::string &entity_id)
        {
            std::vector<Card> out;
            for (const Zone z : kSlotZones)
            {
                ZoneMap &zones = zones_of(z);
                auto it = zones.find(entity_id);
                if (it == zones.end())
                    continue;
                auto cards = it->second.drain();
                for (auto &c : cards)
                {
                    out.push_back(c);
                    discard_pile.push(std::move(c));
                }
                zones.erase(it);
            }
            return out;
        }

        std::vector<std::pair<std::string, std::vector<Card>>>
        CardManager::zone_snapshot(const ZoneMap &zones)
        {
            std::vector<std::pair<std::string, std::vector<Card>>> out;
            out.reserve(zones.size());
            for (const auto &[id, zone] : zones)
            {
                if (zone.empty())  // 空区域不入快照（规范化，便于往返稳定）
                    continue;
                out.emplace_back(id, zone.view());
            }
            std::sort(out.begin(), out.end(),
                      [](const auto &a, const auto &b)
                      { return a.first < b.first; });
            return out;
        }

        void CardManager::restore_zone(
            ZoneMap &zones,
            const std::vector<std::pair<std::string, std::vector<Card>>> &in)
        {
            for (const auto &[id, cards] : in)
                for (const auto &c : cards)
                    zones[id].add(c);
        }

        const std::vector<Card> &CardManager::empty_list()
        {
            static const std::vector<Card> empty;
            return empty;
        }

        CardZone *CardManager::find_zone(Zone zone, const std::string &entity_id)
        {
            ZoneMap &zones = zones_of(zone);
            auto it = zones.find(entity_id);
            return it == zones.end() ? nullptr : &it->second;
        }

        const CardZone *CardManager::find_zone(
            Zone zone, const std::string &entity_id) const
        {
            const ZoneMap &zones = zones_of(zone);
            auto it = zones.find(entity_id);
            return it == zones.end() ? nullptr : &it->second;
        }

        Card CardManager::make_card(const std::string &def_id, const CardCopy &copy)
        {
            return Card{
                def_id + "#" + std::to_string(instance_seq++), def_id, copy.suit,
                copy.number};
        }
    }
}
