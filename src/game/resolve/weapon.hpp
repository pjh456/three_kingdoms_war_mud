/**
 * @file weapon.hpp
 * @brief 「杀」结算管线：指定目标后 → 防具 → 响应 → 被闪后 → 命中前 → 伤害 → 命中后。
 * @note 7 件装备的效果不再写成 if 链，而是注册到 sha_hook_table()：
 *       每项声明「能力 / 插桩点 / 挂在哪一方 / 回调」，新增武器只加一行。
 * @note 规则约定（简单版）：
 *       - 雌雄双股剑：杀指定唯一目标且目标为异性时，令目标弃置一张
 *         手牌，目标弃不起时使用者摸一张牌；
 *       - 仁王盾：黑杀无效（青釭剑无视防具可穿透）；
 *       - 八卦阵：需出闪时可判定，判定描述来自装备数据（当前为红色=闪）；
 *       - 青龙偃月刀：被闪后可再对同一目标使用一张杀；
 *       - 贯石斧：被闪后可弃两张牌令杀依然命中；
 *       - 寒冰剑：命中前可防止伤害改为弃置目标两张牌；
 *       - 麒麟弓：造成伤害后可弃置目标一匹坐骑；
 *       - 方天画戟：杀为最后一张手牌时可额外指定至多两名目标
 *         （作用于目标集合，经目标数校验放宽实现，不走本表钩子）；
 *       - 丈八蛇矛：两张手牌当一张「杀」（虚拟杀无花色，仁王盾黑杀
 *         判定不适用；消费与结算入口在 resolver 层，不走本表钩子）。
 */

#ifndef INCLUDE_TKW_GAME_WEAPON_HPP
#define INCLUDE_TKW_GAME_WEAPON_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "card/catalog.hpp"
#include "card/def.hpp"
#include "card/manager.hpp"
#include "game/resolve/combat.hpp"
#include "game/core/context.hpp"
#include "game/core/decision.hpp"
#include "game/query/equip.hpp"
#include "game/resolve/response.hpp"
#include "game/core/state.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 「杀」结算上下文：钩子读写的共享状态。 */
        struct ShaContext
        {
            GameContext &ctx;
            DecisionSource &ai;
            const card::Card &sha;
            std::string attacker;
            std::string target;
            int amount = 1;
            bool ignore_armor = false; /**< 攻击方无视防具（青釭剑） */
            bool responded = false;    /**< 目标已打出/视为闪 */
            bool blocked = false;      /**< 防具直接无效（仁王盾） */
            bool prevented = false;    /**< 伤害被替代（寒冰剑） */
            int target_count = 1;      /**< 该杀指定的目标数（雌雄仅唯一目标） */
            bool virtual_sha = false; /**< 虚拟杀（丈八两张当杀）：无花色，黑杀判定不适用 */
        };

        inline void resolve_sha(
            GameContext &ctx, DecisionSource &ai, const std::string &attacker,
            const card::Card &sha, const std::string &target, int amount,
            int target_count = 1, bool virtual_sha = false);

        // ── 装备效果（钩子实现）────────────────────────────────────────

        /** @brief 攻击者手牌中的第一张杀（青龙偃月刀续杀用）。 */
        inline Option<card::Card> find_sha_in_hand(
            const GameContext &ctx, const std::string &player)
        {
            for (const auto &c : ctx.cards->hand(player))
            {
                const auto def = ctx.catalog->find(c.def_id);
                if (def.is_some() && is_response_def(*def.unwrap(), card::ResponseKind::Sha))
                    return Option<card::Card>::Some(c);
            }
            return Option<card::Card>::None();
        }

        /**
         * @brief 弃置目标 count 张牌（寒冰剑）。
         * @return 实际弃成功的张数（决策源返回幽灵 id 或选不满时少于请求数）。
         */
        inline int discard_target_cards(
            GameContext &ctx, DecisionSource &ai,
            const std::string &attacker, const std::string &target, int count)
        {
            int discarded = 0;
            for (int i = 0; i < count; ++i)
            {
                const auto picked = ai.pick_card_from_target(ctx, attacker, target);
                if (picked.is_none())
                    break;
                card::Card removed;
                if (remove_card_from_zones(
                        ctx, target, picked.unwrap().instance_id, removed))
                {
                    ++discarded;
                    ctx.cards->discard(removed);
                    emit_card_discarded(ctx, target, removed);
                }
            }
            return discarded;
        }

        /** @brief 弃置目标装备区的一匹坐骑（麒麟弓）。 */
        inline bool discard_first_horse(GameContext &ctx, const std::string &target)
        {
            for (const auto &c : ctx.cards->equip(target))
            {
                const auto def = ctx.catalog->find(c.def_id);
                if (def.is_some() && def.unwrap()->equip.is_some() &&
                    (def.unwrap()->equip.unwrap().slot ==
                         card::EquipSlot::OffensiveHorse ||
                     def.unwrap()->equip.unwrap().slot ==
                         card::EquipSlot::DefensiveHorse))
                {
                    auto removed = ctx.cards->remove_from_equip(target, c.instance_id);
                    if (removed.is_some())
                    {
                        card::Card card = std::move(removed).unwrap();
                        ctx.cards->discard(card);
                        emit_card_discarded(ctx, target, card);
                    }
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 雌雄双股剑：杀指定唯一目标后，目标为异性时令其弃置一张
         *        手牌；目标弃不起（无手牌）时使用者摸一张牌。
         * @note 强制触发（卡面无「可以」）：无决策接缝，引擎自动结算；
         *       目标弃哪张牌走既有 choose_discards 路径，选不中不产生
         *       效果（与贯石斧/寒冰剑对非法选择的处理一致）。
         */
        inline void hook_cixiong(ShaContext &sc)
        {
            // 仅唯一目标触发（方天多目标杀不触发）
            if (sc.target_count != 1)
                return;
            const auto attacker = sc.ctx.entities->find(sc.attacker);
            const auto target = sc.ctx.entities->find(sc.target);
            if (attacker.is_none() || target.is_none())
                return;
            // 同性不触发
            if (attacker.unwrap()->get_gender() == target.unwrap()->get_gender())
                return;

            // 目标有手牌：令其弃置一张
            if (sc.ctx.cards->hand_size(sc.target) > 0)
            {
                const auto discards = sc.ai.choose_discards(
                    sc.ctx, sc.target, 1, DiscardReason::AbilityCost);
                for (const auto &id : discards)
                {
                    auto removed = sc.ctx.cards->remove_from_hand(sc.target, id);
                    if (removed.is_some())
                    {
                        card::Card card = std::move(removed).unwrap();
                        sc.ctx.cards->discard(card);
                        emit_card_discarded(sc.ctx, sc.target, card);
                    }
                    break;
                }
                return;
            }

            // 弃不起：使用者摸一张牌
            apply_draw(sc.ctx, sc.attacker, 1);
        }

        /** @brief 仁王盾：黑色的杀对你无效（青釭剑可穿透，虚拟杀无花色不适用）。 */
        inline void hook_renwang(ShaContext &sc)
        {
            if (!sc.ignore_armor && !sc.virtual_sha && is_black_suit(sc.sha.suit))
                sc.blocked = true;
        }

        /** @brief 八卦阵：需出闪时判定，判定描述来自装备数据。 */
        inline void hook_bagua(ShaContext &sc)
        {
            if (sc.ignore_armor || sc.responded)
                return;
            const card::CardDef *armor = find_equipment(
                sc.ctx, sc.target, card::Ability::JudgementJink);
            if (!armor || armor->judge.is_none())
                return;
            auto judge = perform_judgement(sc.ctx);
            if (judge.is_none())
                return;
            const card::Card judge_card = std::move(judge).unwrap();
            sc.ctx.cards->discard(judge_card);
            emit_card_discarded(sc.ctx, sc.target, judge_card);
            if (judge_result(armor->judge.unwrap(), judge_card) ==
                card::JudgeAction::Jink)
                sc.responded = true;
        }

        /** @brief 青龙偃月刀：目标打出闪后可再对同一目标使用一张杀。 */
        inline void hook_qinglong(ShaContext &sc)
        {
            if (!sc.ai.trigger_effect(
                    sc.ctx, sc.attacker, card::Ability::ExtraShaAfterJink))
                return;
            auto extra = find_sha_in_hand(sc.ctx, sc.attacker);
            if (extra.is_none())
                return;
            auto removed =
                sc.ctx.cards->remove_from_hand(sc.attacker, extra.unwrap().instance_id);
            if (removed.is_some())
            {
                card::Card extra_card = std::move(removed).unwrap();
                sc.ctx.cards->discard(extra_card);
                emit_card_played(sc.ctx, sc.attacker, extra_card);
                emit_card_discarded(sc.ctx, sc.attacker, extra_card);
            }
            resolve_sha(
                sc.ctx, sc.ai, sc.attacker, extra.unwrap(), sc.target, sc.amount);
        }

        /**
         * @brief 贯石斧：目标打出闪后可弃两张牌令杀依然命中。
         * @note 弃满两张才能发动：攻击方手牌不足 2 张不发动（不询问、不弃牌、
         *       不强制命中）；实际弃不满 2 张（含幽灵引用）不强制命中。
         */
        inline void hook_guanshi(ShaContext &sc)
        {
            // 发动前置：手牌不足 2 张付不起代价，直接不发动
            if (sc.ctx.cards->hand_size(sc.attacker) < 2)
                return;

            if (!sc.ai.trigger_effect(
                    sc.ctx, sc.attacker, card::Ability::DiscardTwoForceDamage))
                return;

            const auto discards = sc.ai.choose_discards(
                sc.ctx, sc.attacker, 2, DiscardReason::AbilityCost);

            // 只计数实际弃成功的牌，封顶 2 张
            int discarded = 0;
            for (const auto &id : discards)
            {
                if (discarded == 2)
                    break;
                auto removed = sc.ctx.cards->remove_from_hand(sc.attacker, id);
                if (removed.is_some())
                {
                    ++discarded;
                    card::Card card = std::move(removed).unwrap();
                    sc.ctx.cards->discard(card);
                    emit_card_discarded(sc.ctx, sc.attacker, card);
                }
            }

            // 弃满两张才强制命中，否则杀仍视为被闪
            if (discarded == 2)
                sc.responded = false;
        }

        /**
         * @brief 寒冰剑：防止伤害改为弃置目标两张牌。
         * @note 目标弃满两张才免伤：目标可选区（手牌+装备+判定）不足 2 张
         *       不发动（不询问、不弃牌、不免伤）；实际弃不满 2 张（含幽灵
         *       引用）不免伤。
         */
        inline void hook_hanbing(ShaContext &sc)
        {
            // 发动前置：目标可选区不足 2 张付不起代价，直接不发动
            if (sc.ctx.cards->hand_size(sc.target) +
                    sc.ctx.cards->equip_size(sc.target) +
                    sc.ctx.cards->judge_size(sc.target) < 2)
                return;

            if (!sc.ai.trigger_effect(
                    sc.ctx, sc.attacker, card::Ability::DamageAsDiscard))
                return;

            const int discarded =
                discard_target_cards(sc.ctx, sc.ai, sc.attacker, sc.target, 2);

            // 弃满两张才免伤，否则伤害照常落地
            if (discarded == 2)
                sc.prevented = true;
        }

        /** @brief 麒麟弓：造成伤害后可弃置目标一匹坐骑。 */
        inline void hook_qilin(ShaContext &sc)
        {
            if (sc.ai.trigger_effect(
                    sc.ctx, sc.attacker, card::Ability::DiscardHorseOnDamage))
                discard_first_horse(sc.ctx, sc.target);
        }

        // ── 管线 ────────────────────────────────────────────────────────

        /** @brief 插桩点（顺序即规则顺序）。 */
        enum class ShaPhase : std::uint8_t
        {
            OnTarget,  /**< 指定目标之后（防具之前） */
            Armor,     /**< 防具拦截 */
            Respond,   /**< 响应窗口（判定/出闪） */
            PostJink,  /**< 被闪之后 */
            PreDamage, /**< 命中之前 */
            OnHit,     /**< 造成伤害之后 */
        };

        /** @brief 钩子注册项：能力 / 插桩点 / 挂在哪一方 / 回调。 */
        struct ShaHook
        {
            card::Ability ability;
            ShaPhase phase;
            bool attacker_side;
            void (*fn)(ShaContext &);
        };

        /** @brief 全部装备钩子（新增武器 = 加一行）。 */
        inline const std::vector<ShaHook> &sha_hook_table()
        {
            static const std::vector<ShaHook> table = {
                {card::Ability::Cixiong, ShaPhase::OnTarget, true, hook_cixiong},
                {card::Ability::BlackShaImmune, ShaPhase::Armor, false, hook_renwang},
                {card::Ability::JudgementJink, ShaPhase::Respond, false, hook_bagua},
                {card::Ability::ExtraShaAfterJink, ShaPhase::PostJink, true,
                 hook_qinglong},
                {card::Ability::DiscardTwoForceDamage, ShaPhase::PostJink, true,
                 hook_guanshi},
                {card::Ability::DamageAsDiscard, ShaPhase::PreDamage, true, hook_hanbing},
                {card::Ability::DiscardHorseOnDamage, ShaPhase::OnHit, true, hook_qilin},
            };
            return table;
        }

        /** @brief 执行某插桩点上所有已装备能力的钩子。 */
        inline void run_sha_phase(ShaContext &sc, ShaPhase phase)
        {
            for (const auto &h : sha_hook_table())
            {
                if (h.phase != phase)
                    continue;
                const std::string &owner = h.attacker_side ? sc.attacker : sc.target;
                if (has_ability(sc.ctx, owner, h.ability))
                    h.fn(sc);
            }
        }

        /**
         * @brief 「杀」结算主流程：attacker 对 target 使用杀。
         * @param sha 该杀的卡牌对象（花色用于仁王盾黑杀判定）。
         * @param amount 伤害量（config 驱动，当前数据均为 1）。
         * @param target_count 该杀指定的目标总数（缺省 1；多目标杀逐目标
         *        结算时由调用方传入，供仅唯一目标触发的能力判定）。
         * @param virtual_sha 是否虚拟杀（丈八两张当杀）：真无花色，仁王盾
         *        黑杀判定短路；此时 sha 参数可为占位对象。
         */
        inline void resolve_sha(
            GameContext &ctx, DecisionSource &ai, const std::string &attacker,
            const card::Card &sha, const std::string &target, int amount,
            int target_count, bool virtual_sha)
        {
            ShaContext sc{ctx, ai, sha, attacker, target, amount};
            sc.ignore_armor = has_ability(ctx, attacker, card::Ability::IgnoreArmor);
            sc.target_count = target_count;
            sc.virtual_sha = virtual_sha;

            run_sha_phase(sc, ShaPhase::OnTarget);

            run_sha_phase(sc, ShaPhase::Armor);
            if (sc.blocked)
                return;

            run_sha_phase(sc, ShaPhase::Respond);
            if (!sc.responded)
                sc.responded =
                    request_response(ctx, ai, target, card::ResponseKind::Jink);

            if (sc.responded)
                run_sha_phase(sc, ShaPhase::PostJink);

            if (!sc.responded)
            {
                run_sha_phase(sc, ShaPhase::PreDamage);
                if (sc.prevented)
                    return;
                deal_damage(ctx, ai, attacker, target, amount);
                run_sha_phase(sc, ShaPhase::OnHit);
            }
        }
    }
}

#endif  // INCLUDE_TKW_GAME_WEAPON_HPP
