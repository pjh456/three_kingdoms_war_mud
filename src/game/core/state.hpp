/**
 * @file state.hpp
 * @brief 对局状态的基础操作：扣血/回血/摸牌（不含濒死死亡，那些归 combat.hpp）。
 * @note 这些是「状态层」原语：只改实体状态与牌堆，不发布流程事件（濒死/死亡），
 *       但会发布卡牌域事件（如摸牌）供日志/回放消费。
 */

#ifndef INCLUDE_TKW_GAME_STATE_HPP
#define INCLUDE_TKW_GAME_STATE_HPP

#include <string>
#include <utility>

#include "card/def.hpp"
#include "card/manager.hpp"
#include "entity/manager.hpp"
#include "game/core/card_event.hpp"
#include "game/core/context.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 花色是否为黑（♠/♣）。 */
        inline bool is_black_suit(card::Suit s)
        {
            return s == card::Suit::Spade || s == card::Suit::Club;
        }

        /** @brief 花色是否为红（♥/♦）。 */
        inline bool is_red_suit(card::Suit s)
        {
            return s == card::Suit::Heart || s == card::Suit::Diamond;
        }

        /** @brief 判定牌是否满足触发条件（条件来自数据）。 */
        inline bool judge_triggered(card::JudgeTrigger t, const card::Card &c)
        {
            switch (t)
            {
            case card::JudgeTrigger::Red:
                return is_red_suit(c.suit);
            case card::JudgeTrigger::Black:
                return is_black_suit(c.suit);
            case card::JudgeTrigger::Heart:
                return c.suit == card::Suit::Heart;
            case card::JudgeTrigger::NotHeart:
                return c.suit != card::Suit::Heart;
            case card::JudgeTrigger::Spade2to9:
                return c.suit == card::Suit::Spade && c.number >= 2 && c.number <= 9;
            }
            return false;
        }

        /** @brief 判定结果动作：触发取 success，否则取 failure。 */
        inline card::JudgeAction judge_result(
            const card::JudgeEffect &j, const card::Card &c)
        {
            return judge_triggered(j.trigger, c) ? j.success : j.failure;
        }

        /**
         * @brief 目录查找：命中返回定义指针，未命中或目录未绑定时返回 nullptr。
         */
        inline const card::CardDef *def_of(
            const ReadOnlyContext &ctx, const std::string &def_id)
        {
            if (!ctx.catalog)
                return nullptr;
            const auto def = ctx.catalog->find(def_id);
            return def.is_some() ? def.unwrap() : nullptr;
        }

        /**
         * @brief 单张牌是否满足谓词（目录缺失或未命中即不匹配）。
         * @tparam Accept 谓词类型：接受 `const card::CardDef &`、返回 bool。
         */
        template <class Accept>
        inline bool hand_card_matching(
            const ReadOnlyContext &ctx, const card::Card &c, Accept accept)
        {
            const card::CardDef *def = def_of(ctx, c.def_id);
            return def != nullptr && accept(*def);
        }

        /**
         * @brief 手牌中是否存在满足谓词者（只读扫描）。
         * @tparam Accept 谓词类型：接受 `const card::CardDef &`、返回 bool。
         */
        template <class Accept>
        inline bool any_hand_card_matching(
            const ReadOnlyContext &ctx, const std::string &owner, Accept accept)
        {
            for (const auto &c : ctx.cards->hand(owner))
                if (hand_card_matching(ctx, c, accept))
                    return true;
            return false;
        }

        /**
         * @brief 手牌中首张满足谓词者（只读副本）。
         * @tparam Accept 谓词类型：接受 `const card::CardDef &`、返回 bool。
         * @return Some(首张匹配的手牌)；None = 无匹配（含目录缺失）。
         */
        template <class Accept>
        inline Option<card::Card> find_hand_card_matching(
            const ReadOnlyContext &ctx, const std::string &owner, Accept accept)
        {
            for (const auto &c : ctx.cards->hand(owner))
                if (hand_card_matching(ctx, c, accept))
                    return Option<card::Card>::Some(c);
            return Option<card::Card>::None();
        }

        /** @brief 回血（按上限钳制）。 */
        inline void apply_heal(GameContext &ctx, const std::string &target, int amount)
        {
            const auto e = ctx.entities->find(target);
            if (e.is_some())
                e.unwrap()->heal(amount);
        }

        /**
         * @brief 从摸牌堆取一张；牌堆空且弃牌堆非空时经 rng 洗回后重试。
         * @return None 表示摸牌堆与弃牌堆皆空（或无 rng 且摸牌堆空）。
         * @note 依赖 ctx.rng 洗回；随机源为 null 时牌堆空则直接 None。
         */
        inline Option<card::Card> draw_with_refill(GameContext &ctx)
        {
            if (ctx.cards->draw_size() == 0)
            {
                if (ctx.cards->discard_size() == 0)
                    return Option<card::Card>::None();
                if (ctx.rng)
                    ctx.cards->refill_draw(*ctx.rng);
            }
            return ctx.cards->draw();
        }

        /** @brief 摸 count 张进手牌；牌堆与弃牌堆皆空即停，返回实际摸到的张数。 */
        inline int apply_draw(GameContext &ctx, const std::string &player, int count)
        {
            int drew = 0;
            for (int i = 0; i < count; ++i)
            {
                auto c = draw_with_refill(ctx);
                if (c.is_none())
                    break;
                card::Card card = std::move(c).unwrap();
                ctx.cards->add_to_hand(player, card);
                emit_card_drawn(ctx, player, card);
                ++drew;
            }
            return drew;
        }

        /** @brief 从某实体的任一区域移除指定牌（填 out 返回被移除的牌与来源区域）。 */
        inline bool remove_card_from_zones(
            GameContext &ctx, const std::string &entity_id,
            const std::string &instance_id, card::Card &out,
            Zone *from_zone = nullptr)
        {
            auto c = ctx.cards->remove_from_any(entity_id, instance_id, from_zone);
            if (c.is_none())
                return false;
            out = std::move(c).unwrap();
            return true;
        }

        /** @brief 弃置一张牌并发布弃置事件。按值拷贝入弃牌堆，事件读原牌。 */
        inline void discard_and_emit(
            GameContext &ctx, const std::string &owner, const card::Card &c)
        {
            ctx.cards->discard(c);
            emit_card_discarded(ctx, owner, c);
        }

        /**
         * @brief 从手牌移除指定牌并无条件弃置，发布弃置事件。
         * @param instance_id 待移除的手牌实例；不在手牌时直接失败。
         * @return Some(被弃置的牌)；None = 该牌不在 owner 手牌（无副作用）。
         * @note 弃置按值拷贝、事件读原牌后再整体移动返回，返回值保持完整。
         */
        inline Option<card::Card> remove_and_discard(
            GameContext &ctx, const std::string &owner, const std::string &instance_id)
        {
            auto removed = ctx.cards->remove_from_hand(owner, instance_id);
            if (removed.is_none())
                return Option<card::Card>::None();
            card::Card card = std::move(removed).unwrap();
            discard_and_emit(ctx, owner, card);
            return Option<card::Card>::Some(std::move(card));
        }

        /**
         * @brief 从任一区域移除指定牌并无条件弃置，发布弃置事件。
         * @param instance_id 待移除的牌实例（按手牌 → 装备 → 判定顺序查找）。
         * @return Some(被弃置的牌)；None = 该牌不在 owner 任一区域（无副作用）。
         * @note 弃置按值拷贝、事件读原牌后再整体移动返回，返回值保持完整。
         */
        inline Option<card::Card> remove_any_and_discard(
            GameContext &ctx, const std::string &owner, const std::string &instance_id)
        {
            card::Card card;
            if (!remove_card_from_zones(ctx, owner, instance_id, card))
                return Option<card::Card>::None();
            discard_and_emit(ctx, owner, card);
            return Option<card::Card>::Some(std::move(card));
        }

        /**
         * @brief 从手牌移除指定牌并按谓词校验；合法则弃置并发布弃置事件。
         * @tparam Accept 谓词类型：接受 `const card::CardDef &`、返回 bool。
         * @param instance_id 待消费的手牌实例；不在手牌时直接失败。
         * @return Some(消费的牌) 成功；None = 牌不在手牌，或目录缺失/谓词不匹配
         *         （非法选择已退回手牌）。
         * @note 弃置按值拷贝、事件读原牌后再整体移动返回，返回值保持完整。
         */
        template <class Accept>
        inline Option<card::Card> consume_hand_card_matching(
            GameContext &ctx, const std::string &owner, const std::string &instance_id,
            Accept accept)
        {
            auto removed = ctx.cards->remove_from_hand(owner, instance_id);
            if (removed.is_none())
                return Option<card::Card>::None();
            card::Card card = std::move(removed).unwrap();

            const auto def = ctx.catalog->find(card.def_id);
            if (def.is_none() || !accept(*def.unwrap()))
            {
                ctx.cards->add_to_hand(owner, std::move(card));  // 非法选择退回
                return Option<card::Card>::None();
            }

            discard_and_emit(ctx, owner, card);
            return Option<card::Card>::Some(std::move(card));
        }

        /**
         * @brief 判定：从摸牌堆顶揭示一张（牌堆空则弃牌堆洗回）。
         * @return None 表示摸牌堆与弃牌堆皆空（无法判定）。
         * @note 洗回口径与摸牌一致：同走 draw_with_refill。
         */
        inline Option<card::Card> perform_judgement(GameContext &ctx)
        {
            return draw_with_refill(ctx);
        }
    }
}

#endif  // INCLUDE_TKW_GAME_STATE_HPP