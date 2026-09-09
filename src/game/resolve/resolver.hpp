/**
 * @file resolver.hpp
 * @brief 效果结算器：按 CardEffectKind 分派，把「打出的牌」作用于对局状态。
 * @note 只实现核心结算（杀/闪/桃/无中生有/过拆/顺牵/南蛮/万箭/决斗/桃园/
 *       五谷/借刀）；无懈/延时等返回 UnsupportedKind，作为明确未接缝；
 *       装备走 equip_card，不经本文件效果结算。
 *       响应牌消费、目标校验、距离校验都在本模块（单一写者），
 *       玩家主观选择经 DecisionSource 询问。
 */

#ifndef INCLUDE_TKW_GAME_RESOLVER_HPP
#define INCLUDE_TKW_GAME_RESOLVER_HPP

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "card/catalog.hpp"
#include "card/def.hpp"
#include "card/manager.hpp"
#include "entity/manager.hpp"
#include "event/event_bus.hpp"
#include "game/resolve/combat.hpp"
#include "game/core/context.hpp"
#include "game/resolve/counter.hpp"
#include "game/core/decision.hpp"
#include "game/query/distance.hpp"
#include "game/query/judge.hpp"
#include "game/core/effect.hpp"
#include "game/query/equip.hpp"
#include "game/resolve/response.hpp"
#include "game/core/state.hpp"
#include "game/resolve/weapon.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 结算错误。 */
        enum class EffectError : std::uint8_t
        {
            UnknownCard,     /**< 目录中找不到该卡定义 */
            UnsupportedKind, /**< 该 effect.kind 尚未实现 */
            NoTarget,        /**< 需要至少一个目标 */
            OutOfRange,      /**< 目标不在攻击范围/距离内 */
            InvalidTarget,   /**< 目标数量不符 scope / 不在合法目标集合内 */
            CardNotOwned,    /**< 打出的牌不在该玩家手牌中 */
            InvalidChoice,   /**< 决策源选中的牌不存在于目标区域 */
            ShaLimitExceeded,/**< 本回合杀次数已达上限 */
            DelayedDuplicate,/**< 判定区已有同名的延时锦囊 */
        };

        template <typename T>
        using GameResult = Result<T, EffectError>;

        // 效果类别属性（is_settleable_kind / is_unimplemented_active_kind）见 effect.hpp

        // 杀响应窗口入口（决斗/南蛮/借刀共用；定义见结算区末尾）
        inline bool respond_sha(
            GameContext &ctx, DecisionSource &ai,
            const std::string &entity, const std::string &victim);

        // ── 目标选择 ────────────────────────────────────────────────────

        /**
         * @brief 牌堆中的「杀」定义（Damage 类主动效果的卡，标准牌堆恰好一张）。
         * @return 杀定义；None = 牌堆无杀（虚拟杀无从结算）。
         * @note 虚拟杀（两张手牌当一张杀）借用其效果参数（伤害量/作用范围），
         *       牌堆含多张 Damage 卡时取 deck 序第一张。
         */
        inline Option<const card::CardDef *> find_sha_def(const GameContext &ctx)
        {
            for (const auto &def : *ctx.catalog)
                if (def.effect.is_some() && is_sha_kind(def.effect.unwrap().kind))
                    return Option<const card::CardDef *>::Some(&def);
            return Option<const card::CardDef *>::None();
        }

        /** @brief 按 effect.scope 返回该牌在当前局面下的合法目标集合（含距离过滤）。 */
        inline std::vector<std::string> valid_targets(
            const GameContext &ctx, const std::string &player, const card::CardDef &def)
        {
            std::vector<std::string> out;
            if (def.effect.is_none())
                return out;
            const auto &eff = def.effect.unwrap();
            const auto scope = eff.scope.unwrap_or(card::Scope::Self);

            auto all_others = [&]()
            {
                for (const auto &e : *ctx.entities)
                    if (e->get_id() != player)
                        out.push_back(e->get_id());
            };

            switch (scope)
            {
            case card::Scope::Self:
                out.push_back(player);
                break;
            case card::Scope::All:
                for (const auto &e : *ctx.entities)
                    out.push_back(e->get_id());
                break;
            case card::Scope::AllOthers:
                all_others();
                break;
            case card::Scope::OneOther:
                all_others();
                if (eff.kind == card::CardEffectKind::Damage)
                {
                    out.erase(
                        std::remove_if(out.begin(), out.end(),
                                       [&](const std::string &t)
                                       { return !in_attack_range(ctx, player, t); }),
                        out.end());
                }
                else if (eff.kind == card::CardEffectKind::Steal)
                {
                    out.erase(
                        std::remove_if(out.begin(), out.end(),
                                       [&](const std::string &t)
                                       { return !distance_le(ctx, player, t, eff.range); }),
                        out.end());
                }
                break;
            }
            return out;
        }

        /**
         * @brief 主动效果的目标预校验（只读，不消费打出的牌）。
         * @param cards_consumed 该效果消耗的手牌张数（真杀 1，丈八虚拟杀 2），
         *        参与方天画戟「最后手牌」放宽判定。
         * @return Ok 通过；错误值：
         *         - NoTarget：无目标；
         *         - OutOfRange：目标超出攻击范围/距离；
         *         - InvalidTarget：目标数量不符 scope 或不在合法集合内。
         * @note 前置：def.effect.is_some()（结算入口与出牌动作校验两处均满足）。
         *       借刀杀人特例（targets = {持武器者, 其攻击范围内角色}）在此统一校验。
         *       方天画戟放宽：杀的目标为唯一目标且该杀消耗完手中全部牌时，
         *       OneOther 数量上限放宽为 3（额外至多 2 名，卡面）。
         */
        inline GameResult<void> validate_effect_targets(
            const GameContext &ctx, const std::string &player,
            const card::CardDef &def, const std::vector<std::string> &targets,
            std::size_t cards_consumed = 1)
        {
            const card::CardEffect &eff = def.effect.unwrap();

            // 预校验：目标非空 + 距离
            if (targets.empty())
                return GameResult<void>::Err(EffectError::NoTarget);
            if (eff.kind == card::CardEffectKind::Damage)
            {
                for (const auto &t : targets)
                    if (!in_attack_range(ctx, player, t))
                        return GameResult<void>::Err(EffectError::OutOfRange);
            }
            else if (eff.kind == card::CardEffectKind::Steal)
            {
                for (const auto &t : targets)
                    if (!distance_le(ctx, player, t, eff.range))
                        return GameResult<void>::Err(EffectError::OutOfRange);
            }

            // 目标合法性：数量须符合 scope，且每个目标都必须在合法集合内
            if (eff.kind == card::CardEffectKind::BorrowedSword)
            {
                // 借刀杀人：targets = {A(持武器者), B(A攻击范围内角色)}
                if (targets.size() != 2)
                    return GameResult<void>::Err(EffectError::InvalidTarget);
                const std::string &holder = targets[0];
                const std::string &victim = targets[1];
                if (holder == player || ctx.entities->find(holder).is_none() ||
                    ctx.entities->find(victim).is_none())
                    return GameResult<void>::Err(EffectError::InvalidTarget);
                if (!has_equip_slot(ctx, holder, card::EquipSlot::Weapon))
                    return GameResult<void>::Err(EffectError::InvalidTarget);
                if (!in_attack_range(ctx, holder, victim))
                    return GameResult<void>::Err(EffectError::OutOfRange);
            }
            else
            {
                const auto scope = eff.scope.unwrap_or(card::Scope::Self);
                const auto legal = valid_targets(ctx, player, def);
                const auto in_legal = [&](const std::string &t)
                {
                    return std::find(legal.begin(), legal.end(), t) != legal.end();
                };
                bool target_ok = true;
                switch (scope)
                {
                case card::Scope::Self:
                case card::Scope::OneOther:
                {
                    // 方天画戟：杀消耗完手中全部牌时共可指定至多 3 个目标
                    // （唯一目标 + 额外至多 2 名，卡面）；成员合法性由下方统一检查
                    const bool multi_sha =
                        eff.kind == card::CardEffectKind::Damage &&
                        sha_multi_target(ctx, player, cards_consumed);
                    target_ok = targets.size() <= (multi_sha ? 3 : 1);
                    break;
                }
                case card::Scope::All:
                case card::Scope::AllOthers:
                    // 去重后比对：All/AllOthers 须覆盖合法集合一次且仅一次，
                    // 重复目标（如 {a,a,b}）数量能对上但会重复结算/漏结算
                    {
                        std::vector<std::string> uniq = targets;
                        std::sort(uniq.begin(), uniq.end());
                        uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
                        target_ok = uniq.size() == legal.size();
                    }
                    break;
                }
                if (target_ok)
                {
                    for (const auto &t : targets)
                        if (!in_legal(t))
                        {
                            target_ok = false;
                            break;
                        }
                }
                if (!target_ok)
                    return GameResult<void>::Err(EffectError::InvalidTarget);
            }
            return GameResult<void>::Ok();
        }

        /**
         * @brief 校验「player 打出 card（定义 def），指定 targets」是否合法（只读预检）。
         * @return Ok 合法；错误值：
         *         - CardNotOwned：牌不在 player 手牌中；
         *         - InvalidTarget：延时锦囊目标数不为 1 或出 scope；
         *         - DelayedDuplicate：延时锦囊目标判定区已有同名延时锦囊；
         *         - ShaLimitExceeded：杀且本回合杀次数已达上限（turn）；
         *         - NoTarget/OutOfRange/InvalidTarget：主动效果目标校验失败；
         *         - UnsupportedKind：无主动效果，或效果未实现。
         * @note 检查顺序固定：手牌存在 → 按分类分派（装备直接合法、忽略目标；
         *       延时先 scope 后去重；主动先杀次数后目标再可实现性）→ 主动效果目标
         *       校验 → 可实现性。无副作用：不消费牌、不发事件；实际消费归
         *       equip_card / place_delayed / resolve_play。
         */
        inline GameResult<void> validate_play_action(
            const GameContext &ctx, const std::string &player,
            const card::CardDef &def, const card::Card &card,
            const std::vector<std::string> &targets, const TurnContext &turn)
        {
            // 打出的牌必须在手牌中
            bool in_hand = false;
            for (const auto &c : ctx.cards->hand(player))
                if (c.instance_id == card.instance_id)
                {
                    in_hand = true;
                    break;
                }
            if (!in_hand)
                return GameResult<void>::Err(EffectError::CardNotOwned);

            switch (classify_action(def))
            {
            case PlayClass::Equipment:
                return GameResult<void>::Ok();  // 装备走 equip_card，忽略目标

            case PlayClass::DelayedTrick:
                if (targets.size() != 1)
                    return GameResult<void>::Err(EffectError::InvalidTarget);
                if (!is_delayed_scope_target(player, def, targets.front()))
                    return GameResult<void>::Err(EffectError::InvalidTarget);
                if (has_same_delayed(ctx, targets.front(), def.id))
                    return GameResult<void>::Err(EffectError::DelayedDuplicate);
                return GameResult<void>::Ok();

            case PlayClass::Active:
                if (is_sha_kind(def.effect.unwrap().kind) &&
                    turn.sha_played >= turn.sha_limit)
                    return GameResult<void>::Err(EffectError::ShaLimitExceeded);
                break;

            case PlayClass::None:
                return GameResult<void>::Err(EffectError::UnsupportedKind);
            }

            // 主动效果：目标校验后判可实现性
            auto tr = validate_effect_targets(ctx, player, def, targets);
            if (tr.is_err())
                return tr;
            if (!is_settleable_kind(def.effect.unwrap().kind))
                return GameResult<void>::Err(EffectError::UnsupportedKind);
            return GameResult<void>::Ok();
        }

        /**
         * @brief 校验「两张手牌当一张杀」（丈八蛇矛）是否合法（只读预检）。
         * @return Ok 合法；错误值：
         *         - CardNotOwned：两张为同一张或任一不在 player 手牌中；
         *         - UnsupportedKind：未装备两张当杀能力，或牌堆无「杀」定义；
         *         - ShaLimitExceeded：杀且本回合杀次数已达上限（turn）；
         *         - NoTarget/OutOfRange/InvalidTarget：目标校验失败
         *         （攻击范围内的一名其他角色；方天画戟放宽与杀一致）。
         * @note 检查顺序：两牌在手 → 能力 → 杀次数 → 目标；无副作用：不消费
         *       牌、不发事件；实际消费归 resolve_virtual_sha。
         */
        inline GameResult<void> validate_virtual_sha(
            const GameContext &ctx, const std::string &player,
            const std::string &first_id, const std::string &second_id,
            const std::vector<std::string> &targets, const TurnContext &turn)
        {
            // 两张手牌须为不同牌且都在手牌中
            if (first_id == second_id)
                return GameResult<void>::Err(EffectError::CardNotOwned);
            bool have_first = false;
            bool have_second = false;
            for (const auto &c : ctx.cards->hand(player))
            {
                if (c.instance_id == first_id)
                    have_first = true;
                else if (c.instance_id == second_id)
                    have_second = true;
            }
            if (!have_first || !have_second)
                return GameResult<void>::Err(EffectError::CardNotOwned);

            const auto sha_def = find_sha_def(ctx);
            if (sha_def.is_none() ||
                !has_ability(ctx, player, card::Ability::TwoCardsAsSha))
                return GameResult<void>::Err(EffectError::UnsupportedKind);

            // 虚拟杀按一张杀计次数
            if (turn.sha_played >= turn.sha_limit)
                return GameResult<void>::Err(EffectError::ShaLimitExceeded);

            return validate_effect_targets(ctx, player, *sha_def.unwrap(), targets, 2);
        }

        // ── 结算入口 ────────────────────────────────────────────────────

        /**
         * @brief 结算「player 打出 played 牌，指定 targets」。
         * @note played 按值传入：结算过程会把该牌移出手牌，引用会失效。
         *       校验失败（未知卡/越范围/未实现/空目标）不消耗该牌；
         *       校验通过后先把打出的牌弃置，再应用效果。
         */
        inline GameResult<void> resolve_play(
            GameContext &ctx, DecisionSource &ai, const std::string &player,
            card::Card played, const std::vector<std::string> &targets)
        {
            const auto def_opt = ctx.catalog->find(played.def_id);
            if (def_opt.is_none())
                return GameResult<void>::Err(EffectError::UnknownCard);
            const card::CardDef &def = *def_opt.unwrap();
            if (def.effect.is_none())
            {
                // 装备由 equip_card 处理；其余无主动效果（如无懈）不可主动打出
                if (def.type == card::CardType::Equipment)
                    return GameResult<void>::Ok();
                return GameResult<void>::Err(EffectError::UnsupportedKind);
            }
            const card::CardEffect &eff = def.effect.unwrap();

            // 目标预校验（单一副本见 validate_effect_targets；本处为最终闸门）
            auto tr = validate_effect_targets(ctx, player, def, targets);
            if (tr.is_err())
                return tr;

            // 未实现的效果：不消耗打出的牌
            if (!is_settleable_kind(eff.kind))
                return GameResult<void>::Err(EffectError::UnsupportedKind);

            // 打出的牌必须在手牌中，否则拒绝（不消耗、不结算）
            auto played_removed = ctx.cards->remove_from_hand(player, played.instance_id);
            if (played_removed.is_none())
                return GameResult<void>::Err(EffectError::CardNotOwned);

            // 打出的牌先弃置（防止结算中被再次选中）
            ctx.cards->discard(std::move(played_removed).unwrap());
            emit_card_played(ctx, player, played);

            // 无懈可击只抵消锦囊牌；基本牌（杀/闪/桃）不可无懈
            const bool is_trick = def.type == card::CardType::Trick;
            const auto nullified = [&]()
            {
                return is_trick &&
                       resolve_nullification(ctx, ai, def, player, targets);
            };

            const auto apply = [&]() -> GameResult<void>
            {
                switch (eff.kind)
                {
                case card::CardEffectKind::Damage:
                    for (const auto &t : targets)
                        resolve_sha(
                            ctx, ai, player, played, t, eff.amount,
                            static_cast<int>(targets.size()));
                    return GameResult<void>::Ok();

                case card::CardEffectKind::AoeDamage:
                    for (const auto &t : targets)
                    {
                        if (nullified())
                            continue;
                        bool responded = false;
                        if (eff.response.is_some())
                        {
                            const auto kind = eff.response.unwrap();
                            responded = kind == card::ResponseKind::Sha
                                ? respond_sha(ctx, ai, t, "")
                                : request_response(ctx, ai, t, kind);
                        }
                        if (!responded)
                            deal_damage(ctx, ai, player, t, eff.amount);
                    }
                    return GameResult<void>::Ok();

                case card::CardEffectKind::Heal:
                    for (const auto &t : targets)
                    {
                        if (nullified())
                            continue;
                        apply_heal(ctx, t, eff.amount);
                    }
                    return GameResult<void>::Ok();

                case card::CardEffectKind::Draw:
                    if (nullified())
                        return GameResult<void>::Ok();
                    apply_draw(ctx, player, eff.count);
                    return GameResult<void>::Ok();

                case card::CardEffectKind::DiscardTarget:
                {
                    // 先收集并校验全部选择，再统一落子（事务性）
                    std::vector<std::pair<std::string, card::Card>> picks;
                    for (const auto &t : targets)
                    {
                        if (nullified())
                            continue;
                        const auto picked = ai.pick_card_from_target(ctx, player, t);
                        if (picked.is_none() ||
                            !ctx.cards->has_card(t, picked.unwrap().instance_id))
                            return GameResult<void>::Err(EffectError::InvalidChoice);
                        picks.emplace_back(t, picked.unwrap());
                    }
                    for (const auto &[owner, picked_card] : picks)
                    {
                        card::Card removed;
                        if (!remove_card_from_zones(
                                ctx, owner, picked_card.instance_id, removed))
                            return GameResult<void>::Err(EffectError::InvalidChoice);
                        ctx.cards->discard(removed);
                        emit_card_discarded(ctx, owner, removed);
                    }
                    return GameResult<void>::Ok();
                }

                case card::CardEffectKind::Steal:
                {
                    // 先收集并校验全部选择，再统一落子（事务性）
                    std::vector<std::pair<std::string, card::Card>> picks;
                    for (const auto &t : targets)
                    {
                        if (nullified())
                            continue;
                        const auto picked = ai.pick_card_from_target(ctx, player, t);
                        if (picked.is_none() ||
                            !ctx.cards->has_card(t, picked.unwrap().instance_id))
                            return GameResult<void>::Err(EffectError::InvalidChoice);
                        picks.emplace_back(t, picked.unwrap());
                    }
                    for (const auto &[owner, picked_card] : picks)
                    {
                        card::Card removed;
                        Zone from = Zone::Limbo;
                        if (!remove_card_from_zones(
                                ctx, owner, picked_card.instance_id, removed, &from))
                            return GameResult<void>::Err(EffectError::InvalidChoice);
                        ctx.cards->add_to_hand(player, removed);
                        emit_card_moved(ctx, owner, player, removed, from, Zone::Hand);
                    }
                    return GameResult<void>::Ok();
                }

                case card::CardEffectKind::Duel:
                    if (nullified())
                        return GameResult<void>::Ok();
                    {
                        // 目标先开始，轮流打出杀；先不出的受对方 1 点伤害；
                        // 轮次耗尽（双方每轮都出了杀）平局结算。
                        std::string attacker = player;
                        std::string defender = targets.front();
                        for (int round = 0; round < rules_of(ctx).duel_rounds; ++round)
                        {
                            if (!respond_sha(ctx, ai, defender, ""))
                            {
                                deal_damage(ctx, ai, attacker, defender, eff.amount);
                                return GameResult<void>::Ok();
                            }
                            std::swap(attacker, defender);
                        }
                        // 轮次耗尽：双方每轮都出了杀、无人「先不出」→ 平局，不再造成伤害
                        return GameResult<void>::Ok();
                    }

                case card::CardEffectKind::RevealPick:
                {
                    if (nullified())
                        return GameResult<void>::Ok();

                    // 亮出等同存活人数的牌
                    std::vector<card::Card> revealed;
                    const int n = static_cast<int>(ctx.entities->size());
                    for (int i = 0; i < n; ++i)
                    {
                        auto c = ctx.cards->draw();
                        if (c.is_none())
                            break;
                        revealed.push_back(std::move(c).unwrap());
                    }

                    // 按座位序（从使用者开始）依次选一张
                    for (const auto &p : ctx.entities->order_from(player))
                    {
                        if (revealed.empty())
                            break;
                        const auto picked = ai.pick_from_revealed(ctx, p, revealed);
                        std::size_t idx = 0;
                        if (picked.is_some())
                            for (std::size_t k = 0; k < revealed.size(); ++k)
                                if (revealed[k].instance_id ==
                                    picked.unwrap().instance_id)
                                {
                                    idx = k;
                                    break;
                                }
                        card::Card chosen = revealed[idx];
                        revealed.erase(revealed.begin() + std::ptrdiff_t(idx));
                        ctx.cards->add_to_hand(p, chosen);
                        emit_card_moved(ctx, "", p, chosen, Zone::Limbo, Zone::Hand);
                    }
                    // 剩余置入弃牌堆
                    for (const auto &c : revealed)
                    {
                        ctx.cards->discard(c);
                        emit_card_discarded(ctx, "", c);
                    }
                    return GameResult<void>::Ok();
                }

                case card::CardEffectKind::BorrowedSword:
                {
                    if (nullified())
                        return GameResult<void>::Ok();
                    const std::string &holder = targets[0];
                    const std::string &victim = targets[1];

                    if (respond_sha(ctx, ai, holder, victim))
                        return GameResult<void>::Ok();

                    // 未出杀：使用者获得 holder 的武器
                    for (const auto &c : ctx.cards->equip(holder))
                    {
                        const auto d = ctx.catalog->find(c.def_id);
                        if (d.is_some() && d.unwrap()->equip.is_some() &&
                            d.unwrap()->equip.unwrap().slot ==
                                card::EquipSlot::Weapon)
                        {
                            auto removed =
                                ctx.cards->remove_from_equip(holder, c.instance_id);
                            if (removed.is_some())
                            {
                                card::Card weapon = std::move(removed).unwrap();
                                ctx.cards->add_to_hand(player, weapon);
                                emit_card_moved(
                                    ctx, holder, player, weapon, Zone::Equip,
                                    Zone::Hand);
                            }
                            break;
                        }
                    }
                    return GameResult<void>::Ok();
                }

                default:
                    return GameResult<void>::Err(EffectError::UnsupportedKind);
                }
            };

            const auto rr = apply();
            if (rr.is_err())
            {
                // 结算失败回滚：仅取回打出的牌；结算中途已消耗的响应牌
                // （如决斗中打出的杀）不回退
                auto back = ctx.cards->remove_from_discard(played.instance_id);
                if (back.is_some())
                    ctx.cards->add_to_hand(player, std::move(back).unwrap());
            }
            return rr;
        }

         /**
          * @brief 结算「两张手牌当一张杀」（丈八蛇矛）。
          * @param first_id/second_id 两张手牌的 instance_id。
          * @param validate_targets 是否复验目标（最终闸门，含方天画戟放宽）；
          *        结算目标由引擎固定时（杀响应窗口）传 false 跳过——目标在
          *        打出时已以同一距离谓词校验，与真杀响应路径一致。
          * @note 目标校验（最终闸门，含方天画戟放宽）与两牌在手检查先于消费，
          *       失败不消耗牌；消费后两张各进弃牌堆并各发打出事件（防结算中
          *       被再选），再逐目标按虚拟杀结算。虚拟杀无花色：仁王盾黑杀
          *       判定不适用（经 resolve_sha 的 virtual 标记短路）。
          */
         inline GameResult<void> resolve_virtual_sha(
             GameContext &ctx, DecisionSource &ai, const std::string &player,
             const std::string &first_id, const std::string &second_id,
             const std::vector<std::string> &targets, bool validate_targets = true)
         {
             const auto sha_def = find_sha_def(ctx);
             if (sha_def.is_none())
                 return GameResult<void>::Err(EffectError::UnsupportedKind);
             const card::CardDef &sha = *sha_def.unwrap();
             const card::CardEffect &eff = sha.effect.unwrap();

             // 目标预校验（最终闸门；方天放宽按消耗两张手牌判定）
             if (validate_targets)
             {
                 auto tr = validate_effect_targets(ctx, player, sha, targets, 2);
                 if (tr.is_err())
                     return tr;
             }

            // 两张牌都在手牌中（校验先于消费，失败不消耗）
            bool have_first = false;
            bool have_second = false;
            for (const auto &c : ctx.cards->hand(player))
            {
                if (c.instance_id == first_id)
                    have_first = true;
                else if (c.instance_id == second_id)
                    have_second = true;
            }
            if (first_id == second_id || !have_first || !have_second)
                return GameResult<void>::Err(EffectError::CardNotOwned);

            // 两张牌先弃置（防止结算中被再次选中）
            auto removed = ctx.cards->remove_from_hand(player, first_id);
            if (removed.is_none())
                return GameResult<void>::Err(EffectError::CardNotOwned);
            card::Card first = std::move(removed).unwrap();
            ctx.cards->discard(first);
            emit_card_played(ctx, player, first);
            removed = ctx.cards->remove_from_hand(player, second_id);
            if (removed.is_none())
                return GameResult<void>::Err(EffectError::CardNotOwned);
            card::Card second = std::move(removed).unwrap();
            ctx.cards->discard(second);
            emit_card_played(ctx, player, second);

            // 逐目标虚拟杀结算（无实体牌：花色仅仁王盾黑杀判定消费，已短路）
            const card::Card virtual_sha;
            for (const auto &t : targets)
                resolve_sha(ctx, ai, player, virtual_sha, t, eff.amount,
                            static_cast<int>(targets.size()), true);
            return GameResult<void>::Ok();
        }

        /**
         * @brief 开杀响应窗口并消费响应杀：响应者打出一张真杀，或（装备两张当杀
         *        能力时）打出两张手牌当杀。消费在本函数内完成（弃置+事件）；
         *        给出结算目标（借刀的 B）时再按杀对其结算（真杀带花色、虚拟杀
         *        无花色），否则仅消费（决斗/南蛮无结算目标）。
         * @return 是否发生了有效杀响应；非法选择（幽灵引用/非杀的牌）按不响应
          *         处理，不消耗牌。
          * @note 响应侧不受出牌阶段杀次数限制（次数是出牌阶段「本回合已用杀」的
          *         簿记，响应窗口不在出牌阶段簿记内）。结算目标由引擎固定（借刀
          *         的 B，打出时已以同一距离谓词校验），响应侧不复核目标，与真杀
          *         响应路径一致。借刀响应事件语法：对目标结算=打出、仅消费=弃置
          *         （真杀与虚拟杀同口径）。
          */
        inline bool respond_sha(
            GameContext &ctx, DecisionSource &ai,
            const std::string &entity, const std::string &victim)
        {
            if (!has_response_card(ctx, entity, card::ResponseKind::Sha))
                return false;
            const auto chosen = ai.play_response(ctx, entity, card::ResponseKind::Sha);
            if (chosen.is_none())
                return false;
            const auto &act = chosen.unwrap();

            if (!act.second_instance_id.empty())
            {
                // 两张当杀（丈八蛇矛）：先校验，非法选择按不响应、不消耗
                bool in_first = false;
                bool in_second = false;
                if (act.instance_id != act.second_instance_id)
                {
                    for (const auto &c : ctx.cards->hand(entity))
                    {
                        if (c.instance_id == act.instance_id)
                            in_first = true;
                        else if (c.instance_id == act.second_instance_id)
                            in_second = true;
                    }
                }
                // 能力/两牌在手闸；结算目标引擎固定，不复核（见 @note）
                if (!has_ability(ctx, entity, card::Ability::TwoCardsAsSha) ||
                    find_sha_def(ctx).is_none() || !in_first || !in_second)
                    return false;

                // 有结算目标：虚拟杀结算接管（消费+逐目标结算，跳过目标复验）
                if (!victim.empty())
                    return resolve_virtual_sha(
                               ctx, ai, entity, act.instance_id,
                               act.second_instance_id,
                               std::vector<std::string>{victim}, false)
                         .is_ok();

                // 无结算目标（决斗/南蛮）：仅消费两张
                auto removed = ctx.cards->remove_from_hand(entity, act.instance_id);
                if (removed.is_none())
                    return false;
                card::Card first = std::move(removed).unwrap();
                ctx.cards->discard(first);
                emit_card_played(ctx, entity, first);
                removed = ctx.cards->remove_from_hand(entity, act.second_instance_id);
                if (removed.is_none())
                    return false;
                card::Card second = std::move(removed).unwrap();
                ctx.cards->discard(second);
                emit_card_played(ctx, entity, second);
                return true;
            }

            // 真杀：单牌消费（与既有响应窗口行为一致）
            auto removed = ctx.cards->remove_from_hand(entity, act.instance_id);
            if (removed.is_none())
                return false;
            card::Card card = std::move(removed).unwrap();
            const auto def = ctx.catalog->find(card.def_id);
            if (def.is_none() ||
                !is_response_def(*def.unwrap(), card::ResponseKind::Sha))
            {
                ctx.cards->add_to_hand(entity, std::move(card));
                return false;
            }
            ctx.cards->discard(card);
            if (!victim.empty())
            {
                // 对目标结算的响应计打出（与虚拟杀响应同口径）
                int dmg = 1;
                if (def.unwrap()->effect.is_some())
                    dmg = def.unwrap()->effect.unwrap().amount;
                emit_card_played(ctx, entity, card);
                resolve_sha(ctx, ai, entity, card, victim, dmg);
            }
            else
                emit_card_discarded(ctx, entity, card);
            return true;
        }
    }
}

#endif  // INCLUDE_TKW_GAME_RESOLVER_HPP