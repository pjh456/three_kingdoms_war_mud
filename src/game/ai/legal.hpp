/**
 * @file   legal.hpp
 * @brief  出牌阶段合法动作枚举：AI 与校验共用的单一事实源。
 * @details 产出的每个动作都应能被 `execute_turn`/`resolve_play` 接受；顺序 =
 *          手牌顺序（确定性）。本层只读 `ctx`，不产生任何状态变更。
 * @ingroup tkw_game_ai
 */

#ifndef INCLUDE_TKW_GAME_LEGAL_HPP
#define INCLUDE_TKW_GAME_LEGAL_HPP

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "card/card.hpp"
#include "card/def.hpp"
#include "card/manager.hpp"
#include "game/core/context.hpp"
#include "game/core/decision.hpp"
#include "game/core/effect.hpp"
#include "game/core/state.hpp"
#include "game/query/distance.hpp"
#include "game/query/equip.hpp"
#include "game/query/hero.hpp"
#include "game/query/judge.hpp"
#include "game/resolve/validate.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 一个候选出牌动作。 */
        struct LegalAction
        {
            card::Card card;                 /**< 主牌（虚拟杀时为两张中的首张） */
            std::vector<std::string> targets; /**< 引擎认可的完整目标集合 */
            std::string second_instance_id; /**< 第二张手牌（丈八蛇矛两张当杀；空 = 普通动作） */
            bool recast = false; /**< 重铸动作：弃置此牌并摸一张（targets 为空） */
            bool converted_sha = false; /**< 单张转化当杀（武圣红牌 / 龙胆闪；来源由引擎按武将判定） */
        };

        /**
         * @brief 实体任一区域是否有牌（拆/顺的目标需有牌可拿）。
         * @param[in] ctx 只读容器视图。
         * @param[in] id  待查实体 id。
         * @return 手牌/装备/判定任一区域非空时 true。
         * @post 本接口不改变任何状态。
         */
        inline bool entity_has_any_card(
            const ReadOnlyContext &ctx, const std::string &id)
        {
            return ctx.cards->hand_size(id) > 0 || ctx.cards->equip_size(id) > 0 ||
                   ctx.cards->judge_size(id) > 0;
        }

        /**
         * @brief  两张手牌当杀（丈八蛇矛）：枚举手牌 pair 候选（手牌序 i<j，确定性）。
         * @param[in] ctx    只读容器视图。
         * @param[in] player 出牌/响应者 id。
         * @return 有序 pair 列表；不满足产出条件时为空。
         * @retval empty 未装备该能力、手牌不足 2 张、牌堆无杀定义或手牌已有真杀。
         * @post 不改变任何状态；返回值为新容器。
         * @note 仅在装备两张当杀能力、手牌无真杀、手牌 ≥2 且牌堆有杀定义（虚拟杀
         *       效果参数的单一事实源）时产出（主动/响应两侧同一口径）；杀次数与
         *       目标合法性归校验层（主动侧 validate_virtual_sha / 响应侧预校验）。
         */
        std::vector<std::pair<card::Card, card::Card>> two_cards_as_sha_pairs(
            const ReadOnlyContext &ctx, const std::string &player);

        /**
         * @brief  枚举 player 出牌阶段所有合法主动动作。
         * @param[in] ctx    只读容器视图。
         * @param[in] player 出牌者 id。
         * @param[in] turn   提供本回合杀次数上限（已达上限则不再产出杀）。
         * @return 合法动作列表，顺序 = 手牌顺序（含重铸候选统一追加在末尾）。
         * @retval empty 无任何合法动作（应结束出牌阶段）。
         * @post 不改变任何状态；调用方负责执行并校验每个动作。
         * @note 覆盖装备、延时锦囊、主动效果牌，以及借刀杀人的双目标特例。
         */
        std::vector<LegalAction> legal_actions(
            const ReadOnlyContext &ctx, const std::string &player, const TurnContext &turn);
    }
}

#endif  // INCLUDE_TKW_GAME_LEGAL_HPP
