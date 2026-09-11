/**
 * @file counter.hpp
 * @brief 无懈可击：抵消一张锦囊牌对一名角色产生的效果。
 * @note 规则简化实现：
 *       - 从推导起点（锦囊使用者非空 → 该玩家；空 → 目标集合首位，即延时锦囊
 *         判定窗口的被判定玩家）起，按座位序轮询「是否出无懈」；
 *       - 每出一张无懈翻转「是否被抵消」状态；一整轮无人出则结算；
 *       - 最后状态 = 出无懈次数的奇偶（链式相抵），true = 被抵消。
 * @note 只抵消锦囊牌（type == Trick），基本牌（杀/闪/桃）不可无懈。
 */

#ifndef INCLUDE_TKW_GAME_COUNTER_HPP
#define INCLUDE_TKW_GAME_COUNTER_HPP

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "card/catalog.hpp"
#include "card/def.hpp"
#include "card/manager.hpp"
#include "game/core/card_event.hpp"
#include "game/core/context.hpp"
#include "game/core/decision.hpp"
#include "game/core/effect.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        inline bool has_counter_card(const GameContext &ctx, const std::string &player)
        {
            for (const auto &c : ctx.cards->hand(player))
            {
                const auto def = ctx.catalog->find(c.def_id);
                if (def.is_some() && is_counter_def(*def.unwrap()))
                    return true;
            }
            return false;
        }

        inline bool consume_counter(
            GameContext &ctx, const std::string &player,
            const std::string &instance_id)
        {
            auto removed = ctx.cards->remove_from_hand(player, instance_id);
            if (removed.is_none())
                return false;
            card::Card card = std::move(removed).unwrap();
            const auto def = ctx.catalog->find(card.def_id);
            if (def.is_none() || !is_counter_def(*def.unwrap()))
            {
                ctx.cards->add_to_hand(player, std::move(card));  // 非法选择退回
                return false;
            }
            ctx.cards->discard(card);
            emit_card_discarded(ctx, player, card);
            return true;
        }

        /** @brief 座位序（从 start 开始环绕）。 */
        inline std::vector<std::string> seat_order_from(
            const GameContext &ctx, const std::string &start)
        {
            return ctx.entities->order_from(start);
        }

        /** @brief 询问某玩家是否打出无懈（有牌且决定出则消费）。 */
        inline bool try_play_counter(
            GameContext &ctx, DecisionSource &ai, const std::string &player,
            const std::string &trick_user,
            const std::vector<std::string> &trick_targets)
        {
            if (!has_counter_card(ctx, player))
                return false;
            const auto chosen =
                ai.play_counter(ctx, player, trick_user, trick_targets);
            if (chosen.is_none())
                return false;
            return consume_counter(ctx, player, chosen.unwrap());
        }

        /**
         * @brief 无懈响应窗口（链式）。
         * @param trick 被结算的锦囊定义（规则扩展缝：将来可按锦囊/目标定制
         *        可无懈性；当前实现只用它做语义占位）。
         * @param trick_user 锦囊使用者；空串 = 延时锦囊判定窗口（使用者不随牌
         *        记录，窗口主体为被判定玩家）。
         * @param trick_targets 锦囊目标集合（判定窗口 = 被判定玩家一人），
         *        须非空；轮询起点 = 使用者非空 → 使用者，否则 → 首位目标。
         * @return true = 被无懈抵消（奇数张无懈）。
         * @note 窗口粒度 = 每个受影响目标一次（调用方按目标调用）：一张锦囊
         *       可开多个独立窗口，每个目标窗口需各自出奇数无懈才全部抵消；
         *       这是现行规则的简化，与官方「锦囊级抵消」表述不等价。
         */
        inline bool resolve_nullification(
            GameContext &ctx, DecisionSource &ai, const card::CardDef &trick,
            const std::string &trick_user,
            const std::vector<std::string> &trick_targets)
        {
            (void)trick;
            const std::string &start =
                trick_user.empty() ? trick_targets.front() : trick_user;
            const auto order = seat_order_from(ctx, start);
            bool cancelled = false;
            for (int round = 0; round < rules_of(ctx).wuxie_rounds; ++round)
            {
                bool any = false;
                for (const auto &p : order)
                {
                    if (try_play_counter(ctx, ai, p, trick_user, trick_targets))
                    {
                        cancelled = !cancelled;
                        any = true;
                    }
                }
                if (!any)
                    break;
            }
            return cancelled;
        }
    }
}

#endif  // INCLUDE_TKW_GAME_COUNTER_HPP