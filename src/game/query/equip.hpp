/**
 * @file equip.hpp
 * @brief 装备区查询：按被动能力解析已装备的牌。
 * @note 经 catalog 解析装备牌的定义（本模块不持有目录，仅查询）。
 */

#ifndef INCLUDE_TKW_GAME_EQUIP_HPP
#define INCLUDE_TKW_GAME_EQUIP_HPP

#include <algorithm>
#include <limits>
#include <string>

#include "card/def.hpp"
#include "game/core/context.hpp"

namespace tkw
{
    namespace game
    {
        /**
         * @brief 返回实体装备区中第一件带指定能力的装备定义；无则 nullptr。
         * @note 需要读取装备上的判定描述（如八卦阵）时用本函数。
         */
        inline const card::CardDef *find_equipment(
            const ReadOnlyContext &ctx,
            const std::string &entity_id,
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

        /** @brief 实体装备区是否存在带指定能力的装备。 */
        inline bool has_ability(
            const ReadOnlyContext &ctx,
            const std::string &entity_id,
            card::Ability ability)
        {
            return find_equipment(ctx, entity_id, ability) != nullptr;
        }

        /**
         * @brief 本回合杀次数上限（诸葛连弩 = 不限）。
         * @note 回合流程与出牌动作校验的单一采样点；调用方每轮重采样，
         *       回合中途装备连弩当轮即生效。
         */
        inline int sha_limit(const ReadOnlyContext &ctx, const std::string &player)
        {
            if (has_ability(ctx, player, card::Ability::NoShaLimit))
                return std::numeric_limits<int>::max();
            return rules_of(ctx).sha_limit;
        }

        /**
         * @brief 方天画戟：杀可在唯一目标外额外指定目标（至多 2 名）的条件。
         * @param cards_consumed 该杀消耗的手牌张数（真杀 1，丈八虚拟杀 2）。
         * @note 须在打出的杀移出手牌前判定：hand_size == cards_consumed 即该杀
         *       消耗完手中全部牌（卡面触发条件；丈八虚拟杀 = 最后两张手牌）。
         *       目标数放宽由 validate_effect_targets 消费。
         */
        inline bool sha_multi_target(
            const ReadOnlyContext &ctx, const std::string &player,
            std::size_t cards_consumed = 1)
        {
            return has_ability(ctx, player, card::Ability::MultiTargetSha) &&
                   ctx.cards->hand_size(player) == cards_consumed;
        }

        /** @brief 实体装备区是否存在指定槽位的装备（如借刀杀人的武器）。 */
        inline bool has_equip_slot(
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

#endif  // INCLUDE_TKW_GAME_EQUIP_HPP
