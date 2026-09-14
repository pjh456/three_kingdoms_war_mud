/**
 * @file equip.hpp
 * @brief 装备区查询：按被动能力解析已装备的牌。
 * @note 经 `catalog` 解析装备牌的定义（本类不持有目录，仅查询）。
 * @ingroup tkw_game_query
 */

#ifndef INCLUDE_TKW_GAME_EQUIP_HPP
#define INCLUDE_TKW_GAME_EQUIP_HPP

#include <algorithm>
#include <limits>
#include <string>

#include "card/def.hpp"
#include "game/core/context.hpp"
#include "game/query/hero.hpp"

namespace tkw
{
    namespace game
    {
        /**
         * @brief 装备区只读查询（静态工具类）。
         * @details 纯函数集合：不持有上下文，`ctx` 由调用方传入；本类不改变任何状态。
         */
        class EquipQuery
        {
        public:
            /** @brief 静态工具类，不可实例化。 */
            EquipQuery() = delete;

            /**
             * @brief  返回实体装备区中第一件带指定能力的装备定义。
             * @param[in] ctx       只读上下文。
             * @param[in] entity_id 实体 id。
             * @param[in] ability   目标被动能力。
             * @return 命中的装备定义指针；无则 `nullptr`。
             * @note  需要读取装备上的判定描述（如八卦阵）时用本函数；返回指针在
             *        目录生命周期内稳定。
             */
            static const card::CardDef *find_equipment(
                const ReadOnlyContext &ctx, const std::string &entity_id,
                card::Ability ability);

            /**
             * @brief  实体装备区是否存在带指定能力的装备。
             * @param[in] ctx       只读上下文。
             * @param[in] entity_id 实体 id。
             * @param[in] ability   目标被动能力。
             * @return 存在时为 true。
             * @post  本接口不改变任何状态。
             */
            static bool has_ability(
                const ReadOnlyContext &ctx, const std::string &entity_id,
                card::Ability ability);

            /**
             * @brief  本回合杀次数上限（诸葛连弩或咆哮 = 不限）。
             * @param[in] ctx    只读上下文。
             * @param[in] player 玩家 id。
             * @return `rules_of(ctx).sha_limit`；带「不限杀」能力或锁定技「咆哮」
             *         时为 `INT_MAX`。
             * @note  回合流程与出牌动作校验的单一采样点；调用方每轮重采样，回合
             *        中途装备连弩当轮即生效，锁定技「咆哮」与连弩同构。
             */
            static int sha_limit(
                const ReadOnlyContext &ctx, const std::string &player);

            /**
             * @brief  方天画戟：杀可在唯一目标外额外指定目标（至多 2 名）的条件。
             * @param[in] ctx            只读上下文。
             * @param[in] player         使用杀的玩家 id。
             * @param[in] cards_consumed 该杀消耗的手牌张数（真杀 1，丈八虚拟杀 2）。
             * @return 带「方天画戟」能力且该杀耗尽全部手牌时为 true。
             * @note  须在打出的杀移出手牌前判定：`hand_size == cards_consumed` 即该
             *        杀消耗完手中全部牌（卡面触发条件；丈八虚拟杀 = 最后两张手牌）。
             *        目标数放宽由 `validate_effect_targets` 消费。
             */
            static bool sha_multi_target(
                const ReadOnlyContext &ctx, const std::string &player,
                std::size_t cards_consumed = 1);

            /**
             * @brief  实体装备区是否存在指定槽位的装备（如借刀杀人的武器）。
             * @param[in] ctx       只读上下文。
             * @param[in] entity_id 实体 id。
             * @param[in] slot      装备槽位。
             * @return 存在该槽位装备时为 true。
             * @post  本接口不改变任何状态。
             */
            static bool has_equip_slot(
                const ReadOnlyContext &ctx, const std::string &entity_id,
                card::EquipSlot slot);
        };

        inline bool EquipQuery::has_ability(
            const ReadOnlyContext &ctx, const std::string &entity_id,
            card::Ability ability)
        {
            return find_equipment(ctx, entity_id, ability) != nullptr;
        }

        inline bool EquipQuery::sha_multi_target(
            const ReadOnlyContext &ctx, const std::string &player,
            std::size_t cards_consumed)
        {
            return has_ability(ctx, player, card::Ability::MultiTargetSha) &&
                   ctx.cards->hand_size(player) == cards_consumed;
        }
    }
}

#endif  // INCLUDE_TKW_GAME_EQUIP_HPP
