/**
 * @file validate.hpp
 * @brief 出牌动作只读校验：目标选择与合法性预检，供流程与 AI 枚举共用。
 * @note 与动作枚举（ai/legal.hpp）同源：枚举器产出的动作都应能通过本层校验。
 *       只读无副作用：不消费牌、不发事件；实际消费归 equip_card /
 *       place_delayed / resolve_play。校验与落子分离：新增效果先加校验分支，
 *       再加结算分支。
 */

#ifndef INCLUDE_TKW_GAME_VALIDATE_HPP
#define INCLUDE_TKW_GAME_VALIDATE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "card/def.hpp"
#include "card/manager.hpp"
#include "game/core/context.hpp"
#include "game/core/decision.hpp"
#include "game/core/effect.hpp"
#include "game/query/distance.hpp"
#include "game/query/equip.hpp"
#include "game/query/judge.hpp"
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
            ShaLimitExceeded, /**< 本回合杀次数已达上限 */
            DelayedDuplicate, /**< 判定区已有同名的延时锦囊 */
        };

        template <typename T>
        using GameResult = Result<T, EffectError>;

        // ── 目标选择 ────────────────────────────────────────────────────

        /**
         * @brief 牌堆中的「杀」定义（Damage 类主动效果的卡，标准牌堆恰好一张）。
         * @return 杀定义；None = 牌堆无杀（虚拟杀无从结算）。
         * @note 虚拟杀（两张手牌当一张杀）借用其效果参数（伤害量/作用范围），
         *       牌堆含多张 Damage 卡时取 deck 序第一张。
         */
        inline Option<const card::CardDef *> find_sha_def(const ReadOnlyContext &ctx)
        {
            for (const auto &def : *ctx.catalog)
                if (def.effect.is_some() && is_sha_kind(def.effect.unwrap().kind))
                    return Option<const card::CardDef *>::Some(&def);
            return Option<const card::CardDef *>::None();
        }

        /** @brief 按 effect.scope 返回该牌在当前局面下的合法目标集合（含距离过滤）。 */
        inline std::vector<std::string> valid_targets(
            const ReadOnlyContext &ctx, const std::string &player, const card::CardDef &def)
        {
            std::vector<std::string> out;
            if (def.effect.is_none())
                return out;
            const auto &eff = def.effect.unwrap();
            const auto scope = eff.scope.unwrap_or(card::Scope::Self);

            auto all_others = [&]()
            {
                for (const auto *e : ctx.entities->const_view())
                    if (e->get_id() != player)
                        out.push_back(e->get_id());
            };

            switch (scope)
            {
            case card::Scope::Self:
                out.push_back(player);
                break;
            case card::Scope::All:
                for (const auto *e : ctx.entities->const_view())
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
         *       借刀杀人特例（targets = {持武器者, 其攻击范围内另一名角色}）在此统一校验。
         *       方天画戟放宽：杀的目标为唯一目标且该杀消耗完手中全部牌时，
         *       OneOther 数量上限放宽为 3（额外至多 2 名，卡面）。
         */
        inline GameResult<void> validate_effect_targets(
            const ReadOnlyContext &ctx, const std::string &player,
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
                // 借刀杀人：targets = {A(持武器者), B(A攻击范围内另一名角色)}
                if (targets.size() != 2)
                    return GameResult<void>::Err(EffectError::InvalidTarget);
                const std::string &holder = targets[0];
                const std::string &victim = targets[1];
                if (holder == player || holder == victim ||
                    ctx.entities->find(holder).is_none() ||
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
                    const std::size_t multi_max = static_cast<std::size_t>(
                        rules_of(ctx).sha_multi_target_max);
                    target_ok = targets.size() <= (multi_sha ? multi_max : 1);
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
            const ReadOnlyContext &ctx, const std::string &player,
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
            const ReadOnlyContext &ctx, const std::string &player,
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
    }
}

#endif  // INCLUDE_TKW_GAME_VALIDATE_HPP
