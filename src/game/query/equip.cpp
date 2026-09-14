/**
 * @file   equip.cpp
 * @brief  `EquipQuery` 装备区被动能力查询的定义。
 * @details 承载装备定义查找、杀次数上限与槽位存在性判定；单表达式谓词
 *          `has_ability`/`sha_multi_target` 保留在 `equip.hpp` 内联。
 * @ingroup tkw_game_query
 */

#include "game/query/equip.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace tkw
{
    namespace game
    {
        const card::CardDef *EquipQuery::find_equipment(
            const ReadOnlyContext &ctx, const std::string &entity_id,
            card::Ability ability)
        {
            for (const auto &c : ctx.cards->equip(entity_id))
            {
                const auto def = ctx.catalog->find(c.def_id);
                if (def.is_none())
                    continue;
                const card::CardDef &d = *def.unwrap();
                if (std::find(d.abilities.begin(), d.abilities.end(), ability) !=
                    d.abilities.end())
                    return &d;
            }
            return nullptr;
        }

        int EquipQuery::sha_limit(
            const ReadOnlyContext &ctx, const std::string &player)
        {
            if (has_ability(ctx, player, card::Ability::NoShaLimit) ||
                HeroQuery::has_hero_skill(ctx, player, hero::HeroSkill::PaoXiao))
                return std::numeric_limits<int>::max();
            return rules_of(ctx).sha_limit;
        }

        bool EquipQuery::has_equip_slot(
            const ReadOnlyContext &ctx, const std::string &entity_id,
            card::EquipSlot slot)
        {
            for (const auto &c : ctx.cards->equip(entity_id))
            {
                const auto def = ctx.catalog->find(c.def_id);
                if (def.is_some() && def.unwrap()->equip.is_some() &&
                    def.unwrap()->equip.unwrap().slot == slot)
                    return true;
            }
            return false;
        }
    }
}
