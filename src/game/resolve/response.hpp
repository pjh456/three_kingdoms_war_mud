/**
 * @file response.hpp
 * @brief 响应窗口：目标打出「杀/闪」等响应牌。
 * @details 响应牌的消费（移除+弃置+事件）由结算层负责（单一写者）：单牌窗口
 *          在本模块，杀响应窗口（含两张手牌当杀）在 resolve 层的 respond_sha；
 *          是否响应由 DecisionSource 决定。
 * @ingroup tkw_game_resolve
 */

#ifndef INCLUDE_TKW_GAME_RESPONSE_HPP
#define INCLUDE_TKW_GAME_RESPONSE_HPP

#include <string>
#include <utility>

#include "card/catalog.hpp"
#include "card/def.hpp"
#include "card/manager.hpp"
#include "game/core/context.hpp"
#include "game/core/decision.hpp"
#include "game/core/effect.hpp"
#include "game/core/state.hpp"
#include "game/query/equip.hpp"
#include "game/query/hero.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /**
         * @brief 实体手牌中是否存在指定响应牌。
         * @param[in] ctx       只读上下文。
         * @param[in] entity_id 被询问的实体 id。
         * @param[in] kind      需要的响应牌类别（杀/闪）。
         * @return 存在可响应的牌时为 true；否则 false。
         * @retval true 手牌含该类别真牌，或满足本类别的转化/两张当杀条件。
         * @retval false 手牌与转化来源均不满足。
         * @note 杀响应额外计入「装备两张当杀能力且手牌 ≥2」（丈八蛇矛打出侧）
         *       与转换来源（武圣红牌 / 龙胆闪当杀）；闪响应额外计入转化来源
         *       （龙胆杀当闪）。
         */
        inline bool has_response_card(
            const GameContext &ctx, const std::string &entity_id, card::ResponseKind kind)
        {
            if (any_hand_card_matching(
                    ctx, entity_id,
                    [kind](const card::CardDef &def)
                    { return is_response_def(def, kind); }))
                return true;
            if (kind != card::ResponseKind::Sha)
                return !jink_conversion_cards(ctx, entity_id).empty();
            if (has_ability(ctx, entity_id, card::Ability::TwoCardsAsSha) &&
                ctx.cards->hand_size(entity_id) >= 2)
                return true;
            return !sha_conversion_cards(ctx, entity_id).empty();
        }

        /**
         * @brief 开响应窗口并消费响应牌。
         * @details 先看实体是否有响应牌，有则询问决策源具体打哪张，校验后消费
         *          （移除+弃置）。返回实际消费的牌；`None` = 未响应。
         * @param[in]  ctx       对局上下文（含卡牌管理器、事件总线）。
         * @param[in]  ai        决策源；询问具体打出哪张牌。
         * @param[in]  entity_id 被询问的实体 id。
         * @param[in]  kind      需要的响应牌类别（闪等）。
         * @param[in]  prompt    响应来源与后果（只读事实，透传给决策源做窗口文案）。
         * @return 实际消费的响应牌；`None` = 未响应。
         * @retval Some 引擎已校验并消费该牌（移除+弃置+发事件）。
         * @retval None 无响应牌，或决策源放弃，或选择非法；此时不改变状态。
         * @post 返回 `Some` 时该牌已移出手牌并进入弃牌堆；返回 `None` 时不改变状态。
         * @note 单牌窗口（闪等）：只消费第一张；杀响应窗口走 resolve 层的
         *       respond_sha（另支持两张手牌当杀）。闪窗口的转化来源（龙胆杀
         *       当闪）由谓词识别，消费的仍是所选那张牌。
         */
        inline Option<card::Card> consume_response(
            GameContext &ctx, DecisionSource &ai,
            const std::string &entity_id, card::ResponseKind kind,
            const ResponsePrompt &prompt)
        {
            if (!has_response_card(ctx, entity_id, kind))
                return Option<card::Card>::None();
            const auto chosen = ai.play_response(ctx, entity_id, kind, prompt);
            if (chosen.is_none())
                return Option<card::Card>::None();

            return consume_hand_card_matching(
                ctx, entity_id, chosen.unwrap().instance_id,
                [&ctx, &entity_id, kind](const card::CardDef &def,
                                         const card::Card &c)
                {
                    return is_response_def(def, kind) ||
                           (kind == card::ResponseKind::Jink &&
                            can_convert_card_to_jink(ctx, entity_id, c, def));
                },
                DiscardKind::Response);
        }

        /**
         * @brief 开响应窗口并消费响应牌。
         * @details 委托 `consume_response`，把「是否消费到牌」折叠为布尔结果。
         * @param[in] ctx       对局上下文。
         * @param[in] ai        决策源；询问具体打出哪张牌。
         * @param[in] entity_id 被询问的实体 id。
         * @param[in] kind      需要的响应牌类别。
         * @param[in] prompt    响应来源与后果（只读事实，透传决策源）。
         * @return 成功消费到响应牌时为 true；否则 false。
         * @retval true  已消费一张响应牌。
         * @retval false 未响应或选择非法，状态不变。
         * @post 返回 true 时已消费该牌；返回 false 时不改变状态。
         */
        inline bool request_response(
            GameContext &ctx, DecisionSource &ai,
            const std::string &entity_id, card::ResponseKind kind,
            const ResponsePrompt &prompt)
        {
            return consume_response(ctx, ai, entity_id, kind, prompt).is_some();
        }
    }
}

#endif  // INCLUDE_TKW_GAME_RESPONSE_HPP