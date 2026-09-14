/**
 * @file turn.hpp
 * @brief 回合流程：判定 → 摸牌 → 出牌（含杀次数限制/装备）→ 弃牌。
 * @note 规则约定：
 *       - 判定阶段按判定区顺序结算延时锦囊；乐不思蜀判定非红桃跳过出牌，
 *         兵粮寸断判定非梅花跳过摸牌，
 *         闪电判定黑桃2~9 则造成雷伤、否则移入判定区无同名闪电的下家；
 *       - 杀每回合限一次，装备诸葛连弩后不限制；
 *       - 弃牌阶段手牌上限 = 当前体力值。
 * @note 死亡/濒死救场不在本模块（hp 可被扣到非正，死亡声明归后续流程）。
 * @ingroup tkw_game_flow
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
#include "game/core/context.hpp"
#include "game/core/decision.hpp"
#include "game/core/effect.hpp"
#include "game/core/state.hpp"
#include "game/query/distance.hpp"
#include "game/query/equip.hpp"
#include "game/query/judge.hpp"
#include "game/resolve/combat.hpp"
#include "game/resolve/counter.hpp"
#include "game/resolve/resolver.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 回合流程错误。 */
        enum class TurnError : std::uint8_t
        {
            UnknownPlayer,       /**< 实体不存在。 */
            UnknownCard,         /**< 目录中找不到该卡定义。 */
            CardNotInHand,       /**< 要打出的牌不在手牌中。 */
            InvalidTarget,       /**< 目标不在合法目标集合内。 */
            ShaLimitExceeded,    /**< 本回合杀次数已达上限。 */
            AnalepticLimitExceeded, /**< 本回合已使用过酒（出牌阶段限一次）。 */
            NotEquipment,        /**< 装备动作目标不是装备牌。 */
            DelayedDuplicate,    /**< 判定区已有同名的延时锦囊。 */
            PlayRejected,        /**< 结算器拒绝该效果。 */
            DiscardInsufficient, /**< 弃牌数量不足或引用了不存在的牌。 */
            JudgeEmptyDeck,      /**< 判定时摸牌堆与弃牌堆皆空。 */
        };

        /**
         * @brief 回合流程结果别名。
         * @tparam T 成功时承载的值类型。
         */
        template <typename T>
        using TurnResult = Result<T, TurnError>;

        /** @brief 延时锦囊判定结果。 */
        enum class DelayedOutcome : std::uint8_t
        {
            Normal,          /**< 判定后无特殊效果（乐不思蜀为红桃）。 */
            SkipPlay,        /**< 跳过出牌阶段（乐不思蜀非红桃）。 */
            SkipDraw,        /**< 跳过摸牌阶段（兵粮寸断非梅花）。 */
            LightningStruck, /**< 闪电劈中。 */
            PassedToNext,    /**< 闪电未劈中，移至下家判定区。 */
        };

        // ── 判定 ────────────────────────────────────────────────────────

        /**
         * @brief  下家：按座位序环绕的存活玩家。
         * @param[in] ctx    只读上下文。
         * @param[in] player 参照玩家 id。
         * @return `player` 之后的第一个存活玩家 id；死亡者已被移除，天然跳过。
         */
        inline std::string next_player(const GameContext &ctx, const std::string &player)
        {
            return ctx.entities->next(player);
        }

        /**
         * @brief  结算玩家判定区的一张延时锦囊。
         * @details 判定牌进弃牌堆；延时牌按结果弃置或移入下家判定区，并从原判定
         *          区移除。判定结算前先开无懈窗口，被抵消则直接弃置。
         * @param[in,out] ctx     对局上下文。
         * @param[in,out] ai      决策源（无懈窗口询问）。
         * @param[in]     player  被判定玩家 id。
         * @param[in]     delayed 待结算的延时锦囊牌。
         * @return 结算结果。
         * @retval Ok(DelayedOutcome) 已按判定行动结算。
         * @retval Err(TurnError::UnknownCard)    目录中找不到该卡定义。
         * @retval Err(TurnError::JudgeEmptyDeck) 判定时摸牌堆与弃牌堆皆空。
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
            // （判定窗口使用者不可考 → 空串哨兵，目标 = 被判定玩家）
            if (CounterResolver(ctx, ai).resolve_nullification(
                    CounterWindow::Builder{}.trick(&def).targets({player}).build()))
            {
                discard_and_emit(ctx, player, delayed_card);
                return TurnResult<DelayedOutcome>::Ok(DelayedOutcome::Normal);
            }

            auto judge = perform_judgement(ctx);
            if (judge.is_none())
            {
                // 判定牌不可得：延时牌已移出判定区，弃置以免凭空消失
                discard_and_emit(ctx, player, delayed_card);
                return TurnResult<DelayedOutcome>::Err(TurnError::JudgeEmptyDeck);
            }
            const card::Card judge_card = std::move(judge).unwrap();
            discard_and_emit(ctx, player, judge_card, DiscardKind::Judgement);  // 判定牌进弃牌堆

            if (def.judge.is_none())
            {
                discard_and_emit(ctx, player, delayed_card);
                return TurnResult<DelayedOutcome>::Ok(DelayedOutcome::Normal);
            }

            switch (judge_result(def.judge.unwrap(), judge_card))
            {
            case card::JudgeAction::SkipPlay:
                discard_and_emit(ctx, player, delayed_card);
                return TurnResult<DelayedOutcome>::Ok(DelayedOutcome::SkipPlay);

            case card::JudgeAction::SkipDraw:
                discard_and_emit(ctx, player, delayed_card);
                return TurnResult<DelayedOutcome>::Ok(DelayedOutcome::SkipDraw);

            case card::JudgeAction::Damage:
                discard_and_emit(ctx, player, delayed_card);
                CombatResolver(ctx, ai)
                    .deal_damage(
                        player, DamageSpec::Builder{}
                                    .damage_val(def.judge.unwrap().amount)
                                    .damage_type(def.judge.unwrap().damage_type)
                                    .build());
                return TurnResult<DelayedOutcome>::Ok(
                    DelayedOutcome::LightningStruck);

            case card::JudgeAction::PassToNext:
            {
                const std::string next =
                    next_delayed_target(ctx, player, delayed_card.def_id);
                if (next.empty())
                {
                    // 异常残留态下无空位：弃置而非丢牌
                    discard_and_emit(ctx, player, delayed_card);
                    return TurnResult<DelayedOutcome>::Ok(DelayedOutcome::Normal);
                }
                ctx.cards->add_to_judge(next, delayed_card);
                emit_card_moved(
                    ctx, player, next, delayed_card, Zone::Judge, Zone::Judge);
                return TurnResult<DelayedOutcome>::Ok(DelayedOutcome::PassedToNext);
            }

            case card::JudgeAction::Nothing:
            case card::JudgeAction::Jink:
            default:
                discard_and_emit(ctx, player, delayed_card);
                return TurnResult<DelayedOutcome>::Ok(DelayedOutcome::Normal);
            }
        }

        // ── 出牌阶段辅助 ────────────────────────────────────────────────

        /**
         * @brief  该定义是否为「杀」（效果类别 = `Damage`，单一事实源）。
         * @param[in] def 卡定义。
         * @return 效果类别为 `Damage` 时为 true。
         */
        inline bool is_sha(const card::CardDef &def)
        {
            return def.effect.is_some() && is_sha_kind(def.effect.unwrap().kind);
        }

        /**
         * @brief  从手牌找一张牌。
         * @param[in] ctx         只读上下文。
         * @param[in] player      手牌所有者 id。
         * @param[in] instance_id 目标牌实例 id。
         * @return 查询结果。
         * @retval Some 命中牌副本，便于随后按 `instance_id` 消费。
         * @retval None 手牌中不存在该实例。
         */
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
         * @brief  装备动作：手牌装备到装备区；同槽位已有装备则先弃置旧装备。
         * @param[in,out] ctx    对局上下文。
         * @param[in]     player 装备者 id。
         * @param[in]     card   要装备的手牌。
         * @return 结算结果。
         * @retval Ok  装备成功，旧同槽位装备已弃置。
         * @retval Err(TurnError::NotEquipment)  该卡不是装备牌。
         * @retval Err(TurnError::CardNotInHand) 该牌不在手牌中。
         * @note  同槽位旧装备在取牌前先弃置；`CardNotInHand` 早退发生在旧装备
         *         已弃置之后。
         */
        inline TurnResult<void> equip_card(
            GameContext &ctx, const std::string &player, const card::Card &card)
        {
            const auto def = ctx.catalog->find(card.def_id);
            if (def.is_none() || def.unwrap()->equip.is_none())
                return TurnResult<void>::Err(TurnError::NotEquipment);
            const auto slot = def.unwrap()->equip.unwrap().slot;

            // 按值取装备区副本：remove_from_equip 会 erase 底层 vector，直接遍历原区间迭代器会失效
            const auto equipped = ctx.cards->equip(player);
            for (const auto &c : equipped)
            {
                const auto d = ctx.catalog->find(c.def_id);
                if (d.is_some() && d.unwrap()->equip.is_some() &&
                    d.unwrap()->equip.unwrap().slot == slot)
                {
                    auto old = ctx.cards->remove_from_equip(player, c.instance_id);
                    if (old.is_some())
                    {
                        card::Card old_card = std::move(old).unwrap();
                        discard_and_emit(ctx, player, old_card);
                        apply_equip_lost(ctx, player, old_card);
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
         * @brief  打出延时锦囊：按 `judge.scope` 校验目标，置入其判定区。
         * @param[in,out] ctx     对局上下文。
         * @param[in,out] ai      决策源（无懈窗口询问）。
         * @param[in]     player  使用者 id。
         * @param[in]     card    打出的延时锦囊手牌。
         * @param[in]     targets 目标列表；数量必须恰为 1。
         * @return 结算结果。
         * @retval Ok  已置入目标判定区；被无懈抵消时该牌直接弃置。
         * @retval Err(TurnError::PlayRejected)     该牌不是延时锦囊。
         * @retval Err(TurnError::InvalidTarget)    目标数量不为 1 或目标不在合法范围。
         * @retval Err(TurnError::DelayedDuplicate) 目标判定区已有同名延时锦囊。
         * @retval Err(TurnError::CardNotInHand)    该牌不在手牌中。
         * @note  同名延时锦囊不可叠加；打出时开无懈窗口，被抵消则直接弃置。
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
            if (!is_delayed_scope_target(ctx, player, def, target))
                return TurnResult<void>::Err(TurnError::InvalidTarget);
            if (has_same_delayed(ctx, target, def.id))
                return TurnResult<void>::Err(TurnError::DelayedDuplicate);

            auto removed = ctx.cards->remove_from_hand(player, card.instance_id);
            if (removed.is_none())
                return TurnResult<void>::Err(TurnError::CardNotInHand);
            emit_card_played(ctx, player, card);

            if (CounterResolver(ctx, ai).resolve_nullification(
                    CounterWindow::Builder{}
                        .trick(&def)
                        .trick_user(player)
                        .targets({target})
                        .build()))
            {
                discard_and_emit(ctx, player, card);
                return TurnResult<void>::Ok();
            }

            ctx.cards->add_to_judge(target, std::move(removed).unwrap());
            emit_card_moved(ctx, player, target, card, Zone::Hand, Zone::Judge);
            return TurnResult<void>::Ok();
        }

        // ── 回合入口 ────────────────────────────────────────────────────

        /**
         * @brief  结算错误 → 回合错误（单一映射点）。
         * @param[in] e 结算错误。
         * @return 对应回合错误；未列明的值一律落 `TurnError::PlayRejected`。
         */
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
            case EffectError::AnalepticLimitExceeded:
                return TurnError::AnalepticLimitExceeded;
            case EffectError::DelayedDuplicate:
                return TurnError::DelayedDuplicate;
            default:
                return TurnError::PlayRejected;
            }
        }

        /**
         * @brief  角色是否仍在场。
         * @param[in] ctx    只读上下文。
         * @param[in] player 角色 id。
         * @return 实体仍在容器中时为 true；回合中可能因闪电/决斗等死亡被移除。
         */
        inline bool is_alive(const GameContext &ctx, const std::string &player)
        {
            return ctx.entities->find(player).is_some();
        }

        /** @brief 判定阶段汇总的「跳过阶段」集合：乐不思蜀跳 play、兵粮寸断跳 draw。 */
        struct TurnSkips
        {
            bool skip_play = false; /**< 跳过出牌阶段（乐不思蜀非红桃）。 */
            bool skip_draw = false; /**< 跳过摸牌阶段（兵粮寸断非梅花）。 */
        };

        /**
         * @brief  判定阶段：按判定区顺序结算延时锦囊。
         * @param[in,out] ctx    对局上下文。
         * @param[in,out] ai     决策源。
         * @param[in]     player 当前回合角色 id。
         * @return 本回合需跳过的阶段集合（乐不思蜀非红桃 → `skip_play`，
         *         兵粮寸断非梅花 → `skip_draw`）。
         * @retval Ok(TurnSkips) 已完成判定区结算。
         * @retval Err(TurnError) 某张延时锦囊结算失败，原样上抛。
         * @note  角色中途死亡（如闪电劈死）时停止后续结算，调用方经 `is_alive`
         *         判断回合是否终止。
         */
        inline TurnResult<TurnSkips> run_judgement_phase(
            GameContext &ctx, DecisionSource &ai, const std::string &player)
        {
            TurnSkips skips;
            const auto judge_zone = ctx.cards->judge(player);  // 拷贝
            for (const auto &delayed : judge_zone)
            {
                auto r = resolve_delayed(ctx, ai, player, delayed);
                if (r.is_err())
                    return TurnResult<TurnSkips>::Err(r.unwrap_err());
                const DelayedOutcome outcome = r.unwrap();
                if (outcome == DelayedOutcome::SkipPlay)
                    skips.skip_play = true;
                else if (outcome == DelayedOutcome::SkipDraw)
                    skips.skip_draw = true;
                if (!is_alive(ctx, player))
                    break;  // 闪电劈死 → 回合终止
            }
            return TurnResult<TurnSkips>::Ok(skips);
        }

        /**
         * @brief  摸牌阶段：摸 `draw_phase_count` 张（基础 `rules.draw_per_turn`，
         *         英姿 +1）。
         * @param[in,out] ctx    对局上下文。
         * @param[in]     player 当前回合角色 id。
         * @post  该角色手牌增加相应张数，并发布摸牌事件。
         */
        inline void run_draw_phase(GameContext &ctx, const std::string &player)
        {
            apply_draw(ctx, player, draw_phase_count(ctx, player));
        }

        /**
         * @brief  出牌阶段：循环向 `DecisionSource` 要动作直到结束。
         * @param[in,out] ctx    对局上下文。
         * @param[in,out] ai     决策源。
         * @param[in]     player 当前回合角色 id。
         * @return 结算结果。
         * @retval Ok  决策源不再出牌，或角色中途死亡（决斗自伤等）。
         * @retval Err(TurnError) 非法动作（手牌不存在/目标非法/超杀次数等）立即
         *         中止本回合。
         * @note  每轮重采样杀上限，回合中途装连弩当轮生效；重铸不计杀次数。
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
                const TurnContext turn{
                    player, sha_played, sha_limit(ctx, player), ctx.jiu_used};
                auto action = ai.choose_play(ctx, turn);
                if (action.is_none())
                    break;

                if (!action.unwrap().second_instance_id.empty() ||
                    action.unwrap().converted_sha)
                {
                    // 虚拟杀（丈八蛇矛两张 / 武圣红牌单张）：按一张杀计次数
                    auto vr = validate_virtual_sha(
                        ctx, player, action.unwrap().instance_id,
                        action.unwrap().second_instance_id,
                        action.unwrap().targets, turn);
                    if (vr.is_err())
                        return TurnResult<void>::Err(to_turn_error(vr.unwrap_err()));
                    // 主动使用虚拟杀：消费本回合的酒加成（响应/打出路径不消费）
                    const int jiu = consume_jiu_sha_bonus(ctx, player);
                    auto rr = resolve_virtual_sha(
                        ctx, ai, player, action.unwrap().instance_id,
                        action.unwrap().second_instance_id, action.unwrap().targets,
                        true, jiu);
                    if (rr.is_err())
                        return TurnResult<void>::Err(to_turn_error(rr.unwrap_err()));
                    ++sha_played;
                    continue;
                }

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

                // 重铸：弃置此牌并摸一张，不使用牌面效果、不发打出事件、不计杀次数；
                // 空目标动作也可能是决策侧未透传标记的重铸选择，故按定义补认
                if (action.unwrap().recast ||
                    (def.recast && action.unwrap().targets.empty()))
                {
                    if (!def.recast || !action.unwrap().targets.empty())
                        return TurnResult<void>::Err(TurnError::PlayRejected);
                    if (remove_and_discard(ctx, player, card.unwrap().instance_id)
                            .is_none())
                        return TurnResult<void>::Err(TurnError::CardNotInHand);
                    apply_draw(ctx, player, 1);
                    continue;
                }

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

        /**
         * @brief  弃牌阶段：手牌上限 = 当前体力值。
         * @param[in,out] ctx    对局上下文。
         * @param[in,out] ai     决策源（询问弃牌）。
         * @param[in]     player 当前回合角色 id。
         * @return 结算结果。
         * @retval Ok  手牌未超上限或已弃足。
         * @retval Err(TurnError::DiscardInsufficient) 弃牌数量不足或引用了不存在的牌。
         */
        inline TurnResult<void> run_discard_phase(
            GameContext &ctx, DecisionSource &ai, const std::string &player)
        {
            const int hand_limit =
                ctx.entities->find(player).unwrap()->get_hp_bar().get_cur();
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
                    if (remove_and_discard(ctx, player, id).is_none())
                        return TurnResult<void>::Err(TurnError::DiscardInsufficient);
                }
            }
            return TurnResult<void>::Ok();
        }

        /**
         * @brief  执行 `player` 的一个完整回合：判定 → 摸牌 → 出牌 → 弃牌。
         * @details 四阶段依次为 `run_judgement_phase`、`run_draw_phase`（受
         *          `skip_draw` 抑制）、`run_play_phase`（受 `skip_play` 抑制）、
         *          `run_discard_phase`。
         * @param[in,out] ctx    对局上下文。
         * @param[in,out] ai     决策源。
         * @param[in]     player 当前回合角色 id。
         * @return 结算结果。
         * @retval Ok  回合正常结束，或角色在阶段中途死亡而终止。
         * @retval Err(TurnError) 某阶段失败，原样上抛。
         * @note  入口把 `player` 置入 `ctx.turn_player` 并在返回时还原（覆盖全部
         *         早退），供濒死询问等结算读取当前回合角色；离开本函数即回到
         *         「无回合上下文」。
         * @note  酒的伤害加成与「本回合已用酒」标记在入口清空、出口清空：二者是
         *         回合内运行时状态，不持久化，也不跨回合/跨玩家泄漏。
         * @note  角色在任意阶段死亡即终止本回合（死亡实体已被移除，阶段函数内均
         *         重新 `find` 以免悬垂指针）。
         * @warning 失败时不会回滚已落子的部分（判定/摸牌/出牌可能已结算），调用方
         *          须消费该回合（推进行程），不得以同一角色重入。
         * @see   run_judgement_phase, run_draw_phase, run_play_phase, run_discard_phase
         */
        inline TurnResult<void> execute_turn(
            GameContext &ctx,
            DecisionSource &ai,
            const std::string &player)
        {
            if (ctx.entities->find(player).is_none())
                return TurnResult<void>::Err(TurnError::UnknownPlayer);

            // 回合上下文：置位当前回合角色；酒加成与限一次标记随回合清空/清出，
            // 所有返回路径经守卫覆盖，避免同一 GameContext 跨回合残留
            struct TurnPlayerScope
            {
                GameContext &context;
                std::string prev;
                ~TurnPlayerScope()
                {
                    context.turn_player = std::move(prev);
                    // 酒状态是回合内运行时状态：出回合即失效，不跨回合/跨玩家泄漏
                    context.jiu_damage_owner.clear();
                    context.jiu_used = false;
                }
            } turn_scope{ctx, std::move(ctx.turn_player)};
            ctx.turn_player = player;
            ctx.jiu_damage_owner.clear();
            ctx.jiu_used = false;

            // 1. 判定阶段
            auto jr = run_judgement_phase(ctx, ai, player);
            if (jr.is_err())
                return TurnResult<void>::Err(jr.unwrap_err());
            const TurnSkips skips = jr.unwrap();
            if (!is_alive(ctx, player))
                return TurnResult<void>::Ok();

            // 2. 摸牌阶段
            if (!skips.skip_draw)
                run_draw_phase(ctx, player);

            // 3. 出牌阶段
            if (!skips.skip_play)
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