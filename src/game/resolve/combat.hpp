/**
 * @file combat.hpp
 * @brief 战斗流程：伤害 → 濒死救场（桃）→ 死亡声明与击杀奖惩。
 * @note 这是 entity/event.hpp 注释里「由 combat 发布」的职责归属：
 *       - hp 扣到非正 → 进入濒死：从当前回合角色起按座位序轮询打桃
 *         （无回合上下文时回落濒死者起）；
 *       - 一轮无人可救/不救 → 死亡：区域牌弃置、发布 EntityDiedEvent、移除实体；
 *       - 击杀奖惩：乱斗按通用规则给击杀者发奖励，身份局按死者角色与击杀者
 *         身份结算（击杀反贼发奖励；主公击杀忠臣弃光其手牌与装备；其余无奖）。
 * @note 无武将技能：能作濒死救场牌的只有资源标记 rescue（救任意人）或
 *       self_rescue（仅濒死者本人，如酒）的牌。
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
#include "game/query/equip.hpp"
#include "game/resolve/skill.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /**
         * @brief 玩家手牌中是否有可作濒死救场的牌。
         * @param is_self 该玩家是否为濒死者本人：救自己时酒（self_rescue）也可用。
         */
        inline bool has_rescue(
            const GameContext &ctx, const std::string &player, bool is_self)
        {
            return any_hand_card_matching(
                ctx, player,
                [is_self](const card::CardDef &def)
                { return can_rescue_def(def, is_self); });
        }

        /** @brief 消耗玩家指定的救场牌（按救者身份校验）；失败返回 false。 */
        inline bool consume_rescue(
            GameContext &ctx, const std::string &player,
            const std::string &instance_id, bool is_self)
        {
            return consume_hand_card_matching(
                       ctx, player, instance_id,
                       [is_self](const card::CardDef &def)
                       { return can_rescue_def(def, is_self); },
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
         * @brief 濒死救场：hp ≤ 0 时从当前回合角色起按座位序轮询打桃。
         * @note 起点取 ctx.turn_player；为空或该角色已离场时回落濒死者，
         *       以保留 execute_turn 外直接调用的语义。
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

            // 座位序：从当前回合角色开始；无回合上下文或该角色已离场时回落濒死者
            const bool has_turn = !ctx.turn_player.empty() &&
                                  ctx.entities->find(ctx.turn_player).is_some();
            const auto order =
                ctx.entities->order_from(has_turn ? ctx.turn_player : dying);

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
                    // 酒只能自救：非濒死者本人时 self_rescue 牌不进入候选
                    const bool is_self = (saver == dying);
                    if (!has_rescue(ctx, saver, is_self))
                        continue;
                    const auto chosen = ai.play_peach(ctx, saver, dying);
                    if (chosen.is_none())
                        continue;
                    if (!consume_rescue(ctx, saver, chosen.unwrap(), is_self))
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
                apply_equip_lost(ctx, player, keep);
            }
        }

        /**
         * @brief 击杀奖惩：乱斗给击杀者通用奖励，身份局按角色结算。
         * @param source 伤害来源（空串或已不在场 = 无奖惩）。
         * @param target 死亡角色。
         * @note 身份局：击杀反贼给来源摸 kill_reward 张；主公击杀忠臣弃光其手牌
         *       与装备（不含判定区）；主公/内奸/未知角色无奖励。
         *       奖励摸牌在事件日志中带「击杀奖励」标签。
         */
        inline void apply_kill_effect(
            GameContext &ctx, const std::string &source, const std::string &target)
        {
            if (source.empty() || ctx.entities->find(source).is_none())
                return;

            if (mode_of(ctx) != GameMode::Identity)
            {
                apply_draw(
                    ctx, source, rules_of(ctx).kill_reward, DrawKind::KillReward);
                return;
            }

            switch (role_of(ctx, target))
            {
            case Role::Rebel:
                apply_draw(
                    ctx, source, rules_of(ctx).kill_reward, DrawKind::KillReward);
                break;
            case Role::Loyalist:
                if (role_of(ctx, source) == Role::Lord)
                    discard_hand_and_equip(ctx, source);
                break;
            default:
                break;  // 主公（终局）与内奸：无奖励
            }
        }

        /** @brief 是否为连环传导会触发的属性伤害（火/雷；普通不触发）。 */
        inline bool is_elemental_damage(card::DamageType type)
        {
            return type == card::DamageType::Fire ||
                   type == card::DamageType::Thunder;
        }

        /**
         * @brief 连环传导：对快照中的其余横置者逐个以间接伤害结算。
         * @param chain_targets 原伤害结算前按座位序快照的横置者 id。
         * @note 每个受传导者先重置再结算；传导伤害为间接伤害，不再触发下一轮
         *       传导（官方「经由连环传导的伤害不能再次被传导」）。前序结算中
         *       死亡/已重置者跳过，保证每个受传导者恰受一次。
         */
        inline void propagate_chain_damage(
            GameContext &ctx, DecisionSource &ai, const std::string &source,
            int amount, card::DamageType type,
            const std::vector<std::string> &chain_targets);

        /**
         * @brief 造成伤害（流程入口）：扣血 → 濒死判定 → 死亡与击杀奖惩。
         * @param source 伤害来源（空串 = 无来源如闪电；为存活玩家时按模式与角色发奖惩）。
         * @param type 伤害属性（默认普通；火焰/雷电透传到受伤事件）。
         * @param ignore_armor 本次伤害是否无视防具（青釭剑结算窗）：为真时白银狮子
         *        的伤害上限不生效。
         * @param indirect 是否间接伤害（连环传导）：为真时本伤害不再触发新的传导。
         * @note 白银狮子上限在全部加成（酒/藤甲/古锭刀）累加之后施加，对每一次
         *       伤害实例独立生效。
         * @note 连环：非间接的属性伤害命中横置目标时，先快照其余横置者（座位序、
         *       从目标下家环绕）并重置目标，待原伤害完整结算（含濒死/死亡）后按
         *       快照依次传导同来源、同属性、同实际伤害值；传导不递归。
         */
        inline void deal_damage(
            GameContext &ctx, DecisionSource &ai, const std::string &source,
            const std::string &target, int amount,
            card::DamageType type = card::DamageType::Normal,
            bool ignore_armor = false, bool indirect = false)
        {
            const auto e = ctx.entities->find(target);
            if (e.is_none())
                return;

            // 连环起点：非间接的正属性伤害命中横置目标。先快照其余横置者
            // （原伤害结算前，死亡/移除不影响名单），再重置目标本身
            const bool chain_origin =
                !indirect && amount > 0 && is_elemental_damage(type) &&
                e.unwrap()->get_chained();
            std::vector<std::string> chain_targets;
            if (chain_origin)
            {
                for (const auto &id :
                     ctx.entities->order_from(ctx.entities->next(target)))
                    if (id != target && is_chained(ctx, id))
                        chain_targets.push_back(id);
                e.unwrap()->set_chained(false);
            }

            // 白银狮子：单次伤害至多 1 点；青釭剑结算窗内无视防具则不封顶
            if (!ignore_armor && amount > 1 &&
                has_ability(ctx, target, card::Ability::SilverLion))
                amount = 1;

            const int applied =
                e.unwrap()->take_damage(source, amount, indirect, type);

            // 伤害落定后的武将触发技：早于濒死判定（致死不豁免）
            run_after_damage_skills(ctx, ai, target, source, applied);

            if (e.unwrap()->get_hp() > 0)
            {
                if (chain_origin)
                    propagate_chain_damage(
                        ctx, ai, source, applied, type, chain_targets);
                return;
            }

            const bool died = resolve_dying(ctx, ai, target);
            if (died && !source.empty())
                apply_kill_effect(ctx, source, target);

            if (chain_origin)
                propagate_chain_damage(ctx, ai, source, applied, type, chain_targets);
        }

        inline void propagate_chain_damage(
            GameContext &ctx, DecisionSource &ai, const std::string &source,
            int amount, card::DamageType type,
            const std::vector<std::string> &chain_targets)
        {
            for (const auto &t : chain_targets)
            {
                const auto e = ctx.entities->find(t);
                if (e.is_none() || !e.unwrap()->get_chained())
                    continue;
                e.unwrap()->set_chained(false);
                deal_damage(ctx, ai, source, t, amount, type, false, true);
            }
        }
    }
}

#endif  // INCLUDE_TKW_GAME_COMBAT_HPP