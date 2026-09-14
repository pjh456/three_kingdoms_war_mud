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
        inline std::vector<std::pair<card::Card, card::Card>> two_cards_as_sha_pairs(
            const ReadOnlyContext &ctx, const std::string &player)
        {
            std::vector<std::pair<card::Card, card::Card>> out;
            if (!has_ability(ctx, player, card::Ability::TwoCardsAsSha))
                return out;
            const auto &hand = ctx.cards->hand(player);
            if (hand.size() < 2 || find_sha_def(ctx).is_none())
                return out;
            for (const auto &c : hand)
            {
                const auto d = ctx.catalog->find(c.def_id);
                if (d.is_some() && d.unwrap()->effect.is_some() &&
                    is_sha_kind(d.unwrap()->effect.unwrap().kind))
                    return out;  // 手牌有真杀：不产出虚拟杀
            }
            for (std::size_t i = 0; i + 1 < hand.size(); ++i)
                for (std::size_t j = i + 1; j < hand.size(); ++j)
                    out.emplace_back(hand[i], hand[j]);
            return out;
        }

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
        inline std::vector<LegalAction> legal_actions(
            const ReadOnlyContext &ctx, const std::string &player, const TurnContext &turn)
        {
            std::vector<LegalAction> out;
            // 重铸候选单独收集，统一追加在全部正常动作之后：分组时正常动作
            // 先入组，AI 的目标选择不会落到空目标的重铸动作上（不主动重铸）
            std::vector<LegalAction> recasts;

            for (const auto &c : ctx.cards->hand(player))
            {
                const auto def_opt = ctx.catalog->find(c.def_id);
                if (def_opt.is_none())
                    continue;
                const card::CardDef &def = *def_opt.unwrap();

                if (def.recast)
                    recasts.push_back(LegalAction{c, {}, "", true});

                switch (classify_action(def))
                {
                case PlayClass::Equipment:
                    out.push_back(LegalAction{c, {}});
                    continue;

                case PlayClass::DelayedTrick:
                {
                    for (const auto &t : delayed_legal_targets(ctx, player, def))
                        out.push_back(LegalAction{c, {t}});
                    continue;
                }

                case PlayClass::Active:
                case PlayClass::None:
                    break;
                }

                if (def.effect.is_none())
                    continue;
                const auto kind = def.effect.unwrap().kind;
                if (!is_settleable_kind(kind))
                    continue;

                if (kind == card::CardEffectKind::BorrowedSword)
                {
                    // targets = {A(持武器者), B(A攻击范围内另一名角色)}
                    const auto view = ctx.entities->const_view();
                    for (const auto *e : view)
                    {
                        const std::string &holder = e->get_id();
                        if (holder == player)
                            continue;
                        if (!has_equip_slot(ctx, holder, card::EquipSlot::Weapon))
                            continue;
                        for (const auto *b : view)
                        {
                            const std::string &victim = b->get_id();
                            if (victim != holder &&
                                DistanceQuery(ctx).in_attack_range(holder, victim))
                                out.push_back(LegalAction{c, {holder, victim}});
                        }
                    }
                    continue;
                }

                auto targets = valid_targets(ctx, player, def);
                if (effect_traits(kind).target_card)
                {
                    std::vector<std::string> with_cards;
                    for (const auto &t : targets)
                        if (entity_has_any_card(ctx, t))
                            with_cards.push_back(t);
                    targets = std::move(with_cards);
                }
                if (targets.empty())
                    continue;

                const auto scope = def.effect.unwrap().scope.unwrap_or(card::Scope::Self);
                if (scope == card::Scope::OneOther || scope == card::Scope::AnyOne)
                {
                    // 单目标：每个通过校验的候选目标各产出一个动作
                    for (const auto &t : targets)
                    {
                        const auto ok = validate_play_action(
                            ctx, player, def, c, std::vector<std::string>{t}, turn)
                            .is_ok();
                        if (!ok)
                            continue;
                        out.push_back(LegalAction{c, {t}});

                        // 方天画戟：杀是最后一张手牌时，对同一原目标再产出一个
                        // 多目标动作（原目标 + 至多 2 名其他在范围内角色，
                        // 按实体序确定性截断，避免组合爆炸）
                        if (kind == card::CardEffectKind::Damage &&
                            sha_multi_target(ctx, player))
                        {
                            std::vector<std::string> combo{t};
                            for (const auto &u : targets)
                            {
                                if (u != t)
                                    combo.push_back(u);
                                if (combo.size() >=
                                    static_cast<std::size_t>(
                                        rules_of(ctx).sha_multi_target_max))
                                    break;
                            }
                            if (combo.size() > 1 &&
                                validate_play_action(
                                    ctx, player, def, c, combo, turn).is_ok())
                                out.push_back(LegalAction{c, std::move(combo)});
                        }
                    }
                }
                else if (scope == card::Scope::OneOrTwo)
                {
                    // 单目标动作在前，双目标组合（i<j）在后；均过统一校验，
                    // 确定性顺序 = valid_targets 序，组合上界 C(n,2)
                    for (const auto &t : targets)
                        if (validate_play_action(
                                ctx, player, def, c, std::vector<std::string>{t}, turn)
                                .is_ok())
                            out.push_back(LegalAction{c, {t}});

                    for (std::size_t i = 0; i < targets.size(); ++i)
                        for (std::size_t j = i + 1; j < targets.size(); ++j)
                        {
                            std::vector<std::string> combo{targets[i], targets[j]};
                            if (validate_play_action(ctx, player, def, c, combo, turn)
                                    .is_ok())
                                out.push_back(LegalAction{c, std::move(combo)});
                        }
                }
                else
                {
                    const auto ok =
                        validate_play_action(ctx, player, def, c, targets, turn).is_ok();
                    if (ok)
                        out.push_back(LegalAction{c, std::move(targets)});
                }
            }

            // 丈八蛇矛：手牌无真杀时，两张手牌当一张杀（pair × 目标枚举，
            // 确定性手牌序；杀次数/目标合法性经 validate_virtual_sha 过滤）
            const auto pairs = two_cards_as_sha_pairs(ctx, player);
            if (!pairs.empty())
            {
                const auto sha_def = find_sha_def(ctx);
                const auto targets = valid_targets(ctx, player, *sha_def.unwrap());
                const bool multi = sha_multi_target(ctx, player, 2);
                for (const auto &[first, second] : pairs)
                    for (const auto &t : targets)
                    {
                        if (!validate_virtual_sha(
                                ctx, player, first.instance_id, second.instance_id,
                                std::vector<std::string>{t}, turn)
                                .is_ok())
                            continue;
                        out.push_back(LegalAction{first, {t}, second.instance_id});

                        // 方天画戟：pair 为最后两张手牌时再产出多目标
                        // 动作（原目标 + 至多 2 名其他在范围内角色）
                        if (multi)
                        {
                            std::vector<std::string> combo{t};
                            for (const auto &u : targets)
                            {
                                if (u != t)
                                    combo.push_back(u);
                                if (combo.size() >=
                                    static_cast<std::size_t>(
                                        rules_of(ctx).sha_multi_target_max))
                                    break;
                            }
                            if (combo.size() > 1 &&
                                validate_virtual_sha(
                                    ctx, player, first.instance_id,
                                    second.instance_id, combo, turn)
                                    .is_ok())
                                out.push_back(LegalAction{
                                    first, std::move(combo), second.instance_id});
                        }
                    }
            }

            // 单张转化当杀（武圣红牌 / 龙胆闪，真杀已有普通动作不重复；
            // 杀次数/目标合法性经 validate_virtual_sha 过滤）
            const auto conversion_cards = sha_conversion_cards(ctx, player);
            const auto sha_def = find_sha_def(ctx);
            if (!conversion_cards.empty() && sha_def.is_some())
            {
                const auto targets = valid_targets(ctx, player, *sha_def.unwrap());
                const bool multi = sha_multi_target(ctx, player);
                for (const auto &c : conversion_cards)
                    for (const auto &t : targets)
                    {
                        if (!validate_virtual_sha(
                                ctx, player, c.instance_id, "",
                                std::vector<std::string>{t}, turn)
                                .is_ok())
                            continue;
                        out.push_back(LegalAction{c, {t}, "", false, true});

                        // 方天画戟：该红牌为最后一张手牌时再产出多目标
                        // 动作（原目标 + 至多 2 名其他在范围内角色）
                        if (multi)
                        {
                            std::vector<std::string> combo{t};
                            for (const auto &u : targets)
                            {
                                if (u != t)
                                    combo.push_back(u);
                                if (combo.size() >=
                                    static_cast<std::size_t>(
                                        rules_of(ctx).sha_multi_target_max))
                                    break;
                            }
                            if (combo.size() > 1 &&
                                validate_virtual_sha(
                                    ctx, player, c.instance_id, "", combo, turn)
                                    .is_ok())
                                out.push_back(LegalAction{
                                    c, std::move(combo), "", false, true});
                        }
                    }
            }

            out.insert(out.end(), recasts.begin(), recasts.end());
            return out;
        }
    }
}

#endif  // INCLUDE_TKW_GAME_LEGAL_HPP
