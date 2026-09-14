/**
 * @file combat.hpp
 * @brief 战斗流程：伤害 → 濒死救场（桃）→ 死亡声明与击杀奖惩。
 * @details 战斗流程的状态变化集中于此：
 *          - hp 扣到非正 → 进入濒死：从当前回合角色起按座位序轮询打桃
 *            （无回合上下文时回落濒死者起）；
 *          - 一轮无人可救/不救 → 死亡：区域牌弃置、发布 EntityDiedEvent、移除实体；
 *          - 击杀奖惩：乱斗按通用规则给击杀者发奖励，身份局按死者角色与击杀者
 *            身份结算（击杀反贼发奖励；主公击杀忠臣弃光其手牌与装备；其余无奖）。
 * @note 无武将技能：能作濒死救场牌的只有资源标记 rescue（救任意人）或
 *       self_rescue（仅濒死者本人，如酒）的牌。
 * @ingroup tkw_game_resolve
 */

#ifndef INCLUDE_TKW_GAME_COMBAT_HPP
#define INCLUDE_TKW_GAME_COMBAT_HPP

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
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
         * @brief 一次伤害结算的只读事实。
         * @details 把来源、伤害量、属性与两个结算开关归为一个值，调用方按名给出
         *          字段，不再依赖实参位置。需要按条件分步组装时用 `Builder`。
         * @note `indirect` 表示该伤害不再触发连环传导；`ignore_armor` 表示本次
         *       结算跳过防具对伤害量的修正（青釭剑结算窗）。
         */
        struct DamageSpec
        {
            std::string source;              /**< 伤害来源实体 id；空 = 无来源。 */
            int damage_val = 0;              /**< 伤害量。 */
            card::DamageType damage_type = card::DamageType::Normal; /**< 伤害属性。 */
            bool ignore_armor = false;       /**< 是否无视防具。 */
            bool indirect = false;           /**< 是否间接伤害（不再传导）。 */

            class Builder; /**< 链式构造器；定义见下。 */
        };

        /**
         * @brief `DamageSpec` 的链式构造器（可选字段按需设置）。
         * @details 每个设置方法名与所设字段同名：调用什么就是设置什么。
         */
        class DamageSpec::Builder
        {
        public:
            /**
             * @brief  设置伤害来源。
             * @param[in] source 来源实体 id；空串 = 无来源。
             * @return 本构造器，供链式调用。
             */
            Builder &source(std::string source)
            {
                m_spec.source = std::move(source);
                return *this;
            }

            /**
             * @brief  设置伤害量。
             * @param[in] damage_val 伤害量。
             * @return 本构造器，供链式调用。
             */
            Builder &damage_val(int damage_val)
            {
                m_spec.damage_val = damage_val;
                return *this;
            }

            /**
             * @brief  设置伤害属性。
             * @param[in] damage_type 伤害属性。
             * @return 本构造器，供链式调用。
             */
            Builder &damage_type(card::DamageType damage_type)
            {
                m_spec.damage_type = damage_type;
                return *this;
            }

            /**
             * @brief  设置是否无视防具。
             * @param[in] value 为 `true` 时本次伤害跳过防具修正（青釭剑结算窗）。
             * @return 本构造器，供链式调用。
             */
            Builder &ignore_armor(bool value)
            {
                m_spec.ignore_armor = value;
                return *this;
            }

            /**
             * @brief  设置是否为间接伤害。
             * @param[in] value 为 `true` 时该伤害不再触发连环传导。
             * @return 本构造器，供链式调用。
             */
            Builder &indirect(bool value)
            {
                m_spec.indirect = value;
                return *this;
            }

            /**
             * @brief  产出组装好的伤害事实。
             * @return 组装完成的值。
             */
            DamageSpec build() const { return m_spec; }

        private:
            DamageSpec m_spec; /**< 组装中的值。 */
        };

        /**
         * @brief 伤害结算操作类：以对局上下文与决策源为依赖，收敛战斗流程。
         * @details 把「扣血 → 濒死救场 → 死亡与击杀奖惩 → 连环传导」的入口收敛为
         *          成员函数，构造一次即可在整段结算中复用同一组依赖。
         * @warning 本类**不拥有** `ctx`/`ai`：二者须比本对象存活更久，且不得跨局
         *          复用。
         * @see   DamageSpec
         */
        class CombatResolver
        {
        public:
            /**
             * @brief  绑定对局上下文与决策源。
             * @param[in,out] ctx 对局上下文；本对象只持引用。
             * @param[in,out] ai  决策源；濒死救场与受伤触发技继续经它询问。
             */
            CombatResolver(GameContext &ctx, DecisionSource &ai)
                : m_ctx(ctx), m_ai(ai)
            {
            }

            /**
             * @brief  造成伤害（战斗流程入口）。
             * @param[in] target 受伤实体 id；不存在时直接返回。
             * @param[in] spec   本次伤害的只读事实（来源/量/属性/开关）。
             * @post   目标 hp 已扣减；进入濒死则已完成救场/死亡/击杀奖惩；非间接
             *         属性伤害命中横置目标时已按快照完成传导。
             * @note   白银狮子上限在全部加成累加之后施加，对每次伤害实例独立生效；
             *         连环传导不递归。
             */
            void deal_damage(const std::string &target, const DamageSpec &spec);

        private:
            GameContext &m_ctx;   /**< 对局上下文（引用，非拥有）。 */
            DecisionSource &m_ai; /**< 决策源（引用，非拥有）。 */
        };

        /**
         * @brief 玩家手牌中是否有可作濒死救场的牌。
         * @param[in] ctx     只读上下文。
         * @param[in] player  被查询的实体 id。
         * @param[in] is_self 该玩家是否为濒死者本人：救自己时酒（self_rescue）也可用。
         * @return 有可救场牌时为 true；否则 false。
         * @retval true  手牌中至少一张满足当前救者身份的救援标记。
         * @retval false 手牌为空或无符合标记的牌。
         */
        inline bool has_rescue(
            const GameContext &ctx, const std::string &player, bool is_self)
        {
            return any_hand_card_matching(
                ctx, player,
                [is_self](const card::CardDef &def)
                { return can_rescue_def(def, is_self); });
        }

        /**
         * @brief 消耗玩家指定的救场牌（按救者身份校验）。
         * @param[in] ctx         对局上下文。
         * @param[in] player      救者实体 id。
         * @param[in] instance_id 选中的手牌 instance_id。
         * @param[in] is_self     救者是否为濒死者本人（决定酒是否可用）。
         * @return 成功消费时为 true；否则 false。
         * @retval true  该牌满足救场标记，已移出手牌并按响应语义弃置。
         * @retval false 选择非法或牌不满足身份条件；状态不变。
         */
        inline bool consume_rescue(
            GameContext &ctx, const std::string &player,
            const std::string &instance_id, bool is_self)
        {
            return consume_hand_card_matching(
                       ctx, player, instance_id,
                       [is_self](const card::CardDef &def, const card::Card &)
                       { return can_rescue_def(def, is_self); },
                       DiscardKind::Response)
                .is_some();
        }

        /**
         * @brief 死亡清场：区域牌全部置入弃牌堆，移除实体并发布死亡事件。
         * @param[in] ctx    对局上下文。
         * @param[in] player 死亡实体 id。
         * @post 该实体手牌/装备/判定区全部进入弃牌堆、逐个发布弃置事件；发布
         *       `EntityDiedEvent` 并从实体表移除该 id。
         * @note 已移除或未知 id 时按空处理（不报错）。
         */
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
         * @param[in] ctx   对局上下文。
         * @param[in] ai    决策源；逐救者询问是否出桃/酒。
         * @param[in] dying 濒死实体 id。
         * @return 濒死者最终是否死亡。
         * @retval true  死亡：无人相救或轮次耗尽，`declare_death` 已执行。
         * @retval false 救回：hp > 0。
         * @post 救回时已消费相应救场牌并回血；死亡时区域牌已弃置、实体已移除。
         * @note 起点取 ctx.turn_player；为空或该角色已离场时回落濒死者，
         *       以保留 execute_turn 外直接调用的语义。
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
         * @param[in] ctx    对局上下文。
         * @param[in] player 被弃牌玩家。
         * @post 该玩家手牌与装备区清空，逐张进入弃牌堆并发弃置事件；装备区失去
         *       时联动 `apply_equip_lost`。
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
         * @param[in] ctx    对局上下文。
         * @param[in] source 伤害来源（空串或已不在场 = 无奖惩）。
         * @param[in] target 死亡角色。
         * @post 有奖惩时来源摸 `kill_reward` 张，或主公的来源被弃光手牌与装备。
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

        /**
         * @brief 是否为连环传导会触发的属性伤害（火/雷；普通不触发）。
         * @param[in] type 伤害属性。
         * @return 火或雷时为 true；普通为 false。
         * @retval true  `DamageType::Fire` 或 `DamageType::Thunder`。
         * @retval false 普通或其他属性。
         */
        inline bool is_elemental_damage(card::DamageType type)
        {
            return type == card::DamageType::Fire ||
                   type == card::DamageType::Thunder;
        }

        /**
         * @brief 连环传导：对快照中的其余横置者逐个以间接伤害结算。
         * @param[in] ctx           对局上下文。
         * @param[in] ai            决策源；传导伤害的濒死/技能流程继续用。
         * @param[in] source        伤害来源实体 id（空串 = 无来源）。
         * @param[in] amount        实际传导伤害量。
         * @param[in] type          伤害属性（火/雷）。
         * @param[in] chain_targets 原伤害结算前按座位序快照的横置者 id。
         * @post 名单中仍在场且仍横置者各受一次间接伤害并重置连环状态。
         * @note 每个受传导者先重置再结算；传导伤害为间接伤害，不再触发下一轮
         *       传导（官方「经由连环传导的伤害不能再次被传导」）。前序结算中
         *       死亡/已重置者跳过，保证每个受传导者恰受一次。
         */
        inline void propagate_chain_damage(
            GameContext &ctx, DecisionSource &ai, const std::string &source,
            int amount, card::DamageType type,
            const std::vector<std::string> &chain_targets);

        inline void CombatResolver::deal_damage(
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
                    if (id != target && is_chained(m_ctx, id))
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
                CombatResolver(ctx, ai).deal_damage(
                    t, DamageSpec::Builder{}
                           .source(source)
                           .damage_val(amount)
                           .damage_type(type)
                           .indirect(true)
                           .build());
            }
        }

        /**
         * @brief 造成伤害（便捷转发）。
         * @details 等价于 `CombatResolver(ctx, ai).deal_damage(target, spec)`；
         *          保留以兼容既有调用点与测试。
         * @param[in,out] ctx          对局上下文。
         * @param[in,out] ai           决策源。
         * @param[in]     source       伤害来源（空串 = 无来源）。
         * @param[in]     target       受伤实体 id。
         * @param[in]     amount       伤害量。
         * @param[in]     type         伤害属性。
         * @param[in]     ignore_armor 是否无视防具。
         * @param[in]     indirect     是否间接伤害。
         * @see CombatResolver::deal_damage
         */
        inline void deal_damage(
            GameContext &ctx, DecisionSource &ai, const std::string &source,
            const std::string &target, int amount,
            card::DamageType type = card::DamageType::Normal,
            bool ignore_armor = false, bool indirect = false)
        {
            CombatResolver(ctx, ai).deal_damage(
                target, DamageSpec{source, amount, type, ignore_armor, indirect});
        }
    }
}

#endif  // INCLUDE_TKW_GAME_COMBAT_HPP
