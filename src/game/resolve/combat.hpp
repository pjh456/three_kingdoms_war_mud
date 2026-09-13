/**
 * @file combat.hpp
 * @brief 战斗流程：伤害 → 濒死救场（桃）→ 死亡声明与击杀奖惩。
 * @note 这是 entity/event.hpp 注释里「由 combat 发布」的职责归属：
 *       - hp 扣到非正 → 进入濒死：从濒死角色起按座位序轮询打桃；
 *       - 一轮无人可救/不救 → 死亡：区域牌弃置、发布 EntityDiedEvent、移除实体；
 *       - 击杀奖惩：乱斗按通用规则给击杀者发奖励，身份局按死者角色与击杀者
 *         身份结算（击杀反贼发奖励；主公击杀忠臣弃光其手牌与装备；其余无奖）。
 * @note 无武将技能：能作濒死救场牌的只有资源标记 rescue 的牌。
 */

#ifndef INCLUDE_TKW_GAME_COMBAT_HPP
#define INCLUDE_TKW_GAME_COMBAT_HPP

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "card/catalog.hpp"
#include "card/def.hpp"
#include "card/manager.hpp"
#include "entity/event.hpp"
#include "entity/manager.hpp"
#include "event/event_bus.hpp"
#include "game/core/context.hpp"
#include "game/core/decision.hpp"
#include "game/core/effect.hpp"
#include "game/core/state.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 玩家手牌中是否有桃。 */
        inline bool has_peach(const GameContext &ctx, const std::string &player)
        {
            return any_hand_card_matching(
                ctx, player,
                [](const card::CardDef &def) { return is_rescue_def(def); });
        }

        /** @brief 消耗玩家指定的救场牌（校验确为救场牌）；失败返回 false。 */
        inline bool consume_peach(
            GameContext &ctx, const std::string &player,
            const std::string &instance_id)
        {
            return consume_hand_card_matching(
                       ctx, player, instance_id,
                       [](const card::CardDef &def) { return is_rescue_def(def); },
                       DiscardKind::Response)
                .is_some();
        }

        /** @brief 死亡清场：手牌/装备/判定区全部置入弃牌堆，移除实体并发布死亡事件。 */
        inline void declare_death(GameContext &ctx, const std::string &player)
        {
            for (const auto &c : ctx.cards->discard_all(player))
                emit_card_discarded(ctx, player, c);
            auto ev = std::make_shared<EntityDiedEvent>();
            ev->entity_id = player;
            ctx.bus->publish(ev);
            ctx.entities->remove(player);
        }

        /**
         * @brief 濒死救场：hp ≤ 0 时从濒死角色起按座位序轮询打桃。
         * @return true = 死亡；false = 救回（hp > 0）。
         */
        inline bool resolve_dying(
            GameContext &ctx, DecisionSource &ai, const std::string &dying)
        {
            const auto de = ctx.entities->find(dying);
            if (de.is_none())
                return true;
            {
                auto ev = std::make_shared<EntityDyingEvent>();
                ev->target = dying;
                ev->current_hp = de.unwrap()->get_hp();
                ctx.bus->publish(ev);
            }

            // 座位序，从濒死角色开始
            const auto order = ctx.entities->order_from(dying);

            int rounds = 0;
            while (true)
            {
                const auto e = ctx.entities->find(dying);
                if (e.is_none())
                    return true;
                if (e.unwrap()->get_hp() > 0)
                    return false;

                bool progress = false;
                for (const auto &saver : order)
                {
                    if (!has_peach(ctx, saver))
                        continue;
                    const auto chosen = ai.play_peach(ctx, saver, dying);
                    if (chosen.is_none())
                        continue;
                    if (!consume_peach(ctx, saver, chosen.unwrap()))
                        continue;
                    apply_heal(ctx, dying, rules_of(ctx).rescue_heal);
                    progress = true;
                    const auto cur = ctx.entities->find(dying);
                    if (cur.is_none())
                        return true;
                    if (cur.unwrap()->get_hp() > 0)
                        return false;
                }
                if (!progress)
                    break;
                if (++rounds > rules_of(ctx).dying_rounds)
                    break;  // 保险（桃数量有限，理论上到不了）
            }

            declare_death(ctx, dying);
            return true;
        }

        /**
         * @brief 弃光某玩家的手牌与装备（逐张移除并发布弃置事件）。
         * @param player 被弃牌玩家。
         * @note 不含判定区：判定区多是他人置入的延时锦囊，不属于「手牌与装备」。
         */
        inline void discard_hand_and_equip(GameContext &ctx, const std::string &player)
        {
            // 按值拷贝：移除会改动区域，遍历期间不能持有区域视图引用
            const auto hands = ctx.cards->hand(player);
            for (const auto &c : hands)
            {
                auto removed = ctx.cards->remove_from_hand(player, c.instance_id);
                if (removed.is_none())
                    continue;
                auto keep = std::move(removed).unwrap();
                discard_and_emit(ctx, player, keep);
            }

            const auto equips = ctx.cards->equip(player);
            for (const auto &c : equips)
            {
                auto removed = ctx.cards->remove_from_equip(player, c.instance_id);
                if (removed.is_none())
                    continue;
                auto keep = std::move(removed).unwrap();
                discard_and_emit(ctx, player, keep);
            }
        }

        /**
         * @brief 击杀奖惩：乱斗给击杀者通用奖励，身份局按角色结算。
         * @param source 伤害来源（空串或已不在场 = 无奖惩）。
         * @param target 死亡角色。
         * @note 身份局：击杀反贼给来源摸 kill_reward 张；主公击杀忠臣弃光其手牌
         *       与装备（不含判定区）；主公/内奸/未知角色无奖励。
         */
        inline void apply_kill_effect(
            GameContext &ctx, const std::string &source, const std::string &target)
        {
            if (source.empty() || ctx.entities->find(source).is_none())
                return;

            if (mode_of(ctx) != GameMode::Identity)
            {
                apply_draw(ctx, source, rules_of(ctx).kill_reward);
                return;
            }

            switch (role_of(ctx, target))
            {
            case Role::Rebel:
                apply_draw(ctx, source, rules_of(ctx).kill_reward);
                break;
            case Role::Loyalist:
                if (role_of(ctx, source) == Role::Lord)
                    discard_hand_and_equip(ctx, source);
                break;
            default:
                break;  // 主公（终局）与内奸：无奖励
            }
        }

        /**
         * @brief 造成伤害（流程入口）：扣血 → 濒死判定 → 死亡与击杀奖惩。
         * @param source 伤害来源（空串 = 无来源如闪电；为存活玩家时按模式与角色发奖惩）。
         */
        inline void deal_damage(
            GameContext &ctx, DecisionSource &ai, const std::string &source,
            const std::string &target, int amount)
        {
            const auto e = ctx.entities->find(target);
            if (e.is_none())
                return;
            e.unwrap()->take_damage(source, amount, false);
            if (e.unwrap()->get_hp() > 0)
                return;

            const bool died = resolve_dying(ctx, ai, target);
            if (died && !source.empty())
                apply_kill_effect(ctx, source, target);
        }
    }
}

#endif  // INCLUDE_TKW_GAME_COMBAT_HPP