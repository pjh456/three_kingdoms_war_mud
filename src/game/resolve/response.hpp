/**
 * @file response.hpp
 * @brief 响应窗口：目标打出「杀/闪」等响应牌。
 * @note 响应牌的消费（移除+弃置+事件）由结算层负责（单一写者）：单牌窗口
 *       在本模块，杀响应窗口（含两张手牌当杀）在 resolve 层的 respond_sha；
 *       是否响应由 DecisionSource 决定。
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
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /**
         * @brief 实体手牌中是否存在指定响应牌。
         * @note 杀响应额外计入「装备两张当杀能力且手牌 ≥2」（丈八蛇矛打出侧）。
         */
        inline bool has_response_card(
            const GameContext &ctx, const std::string &entity_id, card::ResponseKind kind)
        {
            if (any_hand_card_matching(
                    ctx, entity_id,
                    [kind](const card::CardDef &def)
                    { return is_response_def(def, kind); }))
                return true;
            return kind == card::ResponseKind::Sha &&
                   has_ability(ctx, entity_id, card::Ability::TwoCardsAsSha) &&
                   ctx.cards->hand_size(entity_id) >= 2;
        }

        /**
         * @brief 开响应窗口：先看实体是否有响应牌，有则询问决策源具体打哪张，
         *        校验后消费（移除+弃置）。返回实际消费的牌；None = 未响应。
         * @note 单牌窗口（闪等）：只消费第一张；杀响应窗口走 resolve 层的
         *       respond_sha（另支持两张手牌当杀）。
         */
        inline Option<card::Card> consume_response(
            GameContext &ctx, DecisionSource &ai,
            const std::string &entity_id, card::ResponseKind kind)
        {
            if (!has_response_card(ctx, entity_id, kind))
                return Option<card::Card>::None();
            const auto chosen = ai.play_response(ctx, entity_id, kind);
            if (chosen.is_none())
                return Option<card::Card>::None();

            return consume_hand_card_matching(
                ctx, entity_id, chosen.unwrap().instance_id,
                [kind](const card::CardDef &def) { return is_response_def(def, kind); });
        }

        /**
         * @brief 开响应窗口并消费响应牌。返回是否成功响应。
         */
        inline bool request_response(
            GameContext &ctx, DecisionSource &ai,
            const std::string &entity_id, card::ResponseKind kind)
        {
            return consume_response(ctx, ai, entity_id, kind).is_some();
        }
    }
}

#endif  // INCLUDE_TKW_GAME_RESPONSE_HPP