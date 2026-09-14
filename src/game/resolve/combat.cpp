/**
 * @file   combat.cpp
 * @brief  战斗流程的函数体定义。
 * @details 实现 `combat.hpp` 声明的伤害落定、濒死救场、死亡清场、击杀奖惩与连环
 *          传导。
 * @ingroup tkw_game_resolve
 */

#include "game/resolve/combat.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tkw
{
    namespace game
    {
        bool has_rescue(
            const GameContext &ctx, const std::string &player, bool is_self)
        {
            return StateQuery::any_hand_card_matching(
                ctx, player,
                [is_self](const card::CardDef &def)
                { return can_rescue_def(def, is_self); });
        }

        bool consume_rescue(
            GameContext &ctx, const std::string &player,
            const std::string &instance_id, bool is_self)
        {
            return StateOps(ctx).consume_hand_card_matching(player, instance_id,
                       [is_self](const card::CardDef &def, const card::Card &)
                       { return can_rescue_def(def, is_self); },
                       DiscardKind::Response)
                .is_some();
        }

        void declare_death(GameContext &ctx, const std::string &player)
        {
            for (const auto &c : ctx.cards->discard_all(player))
                emit_card_discarded(ctx, player, c);
            auto ev = std::make_shared<EntityDiedEvent>();
            ev->entity_id = player;
            ctx.bus->publish(ev);
            ctx.entities->remove(player);
        }

        bool resolve_dying(
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
                    StateOps(ctx).apply_heal(dying, rules_of(ctx).rescue_heal);
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

        void discard_hand_and_equip(GameContext &ctx, const std::string &player)
        {
            // 按值拷贝：移除会改动区域，遍历期间不能持有区域视图引用
            const auto hands = ctx.cards->hand(player);
            for (const auto &c : hands)
            {
                auto removed = ctx.cards->remove_from_hand(player, c.instance_id);
                if (removed.is_none())
                    continue;
                auto keep = std::move(removed).unwrap();
                StateOps(ctx).discard_and_emit(player, keep);
            }

            const auto equips = ctx.cards->equip(player);
            for (const auto &c : equips)
            {
                auto removed = ctx.cards->remove_from_equip(player, c.instance_id);
                if (removed.is_none())
                    continue;
                auto keep = std::move(removed).unwrap();
                StateOps(ctx).discard_and_emit(player, keep);
                StateOps(ctx).apply_equip_lost(player, keep);
            }
        }

        void apply_kill_effect(
            GameContext &ctx, const std::string &source, const std::string &target)
        {
            if (source.empty() || ctx.entities->find(source).is_none())
                return;

            if (mode_of(ctx) != GameMode::Identity)
            {
                StateOps(ctx).apply_draw(source, rules_of(ctx).kill_reward, DrawKind::KillReward);
                return;
            }

            switch (role_of(ctx, target))
            {
            case Role::Rebel:
                StateOps(ctx).apply_draw(source, rules_of(ctx).kill_reward, DrawKind::KillReward);
                break;
            case Role::Loyalist:
                if (role_of(ctx, source) == Role::Lord)
                    discard_hand_and_equip(ctx, source);
                break;
            default:
                break;  // 主公（终局）与内奸：无奖励
            }
        }

        void CombatResolver::deal_damage(
            const std::string &target, const DamageSpec &spec)
        {
            const auto e = m_ctx.entities->find(target);
            if (e.is_none())
                return;

            // 连环起点：非间接的正属性伤害命中横置目标。先快照其余横置者
            // （原伤害结算前，死亡/移除不影响名单），再重置目标本身
            const bool chain_origin =
                !spec.indirect && spec.damage_val > 0 &&
                is_elemental_damage(spec.damage_type) && e.unwrap()->get_chained();
            std::vector<std::string> chain_targets;
            if (chain_origin)
            {
                for (const auto &id :
                     m_ctx.entities->order_from(m_ctx.entities->next(target)))
                    if (id != target && StateQuery::is_chained(m_ctx, id))
                        chain_targets.push_back(id);
                e.unwrap()->set_chained(false);
            }

            // 白银狮子：单次伤害至多 1 点；青釭剑结算窗内无视防具则不封顶
            int amount = spec.damage_val;
            if (!spec.ignore_armor && amount > 1 &&
                EquipQuery::has_ability(m_ctx, target, card::Ability::SilverLion))
                amount = 1;

            const int applied =
                e.unwrap()->take_damage(spec.source, amount, spec.indirect, spec.damage_type);

            // 伤害落定后的武将触发技：早于濒死判定（致死不豁免）
            run_after_damage_skills(m_ctx, m_ai, target, spec.source, applied);

            if (e.unwrap()->get_hp() > 0)
            {
                if (chain_origin)
                    propagate_chain_damage(
                        m_ctx, m_ai, spec.source, applied, spec.damage_type, chain_targets);
                return;
            }

            const bool died = resolve_dying(m_ctx, m_ai, target);
            if (died && !spec.source.empty())
                apply_kill_effect(m_ctx, spec.source, target);

            if (chain_origin)
                propagate_chain_damage(
                    m_ctx, m_ai, spec.source, applied, spec.damage_type, chain_targets);
        }

        void propagate_chain_damage(
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
                CombatResolver(ctx, ai).deal_damage(
                    t, DamageSpec::Builder{}
                           .source(source)
                           .damage_val(amount)
                           .damage_type(type)
                           .indirect(true)
                           .build());
            }
        }
    }
}
