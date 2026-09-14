/**
 * @file   turn.cpp
 * @brief  回合四阶段与只读查询的定义。
 * @ingroup tkw_game_flow
 */

#include "game/flow/turn.hpp"

#include <string>
#include <utility>
#include <vector>

namespace tkw
{
    namespace game
    {
        Option<card::Card> TurnQuery::find_in_hand(
            const GameContext &ctx,
            const std::string &player,
            const std::string &instance_id)
        {
            for (const auto &c : ctx.cards->hand(player))
                if (c.instance_id == instance_id)
                    return Option<card::Card>::Some(c);
            return Option<card::Card>::None();
        }

        TurnError TurnQuery::to_turn_error(EffectError e)
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

        TurnResult<DelayedOutcome> TurnFlow::resolve_delayed(
            const std::string &player,
            const card::Card &delayed)
        {
            const auto def_opt = m_ctx.catalog->find(delayed.def_id);
            if (def_opt.is_none())
                return TurnResult<DelayedOutcome>::Err(TurnError::UnknownCard);
            const card::CardDef &def = *def_opt.unwrap();

            // 先从判定区移除延时牌（各分支决定弃置或移送下家）
            auto removed = m_ctx.cards->remove_from_judge(player, delayed.instance_id);
            const card::Card delayed_card =
                removed.is_some() ? std::move(removed).unwrap() : delayed;

            // 无懈窗口：判定结算前可被抵消，抵消则直接弃置
            // （判定窗口使用者不可考 → 空串哨兵，目标 = 被判定玩家）
            if (CounterResolver(m_ctx, m_ai).resolve_nullification(
                    CounterWindow::Builder{}.trick(&def).targets({player}).build()))
            {
                StateOps(m_ctx).discard_and_emit(player, delayed_card);
                return TurnResult<DelayedOutcome>::Ok(DelayedOutcome::Normal);
            }

            auto judge = StateOps(m_ctx).perform_judgement();
            if (judge.is_none())
            {
                // 判定牌不可得：延时牌已移出判定区，弃置以免凭空消失
                StateOps(m_ctx).discard_and_emit(player, delayed_card);
                return TurnResult<DelayedOutcome>::Err(TurnError::JudgeEmptyDeck);
            }
            const card::Card judge_card = std::move(judge).unwrap();
            StateOps(m_ctx).discard_and_emit(
                player, judge_card, DiscardKind::Judgement);  // 判定牌进弃牌堆

            if (def.judge.is_none())
            {
                StateOps(m_ctx).discard_and_emit(player, delayed_card);
                return TurnResult<DelayedOutcome>::Ok(DelayedOutcome::Normal);
            }

            switch (StateQuery::judge_result(def.judge.unwrap(), judge_card))
            {
            case card::JudgeAction::SkipPlay:
                StateOps(m_ctx).discard_and_emit(player, delayed_card);
                return TurnResult<DelayedOutcome>::Ok(DelayedOutcome::SkipPlay);

            case card::JudgeAction::SkipDraw:
                StateOps(m_ctx).discard_and_emit(player, delayed_card);
                return TurnResult<DelayedOutcome>::Ok(DelayedOutcome::SkipDraw);

            case card::JudgeAction::Damage:
                StateOps(m_ctx).discard_and_emit(player, delayed_card);
                CombatResolver(m_ctx, m_ai)
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
                    JudgeQuery::next_delayed_target(m_ctx, player, delayed_card.def_id);
                if (next.empty())
                {
                    // 异常残留态下无空位：弃置而非丢牌
                    StateOps(m_ctx).discard_and_emit(player, delayed_card);
                    return TurnResult<DelayedOutcome>::Ok(DelayedOutcome::Normal);
                }
                m_ctx.cards->add_to_judge(next, delayed_card);
                emit_card_moved(
                    m_ctx, player, next, delayed_card, Zone::Judge, Zone::Judge);
                return TurnResult<DelayedOutcome>::Ok(DelayedOutcome::PassedToNext);
            }

            case card::JudgeAction::Nothing:
            case card::JudgeAction::Jink:
            default:
                StateOps(m_ctx).discard_and_emit(player, delayed_card);
                return TurnResult<DelayedOutcome>::Ok(DelayedOutcome::Normal);
            }
        }

        TurnResult<void> TurnFlow::equip_card(
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
                        StateOps(ctx).discard_and_emit(player, old_card);
                        StateOps(ctx).apply_equip_lost(player, old_card);
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

        TurnResult<void> TurnFlow::place_delayed(
            const std::string &player,
            const card::Card &card, const std::vector<std::string> &targets)
        {
            const auto def_opt = m_ctx.catalog->find(card.def_id);
            if (def_opt.is_none() || !is_delayed_trick(*def_opt.unwrap()))
                return TurnResult<void>::Err(TurnError::PlayRejected);
            const card::CardDef &def = *def_opt.unwrap();

            // 目标数量与 scope 先于同名去重：出 scope 且重名的目标先报 InvalidTarget
            if (targets.size() != 1)
                return TurnResult<void>::Err(TurnError::InvalidTarget);
            const std::string &target = targets.front();
            if (!JudgeQuery::is_delayed_scope_target(m_ctx, player, def, target))
                return TurnResult<void>::Err(TurnError::InvalidTarget);
            if (JudgeQuery::has_same_delayed(m_ctx, target, def.id))
                return TurnResult<void>::Err(TurnError::DelayedDuplicate);

            auto removed = m_ctx.cards->remove_from_hand(player, card.instance_id);
            if (removed.is_none())
                return TurnResult<void>::Err(TurnError::CardNotInHand);
            emit_card_played(m_ctx, player, card);

            if (CounterResolver(m_ctx, m_ai).resolve_nullification(
                    CounterWindow::Builder{}
                        .trick(&def)
                        .trick_user(player)
                        .targets({target})
                        .build()))
            {
                StateOps(m_ctx).discard_and_emit(player, card);
                return TurnResult<void>::Ok();
            }

            m_ctx.cards->add_to_judge(target, std::move(removed).unwrap());
            emit_card_moved(m_ctx, player, target, card, Zone::Hand, Zone::Judge);
            return TurnResult<void>::Ok();
        }

        TurnResult<TurnSkips> TurnFlow::run_judgement_phase(
            const std::string &player)
        {
            TurnSkips skips;
            const auto judge_zone = m_ctx.cards->judge(player);  // 拷贝
            for (const auto &delayed : judge_zone)
            {
                auto r = resolve_delayed(player, delayed);
                if (r.is_err())
                    return TurnResult<TurnSkips>::Err(r.unwrap_err());
                const DelayedOutcome outcome = r.unwrap();
                if (outcome == DelayedOutcome::SkipPlay)
                    skips.skip_play = true;
                else if (outcome == DelayedOutcome::SkipDraw)
                    skips.skip_draw = true;
                if (!TurnQuery::is_alive(m_ctx, player))
                    break;  // 闪电劈死 → 回合终止
            }
            return TurnResult<TurnSkips>::Ok(skips);
        }

        void TurnFlow::run_draw_phase(const std::string &player)
        {
            StateOps(m_ctx).apply_draw(
                player, HeroQuery::draw_phase_count(m_ctx, player));
        }

        TurnResult<void> TurnFlow::run_play_phase(const std::string &player)
        {
            int sha_played = 0;
            while (true)
            {
                if (!TurnQuery::is_alive(m_ctx, player))
                    return TurnResult<void>::Ok();
                // 每轮重采样杀上限：回合中途装连弩要当轮生效，与引擎侧强制检查一致
                const TurnContext turn{
                    player, sha_played, EquipQuery::sha_limit(m_ctx, player),
                    m_ctx.jiu_used};
                auto action = m_ai.choose_play(m_ctx, turn);
                if (action.is_none())
                    break;

                if (!action.unwrap().second_instance_id.empty() ||
                    action.unwrap().converted_sha)
                {
                    // 虚拟杀（丈八蛇矛两张 / 武圣红牌单张）：按一张杀计次数
                    auto vr = validate_virtual_sha(
                        m_ctx, player, action.unwrap().instance_id,
                        action.unwrap().second_instance_id,
                        action.unwrap().targets, turn);
                    if (vr.is_err())
                        return TurnResult<void>::Err(
                            TurnQuery::to_turn_error(vr.unwrap_err()));
                    // 主动使用虚拟杀：消费本回合的酒加成（响应/打出路径不消费）
                    const int jiu = StateOps(m_ctx).consume_jiu_sha_bonus(player);
                    auto rr = resolve_virtual_sha(
                        m_ctx, m_ai, player, action.unwrap().instance_id,
                        action.unwrap().second_instance_id, action.unwrap().targets,
                        true, jiu);
                    if (rr.is_err())
                        return TurnResult<void>::Err(
                            TurnQuery::to_turn_error(rr.unwrap_err()));
                    ++sha_played;
                    continue;
                }

                const auto card =
                    TurnQuery::find_in_hand(m_ctx, player, action.unwrap().instance_id);
                if (card.is_none())
                    return TurnResult<void>::Err(TurnError::CardNotInHand);

                const auto def_opt = m_ctx.catalog->find(card.unwrap().def_id);
                if (def_opt.is_none())
                    return TurnResult<void>::Err(TurnError::UnknownCard);
                const card::CardDef &def = *def_opt.unwrap();

                // 统一预检（手牌/分派/杀次数/目标/可实现性），失败即回合错误
                auto vr = validate_play_action(
                    m_ctx, player, def, card.unwrap(), action.unwrap().targets, turn);
                if (vr.is_err())
                    return TurnResult<void>::Err(
                        TurnQuery::to_turn_error(vr.unwrap_err()));

                // 重铸：弃置此牌并摸一张，不使用牌面效果、不发打出事件、不计杀次数；
                // 空目标动作也可能是决策侧未透传标记的重铸选择，故按定义补认
                if (action.unwrap().recast ||
                    (def.recast && action.unwrap().targets.empty()))
                {
                    if (!def.recast || !action.unwrap().targets.empty())
                        return TurnResult<void>::Err(TurnError::PlayRejected);
                    if (StateOps(m_ctx)
                            .remove_and_discard(player, card.unwrap().instance_id)
                            .is_none())
                        return TurnResult<void>::Err(TurnError::CardNotInHand);
                    StateOps(m_ctx).apply_draw(player, 1);
                    continue;
                }

                switch (classify_action(def))
                {
                case PlayClass::Equipment:
                {
                    auto er = TurnFlow::equip_card(m_ctx, player, card.unwrap());
                    if (er.is_err())
                        return TurnResult<void>::Err(er.unwrap_err());
                    continue;
                }

                case PlayClass::DelayedTrick:
                {
                    auto dr = place_delayed(
                        player, card.unwrap(), action.unwrap().targets);
                    if (dr.is_err())
                        return TurnResult<void>::Err(dr.unwrap_err());
                    continue;
                }

                case PlayClass::Active:
                case PlayClass::None:
                    break;  // 主动效果与无效果牌都经 resolve_play 最终闸门
                }

                auto rr = resolve_play(
                    m_ctx, m_ai, player, card.unwrap(), action.unwrap().targets);
                if (rr.is_err())
                    return TurnResult<void>::Err(
                        TurnQuery::to_turn_error(rr.unwrap_err()));
                if (TurnQuery::is_sha(def))
                    ++sha_played;
                if (!TurnQuery::is_alive(m_ctx, player))
                    return TurnResult<void>::Ok();  // 决斗等自伤致死
            }
            return TurnResult<void>::Ok();
        }

        TurnResult<void> TurnFlow::run_discard_phase(
            const std::string &player)
        {
            const int hand_limit =
                m_ctx.entities->find(player).unwrap()->get_hp_bar().get_cur();
            const int over =
                static_cast<int>(m_ctx.cards->hand_size(player)) - hand_limit;
            if (over > 0)
            {
                const auto discards =
                    m_ai.choose_discards(m_ctx, player, over, DiscardReason::TurnLimit);
                if (static_cast<int>(discards.size()) != over)
                    return TurnResult<void>::Err(TurnError::DiscardInsufficient);
                for (const auto &id : discards)
                {
                    if (StateOps(m_ctx).remove_and_discard(player, id).is_none())
                        return TurnResult<void>::Err(TurnError::DiscardInsufficient);
                }
            }
            return TurnResult<void>::Ok();
        }

        TurnResult<void> TurnFlow::execute_turn(const std::string &player)
        {
            if (m_ctx.entities->find(player).is_none())
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
            } turn_scope{m_ctx, std::move(m_ctx.turn_player)};
            m_ctx.turn_player = player;
            m_ctx.jiu_damage_owner.clear();
            m_ctx.jiu_used = false;

            // 1. 判定阶段
            auto jr = run_judgement_phase(player);
            if (jr.is_err())
                return TurnResult<void>::Err(jr.unwrap_err());
            const TurnSkips skips = jr.unwrap();
            if (!TurnQuery::is_alive(m_ctx, player))
                return TurnResult<void>::Ok();

            // 2. 摸牌阶段
            if (!skips.skip_draw)
                run_draw_phase(player);

            // 3. 出牌阶段
            if (!skips.skip_play)
            {
                auto pr = run_play_phase(player);
                if (pr.is_err())
                    return TurnResult<void>::Err(pr.unwrap_err());
            }

            // 4. 弃牌阶段
            if (!TurnQuery::is_alive(m_ctx, player))
                return TurnResult<void>::Ok();
            return run_discard_phase(player);
        }
    }
}
