/**
 * @file turn.hpp
 * @brief 回合流程：判定 → 摸牌 → 出牌（含杀次数限制/装备）→ 弃牌。
 * @note 规则约定：
 *       - 判定阶段按判定区顺序结算延时锦囊；乐不思蜀判定非红桃跳过出牌，
 *         闪电判定黑桃2~9 则造成雷伤、否则移入下家判定区；
 *       - 杀每回合限一次，装备诸葛连弩后不限制；
 *       - 弃牌阶段手牌上限 = 体力上限。
 * @note 死亡/濒死救场不在本模块（hp 可被扣到非正，死亡声明归后续流程）。
 */

#ifndef INCLUDE_TKW_GAME_TURN_HPP
#define INCLUDE_TKW_GAME_TURN_HPP

#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "card/card.hpp"
#include "card/def.hpp"
#include "card/manager.hpp"
#include "game/resolve/combat.hpp"
#include "game/core/context.hpp"
#include "game/resolve/counter.hpp"
#include "game/core/decision.hpp"
#include "game/core/effect.hpp"
#include "game/query/distance.hpp"
#include "game/query/equip.hpp"
#include "game/query/judge.hpp"
#include "game/resolve/resolver.hpp"
#include "game/core/state.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 回合流程错误。 */
        enum class TurnError : std::uint8_t
        {
            UnknownPlayer,       /**< 实体不存在 */
            UnknownCard,         /**< 目录中找不到该卡定义 */
            CardNotInHand,       /**< 要打出的牌不在手牌中 */
            InvalidTarget,       /**< 目标不在合法目标集合内 */
            ShaLimitExceeded,    /**< 本回合杀次数已达上限 */
            NotEquipment,        /**< 装备动作目标不是装备牌 */
            DelayedDuplicate,    /**< 判定区已有同名的延时锦囊 */
            PlayRejected,        /**< 结算器拒绝该效果 */
            DiscardInsufficient, /**< 弃牌数量不足/引用了不存在的牌 */
            JudgeEmptyDeck,      /**< 判定时摸牌堆与弃牌堆皆空 */
        };

        template <typename T>
        using TurnResult = Result<T, TurnError>;

        /** @brief 延时锦囊判定结果。 */
        enum class DelayedOutcome : std::uint8_t
        {
            Normal,          /**< 判定后无特殊效果（乐不思蜀为红桃） */
            SkipPlay,        /**< 跳过出牌阶段（乐不思蜀非红桃） */
            LightningStruck, /**< 闪电劈中 */
            PassedToNext,    /**< 闪电未劈中，移至下家判定区 */
        };

        // ── 判定 ────────────────────────────────────────────────────────

        /** @brief 下家（按座位序环绕；死亡者已被移除，天然跳过）。 */
        inline std::string next_player(const GameContext &ctx, const std::string &player)
        {
            return ctx.entities->next(player);
        }

        /**
         * @brief 结算玩家判定区的一张延时锦囊（判定牌进弃牌堆；延时牌按结果
         *        弃置或移入下家判定区，从玩家判定区移除）。
         */
        inline TurnResult<DelayedOutcome> resolve_delayed(
            GameContext &ctx, DecisionSource &ai,
            const std::string &player,
            const card::Card &delayed)
        {
            const auto def_opt = ctx.catalog->find(delayed.def_id);
            if (def_opt.is_none())
                return TurnResult<DelayedOutcome>::Err(TurnError::UnknownCard);
            const card::CardDef &def = *def_opt.unwrap();

            // 先从判定区移除延时牌（各分支决定弃置或移送下家）
            auto removed = ctx.cards->remove_from_judge(player, delayed.instance_id);
            const card::Card delayed_card =
                removed.is_some() ? std::move(removed).unwrap() : delayed;

            // 无懈窗口：判定结算前可被抵消，抵消则直接弃置
            if (resolve_nullification(ctx, ai, def, player))
            {
                ctx.cards->discard(delayed_card);
                emit_card_discarded(ctx, player, delayed_card);
                return TurnResult<DelayedOutcome>::Ok(DelayedOutcome::Normal);
            }

            auto judge = perform_judgement(ctx);
            if (judge.is_none())
                return TurnResult<DelayedOutcome>::Err(TurnError::JudgeEmptyDeck);
            const card::Card judge_card = std::move(judge).unwrap();
            ctx.cards->discard(judge_card);  // 判定牌进弃牌堆
            emit_card_discarded(ctx, player, judge_card);

            if (def.judge.is_none())
            {
                ctx.cards->discard(delayed_card);
                emit_card_discarded(ctx, player, delayed_card);
                return TurnResult<DelayedOutcome>::Ok(DelayedOutcome::Normal);
            }

            switch (judge_result(def.judge.unwrap(), judge_card))
            {
            case card::JudgeAction::SkipPlay:
                ctx.cards->discard(delayed_card);
                emit_card_discarded(ctx, player, delayed_card);
                return TurnResult<DelayedOutcome>::Ok(DelayedOutcome::SkipPlay);

            case card::JudgeAction::Damage:
                ctx.cards->discard(delayed_card);
                emit_card_discarded(ctx, player, delayed_card);
                deal_damage(ctx, ai, "", player, def.judge.unwrap().amount);
                return TurnResult<DelayedOutcome>::Ok(
                    DelayedOutcome::LightningStruck);

            case card::JudgeAction::PassToNext:
            {
                const std::string next = next_player(ctx, player);
                ctx.cards->add_to_judge(next, delayed_card);
                emit_card_moved(
                    ctx, player, next, delayed_card, Zone::Judge, Zone::Judge);
                return TurnResult<DelayedOutcome>::Ok(DelayedOutcome::PassedToNext);
            }

            case card::JudgeAction::Nothing:
            case card::JudgeAction::Jink:
            default:
                ctx.cards->discard(delayed_card);
                emit_card_discarded(ctx, player, delayed_card);
                return TurnResult<DelayedOutcome>::Ok(DelayedOutcome::Normal);
            }
        }

        // ── 出牌阶段辅助 ────────────────────────────────────────────────

        /** @brief 该定义是否为「杀」（效果类别 = Damage，单一事实源）。 */
        inline bool is_sha(const card::CardDef &def)
        {
            return def.effect.is_some() && is_sha_kind(def.effect.unwrap().kind);
        }

        /** @brief 从手牌找一张牌（返回副本，便于随后按 instance_id 消费）。 */
        inline Option<card::Card> find_in_hand(
            const GameContext &ctx,
            const std::string &player,
            const std::string &instance_id)
        {
            for (const auto &c : ctx.cards->hand(player))
                if (c.instance_id == instance_id)
                    return Option<card::Card>::Some(c);
            return Option<card::Card>::None();
        }

        /**
         * @brief 装备动作：手牌装备到装备区；同槽位已有装备则先弃置旧装备。
         */
        inline TurnResult<void> equip_card(
            GameContext &ctx, const std::string &player, const card::Card &card)
        {
            const auto def = ctx.catalog->find(card.def_id);
            if (def.is_none() || def.unwrap()->equip.is_none())
                return TurnResult<void>::Err(TurnError::NotEquipment);
            const auto slot = def.unwrap()->equip.unwrap().slot;

            for (const auto &c : ctx.cards->equip(player))
            {
                const auto d = ctx.catalog->find(c.def_id);
                if (d.is_some() && d.unwrap()->equip.is_some() &&
                    d.unwrap()->equip.unwrap().slot == slot)
                {
                    auto old = ctx.cards->remove_from_equip(player, c.instance_id);
                    if (old.is_some())
                    {
                        card::Card old_card = std::move(old).unwrap();
                        ctx.cards->discard(old_card);
                        emit_card_discarded(ctx, player, old_card);
                    }
                }
            }

            auto removed = ctx.cards->remove_from_hand(player, card.instance_id);
            if (removed.is_none())
                return TurnResult<void>::Err(TurnError::CardNotInHand);
            ctx.cards->add_to_equip(player, card);
            emit_card_played(ctx, player, card);
            emit_card_moved(ctx, player, player, card, Zone::Hand, Zone::Equip);
            return TurnResult<void>::Ok();
        }

        /**
         * @brief 打出延时锦囊：按 judge.scope 校验目标，置入其判定区。
         * @note 同名延时锦囊不可叠加；打出时开无懈窗口，被抵消则直接弃置。
         */
        inline TurnResult<void> place_delayed(
            GameContext &ctx, DecisionSource &ai, const std::string &player,
            const card::Card &card, const std::vector<std::string> &targets)
        {
            const auto def_opt = ctx.catalog->find(card.def_id);
            if (def_opt.is_none() || !is_delayed_trick(*def_opt.unwrap()))
                return TurnResult<void>::Err(TurnError::PlayRejected);
            const card::CardDef &def = *def_opt.unwrap();

            // 目标数量与 scope 先于同名去重：出 scope 且重名的目标先报 InvalidTarget
            if (targets.size() != 1)
                return TurnResult<void>::Err(TurnError::InvalidTarget);
            const std::string &target = targets.front();
            if (!is_delayed_scope_target(player, def, target))
                return TurnResult<void>::Err(TurnError::InvalidTarget);
            if (has_same_delayed(ctx, target, def.id))
                return TurnResult<void>::Err(TurnError::DelayedDuplicate);

            auto removed = ctx.cards->remove_from_hand(player, card.instance_id);
            if (removed.is_none())
                return TurnResult<void>::Err(TurnError::CardNotInHand);
            emit_card_played(ctx, player, card);

            if (resolve_nullification(ctx, ai, def, player))
            {
                ctx.cards->discard(std::move(removed).unwrap());
                emit_card_discarded(ctx, player, card);
                return TurnResult<void>::Ok();
            }

            ctx.cards->add_to_judge(target, std::move(removed).unwrap());
            emit_card_moved(ctx, player, target, card, Zone::Hand, Zone::Judge);
            return TurnResult<void>::Ok();
        }

        // ── 回合入口 ────────────────────────────────────────────────────

        /** @brief 结算错误 → 回合错误（单一映射点；未列明的值落 PlayRejected）。 */
        inline TurnError to_turn_error(EffectError e)
        {
            switch (e)
            {
            case EffectError::OutOfRange:
            case EffectError::InvalidTarget:
                return TurnError::InvalidTarget;
            case EffectError::CardNotOwned:
                return TurnError::CardNotInHand;
            case EffectError::ShaLimitExceeded:
                return TurnError::ShaLimitExceeded;
            case EffectError::DelayedDuplicate:
                return TurnError::DelayedDuplicate;
            default:
                return TurnError::PlayRejected;
            }
        }

        /** @brief 角色是否仍在场（回合中可能因闪电/决斗等死亡被移除）。 */
        inline bool is_alive(const GameContext &ctx, const std::string &player)
        {
            return ctx.entities->find(player).is_some();
        }

        /**
         * @brief 判定阶段：按判定区顺序结算延时锦囊。
         * @return 是否跳过出牌阶段（乐不思蜀判定非红桃）；角色中途死亡时调用方
         *         经 is_alive 判断，本函数不再继续结算。
         */
        inline TurnResult<bool> run_judgement_phase(
            GameContext &ctx, DecisionSource &ai, const std::string &player)
        {
            bool skip_play = false;
            const auto judge_zone = ctx.cards->judge(player);  // 拷贝
            for (const auto &delayed : judge_zone)
            {
                auto r = resolve_delayed(ctx, ai, player, delayed);
                if (r.is_err())
                    return TurnResult<bool>::Err(r.unwrap_err());
                if (r.unwrap() == DelayedOutcome::SkipPlay)
                    skip_play = true;
                if (!is_alive(ctx, player))
                    break;  // 闪电劈死 → 回合终止
            }
            return TurnResult<bool>::Ok(skip_play);
        }

        /** @brief 摸牌阶段：摸 rules.draw_per_turn 张。 */
        inline void run_draw_phase(GameContext &ctx, const std::string &player)
        {
            apply_draw(ctx, player, rules_of(ctx).draw_per_turn);
        }

        /**
         * @brief 出牌阶段：循环向 DecisionSource 要动作直到结束。
         * @note 非法动作（手牌不存在/目标非法/超杀次数）立即报错并中止本回合；
         *       角色中途死亡（决斗自伤等）即返回 Ok，由调用方判断。
         */
        inline TurnResult<void> run_play_phase(
            GameContext &ctx, DecisionSource &ai, const std::string &player)
        {
            int sha_played = 0;
            while (true)
            {
                if (!is_alive(ctx, player))
                    return TurnResult<void>::Ok();
                // 每轮重采样杀上限：回合中途装连弩要当轮生效，与引擎侧强制检查一致
                const TurnContext turn{player, sha_played, sha_limit(ctx, player)};
                auto action = ai.choose_play(ctx, turn);
                if (action.is_none())
                    break;

                const auto card =
                    find_in_hand(ctx, player, action.unwrap().instance_id);
                if (card.is_none())
                    return TurnResult<void>::Err(TurnError::CardNotInHand);

                const auto def_opt = ctx.catalog->find(card.unwrap().def_id);
                if (def_opt.is_none())
                    return TurnResult<void>::Err(TurnError::UnknownCard);
                const card::CardDef &def = *def_opt.unwrap();

                // 统一预检（手牌/分派/杀次数/目标/可实现性），失败即回合错误
                auto vr = validate_play_action(
                    ctx, player, def, card.unwrap(), action.unwrap().targets, turn);
                if (vr.is_err())
                    return TurnResult<void>::Err(to_turn_error(vr.unwrap_err()));

                switch (classify_action(def))
                {
                case PlayClass::Equipment:
                {
                    auto er = equip_card(ctx, player, card.unwrap());
                    if (er.is_err())
                        return TurnResult<void>::Err(er.unwrap_err());
                    continue;
                }

                case PlayClass::DelayedTrick:
                {
                    auto dr = place_delayed(
                        ctx, ai, player, card.unwrap(), action.unwrap().targets);
                    if (dr.is_err())
                        return TurnResult<void>::Err(dr.unwrap_err());
                    continue;
                }

                case PlayClass::Active:
                case PlayClass::None:
                    break;  // 主动效果与无效果牌都经 resolve_play 最终闸门
                }

                auto rr = resolve_play(
                    ctx, ai, player, card.unwrap(), action.unwrap().targets);
                if (rr.is_err())
                    return TurnResult<void>::Err(to_turn_error(rr.unwrap_err()));
                if (is_sha(def))
                    ++sha_played;
                if (!is_alive(ctx, player))
                    return TurnResult<void>::Ok();  // 决斗等自伤致死
            }
            return TurnResult<void>::Ok();
        }

        /** @brief 弃牌阶段：手牌上限 = 体力上限。 */
        inline TurnResult<void> run_discard_phase(
            GameContext &ctx, DecisionSource &ai, const std::string &player)
        {
            const int hand_limit =
                ctx.entities->find(player).unwrap()->get_hp_bar().get_max();
            const int over =
                static_cast<int>(ctx.cards->hand_size(player)) - hand_limit;
            if (over > 0)
            {
                const auto discards =
                    ai.choose_discards(ctx, player, over, DiscardReason::TurnLimit);
                if (static_cast<int>(discards.size()) != over)
                    return TurnResult<void>::Err(TurnError::DiscardInsufficient);
                for (const auto &id : discards)
                {
                    auto removed = ctx.cards->remove_from_hand(player, id);
                    if (removed.is_none())
                        return TurnResult<void>::Err(TurnError::DiscardInsufficient);
                    card::Card card = std::move(removed).unwrap();
                    ctx.cards->discard(card);
                    emit_card_discarded(ctx, player, card);
                }
            }
            return TurnResult<void>::Ok();
        }

        /**
         * @brief 执行 player 的一个完整回合：判定 → 摸2 → 出牌 → 弃牌。
         * @note 各阶段见 run_*_phase；角色在任意阶段死亡即终止本回合（死亡实体
         *       已被移除，阶段函数内均重新 find 以免悬垂指针）。
         */
        inline TurnResult<void> execute_turn(
            GameContext &ctx,
            DecisionSource &ai,
            const std::string &player)
        {
            if (ctx.entities->find(player).is_none())
                return TurnResult<void>::Err(TurnError::UnknownPlayer);

            // 1. 判定阶段
            auto jr = run_judgement_phase(ctx, ai, player);
            if (jr.is_err())
                return TurnResult<void>::Err(jr.unwrap_err());
            const bool skip_play = jr.unwrap();
            if (!is_alive(ctx, player))
                return TurnResult<void>::Ok();

            // 2. 摸牌阶段
            run_draw_phase(ctx, player);

            // 3. 出牌阶段
            if (!skip_play)
            {
                auto pr = run_play_phase(ctx, ai, player);
                if (pr.is_err())
                    return TurnResult<void>::Err(pr.unwrap_err());
            }

            // 4. 弃牌阶段
            if (!is_alive(ctx, player))
                return TurnResult<void>::Ok();
            return run_discard_phase(ctx, ai, player);
        }
    }
}

#endif  // INCLUDE_TKW_GAME_TURN_HPP