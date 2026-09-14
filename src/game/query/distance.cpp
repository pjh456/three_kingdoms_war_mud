/**
 * @file   distance.cpp
 * @brief  `DistanceQuery` 座次距离与装备/技能修正的定义。
 * @details 承载座次环距离、装备摘要与含马修正的距离计算；单表达式距离判定
 *          `distance_le` 保留在 `distance.hpp` 内联。
 * @ingroup tkw_game_query
 */

#include "game/query/distance.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace tkw
{
    namespace game
    {
        int DistanceQuery::seat_distance(
            const ReadOnlyContext &ctx, const std::string &a, const std::string &b)
        {
            const auto ids = ctx.entities->ordered_ids();
            const auto ia = std::find(ids.begin(), ids.end(), a);
            const auto ib = std::find(ids.begin(), ids.end(), b);
            if (ia == ids.end() || ib == ids.end())
                return 0;
            const int n = static_cast<int>(ids.size());
            const int i = static_cast<int>(ia - ids.begin());
            const int j = static_cast<int>(ib - ids.begin());
            const int d = std::abs(i - j);
            return std::min(d, n - d);
        }

        EquipSummary DistanceQuery::summarize_equipment(
            const ReadOnlyContext &ctx, const std::string &entity_id)
        {
            EquipSummary s;
            for (const auto &c : ctx.cards->equip(entity_id))
            {
                const auto def = ctx.catalog->find(c.def_id);
                if (def.is_none())
                    continue;
                const auto &eq = def.unwrap()->equip;
                if (eq.is_none())
                    continue;
                const auto &e = eq.unwrap();
                if (e.slot == card::EquipSlot::Weapon)
                    s.weapon_range = e.range;
                else if (e.slot == card::EquipSlot::OffensiveHorse)
                    s.offensive_horse = true;
                else if (e.slot == card::EquipSlot::DefensiveHorse)
                    s.defensive_horse = true;
            }
            return s;
        }

        int DistanceQuery::distance_between(
            const ReadOnlyContext &ctx, const std::string &from,
            const std::string &to)
        {
            int d = seat_distance(ctx, from, to);
            const auto fs = summarize_equipment(ctx, from);
            const auto ts = summarize_equipment(ctx, to);
            if (fs.offensive_horse)
                --d;
            if (ts.defensive_horse)
                ++d;

            // 锁定技「马术」：from 计算到其他角色的距离再 -1（下限 1 不变）
            if (HeroQuery::has_hero_skill(ctx, from, hero::HeroSkill::MaShu))
                --d;
            return std::max(d, 1);
        }

        bool DistanceQuery::in_attack_range(
            const ReadOnlyContext &ctx, const std::string &from,
            const std::string &to)
        {
            const int range = summarize_equipment(ctx, from).weapon_range;
            return distance_le(ctx, from, to, range > 0 ? range : 1);
        }
    }
}
