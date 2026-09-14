/**
 * @file   weapon.cpp
 * @brief  「杀」结算管线与装备钩子的函数体定义。
 * @details 实现 `weapon.hpp` 声明的钩子实现、钩子表、共用闪响应入口与「杀」管线
 *          主流程；新增武器只需在钩子表加一行。
 * @ingroup tkw_game_resolve
 */

#include "game/resolve/weapon.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace tkw
{
    namespace game
    {
        Option<card::Card> find_sha_in_hand(
            const GameContext &ctx, const std::string &player)
        {
            return StateQuery::find_hand_card_matching(
                ctx, player, [](const card::CardDef &def)
                { return is_response_def(def, card::ResponseKind::Sha); });
        }

        int discard_target_cards(
            GameContext &ctx, DecisionSource &ai,
            const std::string &attacker, const std::string &target, int count)
        {
            int discarded = 0;
            for (int i = 0; i < count; ++i)
            {
                const auto picked = ai.pick_card_from_target(
                    ctx, attacker, target, PickCardScope::HandEquip);
                if (picked.is_none())
                    break;
                const auto chosen = StateOps(ctx).resolve_target_pick(target, picked.unwrap());
                if (chosen.is_none())
                    break;
                if (StateOps(ctx).remove_any_and_discard(target, chosen.unwrap().instance_id)
                        .is_some())
                    ++discarded;
            }
            return discarded;
        }

        bool is_horse_def(const card::CardDef &def)
        {
            if (def.equip.is_none())
                return false;
            const auto slot = def.equip.unwrap().slot;
            return slot == card::EquipSlot::OffensiveHorse ||
                   slot == card::EquipSlot::DefensiveHorse;
        }

        bool target_has_horse(const GameContext &ctx, const std::string &target)
        {
            for (const auto &c : ctx.cards->equip(target))
            {
                const auto def = ctx.catalog->find(c.def_id);
                if (def.is_some() && is_horse_def(*def.unwrap()))
                    return true;
            }
            return false;
        }

        std::vector<card::Card> target_horses(
            const GameContext &ctx, const std::string &target)
        {
            std::vector<card::Card> out;
            for (const auto &c : ctx.cards->equip(target))
            {
                const auto def = ctx.catalog->find(c.def_id);
                if (def.is_some() && is_horse_def(*def.unwrap()))
                    out.push_back(c);
            }
            return out;
        }

        void hook_cixiong(ShaContext &sc)
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

            // 使用者可选：拒绝则不弃不摸
            if (!sc.ai.trigger_effect(sc.ctx, sc.attacker, card::Ability::Cixiong))
                return;

            // 目标有手牌：弃一张，或（空/无效选择）令使用者摸一张
            if (sc.ctx.cards->hand_size(sc.target) > 0)
            {
                const auto discards = sc.ai.choose_discards(
                    sc.ctx, sc.target, 1, DiscardReason::CixiongChoice);
                for (const auto &id : discards)
                {
                    if (StateOps(sc.ctx).remove_and_discard(sc.target, id).is_some())
                        return;
                    break;
                }
            }

            // 无手牌或目标选择放弃弃牌：使用者摸一张牌
            StateOps(sc.ctx).apply_draw(sc.attacker, 1);
        }

        void hook_renwang(ShaContext &sc)
        {
            if (!sc.ignore_armor && !sc.virtual_sha && StateQuery::is_black_suit(sc.sha.suit))
                sc.blocked = true;
        }

        void hook_tengjia(ShaContext &sc)
        {
            // 青釭剑穿透：藤甲不生效，普通杀照常命中且火焰不加伤
            if (sc.ignore_armor)
                return;

            // 普通杀无效（含丈八两张当杀的虚拟杀）
            if (sc.damage_type == card::DamageType::Normal)
            {
                sc.blocked = true;
                return;
            }

            // 火焰伤害 +1
            if (sc.damage_type == card::DamageType::Fire)
                sc.damage_bonus += 1;
        }

        bool trigger_bagua_jink(
            GameContext &ctx, DecisionSource &ai, const std::string &target)
        {
            if (!ai.trigger_effect(ctx, target, card::Ability::JudgementJink))
                return false;
            const card::CardDef *armor =
                EquipQuery::find_equipment(ctx, target, card::Ability::JudgementJink);
            if (!armor || armor->judge.is_none())
                return false;
            auto judge = StateOps(ctx).perform_judgement();
            if (judge.is_none())
                return false;
            const card::Card judge_card = std::move(judge).unwrap();
            StateOps(ctx).discard_and_emit(target, judge_card, DiscardKind::Judgement);
            return StateQuery::judge_result(armor->judge.unwrap(), judge_card) ==
                   card::JudgeAction::Jink;
        }

        void hook_bagua(ShaContext &sc)
        {
            if (sc.ignore_armor || sc.responded)
                return;
            if (trigger_bagua_jink(sc.ctx, sc.ai, sc.target))
                sc.responded = true;
        }

        void hook_qinglong(ShaContext &sc)
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
                // 打出的牌只发打出事件；进弃牌堆是打出的必然后果，不另发弃置事件
                emit_card_played(sc.ctx, sc.attacker, extra_card);
            }
            ShaResolver(sc.ctx, sc.ai).resolve_sha(
                ShaRequest::Builder{}
                    .attacker(sc.attacker)
                    .sha(extra.unwrap())
                    .target(sc.target)
                    .damage_val(sc.amount)
                    .damage_type(sc.damage_type)
                    .build());
        }

        void hook_guanshi(ShaContext &sc)
        {
            // 发动前置：手牌不足两张付不起代价，直接不发动
            if (sc.ctx.cards->hand_size(sc.attacker) <
                static_cast<std::size_t>(rules_of(sc.ctx).two_card_cost))
                return;

            if (!sc.ai.trigger_effect(
                    sc.ctx, sc.attacker, card::Ability::DiscardTwoForceDamage))
                return;

            const auto discards = sc.ai.choose_discards(
                sc.ctx, sc.attacker, rules_of(sc.ctx).two_card_cost,
                DiscardReason::AbilityCost);

            // 只计数实际弃成功的牌，封顶 two_card_cost 张
            int discarded = 0;
            for (const auto &id : discards)
            {
                if (discarded == rules_of(sc.ctx).two_card_cost)
                    break;
                if (StateOps(sc.ctx).remove_and_discard(sc.attacker, id).is_some())
                    ++discarded;
            }

            // 弃满两张才强制命中，否则杀仍视为被闪
            if (discarded == rules_of(sc.ctx).two_card_cost)
                sc.responded = false;
        }

        void hook_hanbing(ShaContext &sc)
        {
            // 发动前置：可选区（手牌+装备）不足两张付不起代价，直接不发动
            if (sc.ctx.cards->hand_size(sc.target) +
                    sc.ctx.cards->equip_size(sc.target) <
                static_cast<std::size_t>(rules_of(sc.ctx).two_card_cost))
                return;

            if (!sc.ai.trigger_effect(
                    sc.ctx, sc.attacker, card::Ability::DamageAsDiscard))
                return;

            const int discarded = discard_target_cards(
                sc.ctx, sc.ai, sc.attacker, sc.target,
                rules_of(sc.ctx).two_card_cost);

            // 弃满两张才免伤，否则伤害照常落地
            if (discarded == rules_of(sc.ctx).two_card_cost)
                sc.prevented = true;
        }

        void hook_guding(ShaContext &sc)
        {
            if (sc.ctx.cards->hand_size(sc.target) == 0)
                sc.damage_bonus += 1;
        }

        void hook_qilin(ShaContext &sc)
        {
            if (!target_has_horse(sc.ctx, sc.target))
                return;

            if (!sc.ai.trigger_effect(
                    sc.ctx, sc.attacker, card::Ability::DiscardHorseOnDamage))
                return;

            // 候选为目标的全部坐骑（装备区顺序），攻击方从中选一张
            const auto horses = target_horses(sc.ctx, sc.target);
            if (horses.empty())
                return;
            std::string chosen = horses.front().instance_id;
            const auto picked = sc.ai.pick_from_revealed(
                sc.ctx, sc.attacker, horses, RevealSource::Qilin);
            if (picked.is_some())
                for (const auto &h : horses)
                    if (h.instance_id == picked.unwrap().instance_id)
                    {
                        chosen = h.instance_id;
                        break;
                    }

            auto removed = sc.ctx.cards->remove_from_equip(sc.target, chosen);
            if (removed.is_some())
            {
                card::Card card = std::move(removed).unwrap();
                StateOps(sc.ctx).discard_and_emit(sc.target, card);
            }
        }

        const std::vector<ShaHook> &sha_hook_table()
        {
            static const std::vector<ShaHook> table = {
                {card::Ability::Cixiong, ShaPhase::OnTarget, true, hook_cixiong},
                {card::Ability::BlackShaImmune, ShaPhase::Armor, false, hook_renwang},
                {card::Ability::VineArmor, ShaPhase::Armor, false, hook_tengjia},
                {card::Ability::JudgementJink, ShaPhase::Respond, false, hook_bagua},
                {card::Ability::ExtraShaAfterJink, ShaPhase::PostJink, true,
                 hook_qinglong},
                {card::Ability::DiscardTwoForceDamage, ShaPhase::PostJink, true,
                 hook_guanshi},
                {card::Ability::DamageAsDiscard, ShaPhase::PreDamage, true, hook_hanbing},
                {card::Ability::GudingBlade, ShaPhase::PreDamage, true, hook_guding},
                {card::Ability::DiscardHorseOnDamage, ShaPhase::OnHit, true, hook_qilin},
            };
            return table;
        }

        void run_sha_phase(ShaContext &sc, ShaPhase phase)
        {
            for (const auto &h : sha_hook_table())
            {
                if (h.phase != phase)
                    continue;
                const std::string &owner = h.attacker_side ? sc.attacker : sc.target;
                if (EquipQuery::has_ability(sc.ctx, owner, h.ability))
                    h.fn(sc);
            }
        }

        bool request_jink(
            GameContext &ctx, DecisionSource &ai, const std::string &target,
            const ResponsePrompt &prompt)
        {
            // 有对应防具才询问发动，避免对未装备者多开触发窗口
            if (EquipQuery::has_ability(ctx, target, card::Ability::JudgementJink) &&
                trigger_bagua_jink(ctx, ai, target))
                return true;

            // 无装备生效：开真闪响应窗口（打出真闪）
            return request_response(
                ctx, ai, target, card::ResponseKind::Jink, prompt);
        }

        void ShaResolver::resolve_sha(const ShaRequest &request)
        {
            ShaContext sc{m_ctx, m_ai, request.sha, request.attacker,
                          request.target, request.damage_val};
            sc.ignore_armor =
                EquipQuery::has_ability(m_ctx, request.attacker, card::Ability::IgnoreArmor);
            sc.target_count = request.target_count;
            sc.virtual_sha = request.virtual_sha;
            sc.damage_type = request.damage_type;
            // 加成初值先落位，钩子（藤甲火焰脆弱等）在 Armor 阶段累加
            sc.damage_bonus = request.damage_bonus;

            // 朱雀羽扇：普通杀使用时可转为火焰伤害。非锁定技、可放弃、无每回合
            // 限制；火杀/雷杀属性非普通，不询问（只能转化普通杀）
            if (sc.damage_type == card::DamageType::Normal &&
                EquipQuery::has_ability(m_ctx, request.attacker, card::Ability::FireShaConvert) &&
                m_ai.trigger_effect(m_ctx, request.attacker,
                                    card::Ability::FireShaConvert))
                sc.damage_type = card::DamageType::Fire;

            run_sha_phase(sc, ShaPhase::OnTarget);

            run_sha_phase(sc, ShaPhase::Armor);
            if (sc.blocked)
                return;

            run_sha_phase(sc, ShaPhase::Respond);
            if (!sc.responded)
                sc.responded = request_response(
                    m_ctx, m_ai, request.target, card::ResponseKind::Jink,
                    {sc.sha.def_id, sc.attacker, sc.amount});

            if (sc.responded)
                run_sha_phase(sc, ShaPhase::PostJink);

            if (!sc.responded)
            {
                run_sha_phase(sc, ShaPhase::PreDamage);
                if (sc.prevented)
                    return;
                CombatResolver(m_ctx, m_ai).deal_damage(
                    request.target,
                    DamageSpec::Builder{}
                        .source(request.attacker)
                        .damage_val(request.damage_val + sc.damage_bonus)
                        .damage_type(sc.damage_type)
                        .ignore_armor(sc.ignore_armor)
                        .build());
                run_sha_phase(sc, ShaPhase::OnHit);
            }
        }
    }
}
