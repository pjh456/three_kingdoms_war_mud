/**
 * @file   aggressive.cpp
 * @brief  攻击优先决策档出牌逻辑的定义。
 * @ingroup tkw_game_ai
 */

#include "game/ai/aggressive.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace tkw
{
    namespace game
    {
        namespace ai
        {
            DecisionChoice AggressiveDecider::decide_best_card(
                const DecisionRequest &req)
            {
                DecisionChoice out;
                if (req.options.empty())
                    return out;

                std::size_t best = 0;
                int best_value = card_value_of(req, req.options.front());
                for (std::size_t i = 1; i < req.options.size(); ++i)
                {
                    const int v = card_value_of(req, req.options[i]);
                    if (v > best_value)
                    {
                        best_value = v;
                        best = i;
                    }
                }
                out.option_index = Option<std::size_t>::Some(best);
                return out;
            }

            DecisionChoice AggressiveDecider::decide_play(const DecisionRequest &req)
            {
                // 手牌有真杀：剔除单张转化动作，交回普通分组（真杀正常打出）
                const bool real_sha = has_real_sha(req);
                std::vector<LegalAction> acts;
                acts.reserve(req.legal.size());
                for (const auto &a : req.legal)
                    if (!(real_sha && a.converted_sha))
                        acts.push_back(a);

                std::vector<std::string> order;
                std::vector<std::vector<LegalAction>> groups;
                for (const auto &a : acts)
                {
                    const auto it =
                        std::find(order.begin(), order.end(), a.card.instance_id);
                    if (it == order.end())
                    {
                        order.push_back(a.card.instance_id);
                        groups.push_back({a});
                    }
                    else
                    {
                        groups[static_cast<std::size_t>(it - order.begin())]
                            .push_back(a);
                    }
                }

                // 卡类优先级选组：跳过的组（满血自疗/缺 def）不参与比较
                const std::vector<LegalAction> *best = nullptr;
                int best_priority = 0;
                for (const auto &opts : groups)
                {
                    const int p = group_priority(req, opts);
                    if (p < 0)
                        continue;
                    if (best == nullptr || p < best_priority)
                    {
                        best_priority = p;
                        best = &opts;
                    }
                }
                if (best == nullptr)
                    return DecisionChoice{};

                return decide_play_group(
                    req, *best, find_def(req, best->front().card.def_id));
            }

            DecisionChoice AggressiveDecider::decide_play_group(
                const DecisionRequest &req, const std::vector<LegalAction> &opts,
                const card::CardDef *def)
            {
                // 虚拟杀：组内存在两张当杀或单张转化动作即按伤害接管
                std::vector<LegalAction> virtual_sha;
                for (const auto &a : opts)
                    if (!a.second_instance_id.empty() || a.converted_sha)
                        virtual_sha.push_back(a);
                if (!virtual_sha.empty())
                    return decide_zhangba(req, virtual_sha);

                return select_group_targets(req, opts, *def);
            }

            int AggressiveDecider::group_priority(
                const DecisionRequest &req,
                const std::vector<LegalAction> &opts)
            {
                for (const auto &a : opts)
                    if (!a.second_instance_id.empty() || a.converted_sha)
                        return kPlayPriorityDamage;
                const card::CardDef *def =
                    find_def(req, opts.front().card.def_id);
                if (!def)
                    return kPlayPrioritySkip;

                // 身份局避让：有害组全部候选只打友方时跳过该组；
                // 乱斗/无角色该谓词恒 false，行为不变
                if (is_harmful_def(*def) &&
                    group_avoids_all_targets(req, opts))
                    return kPlayPrioritySkip;
                return play_priority(req, *def);
            }

            int AggressiveDecider::play_priority(
                const DecisionRequest &req, const card::CardDef &def)
            {
                if (def.effect.is_none())
                    return (def.type == card::CardType::Trick &&
                            def.judge.is_some())
                        ? kPlayPriorityDelayed
                        : kPlayPriorityOther;
                const card::CardEffect &eff = def.effect.unwrap();
                switch (eff.kind)
                {
                case card::CardEffectKind::Analeptic:
                    return kPlayPriorityAnaleptic;
                case card::CardEffectKind::Damage:
                    return kPlayPriorityDamage;
                case card::CardEffectKind::AoeDamage:
                    return kPlayPriorityAoe;
                case card::CardEffectKind::Duel:
                    return kPlayPriorityDuel;
                case card::CardEffectKind::FireAttack:
                    return kPlayPriorityDuel;
                case card::CardEffectKind::DiscardTarget:
                    return kPlayPriorityDiscard;
                case card::CardEffectKind::Steal:
                    return kPlayPrioritySteal;
                case card::CardEffectKind::Draw:
                    return kPlayPriorityDraw;
                case card::CardEffectKind::BorrowedSword:
                    return kPlayPriorityBorrowedSword;
                case card::CardEffectKind::Heal:
                    if (eff.scope.unwrap_or(card::Scope::Self) ==
                            card::Scope::Self &&
                        req.view.self_hp >= req.view.self_max_hp)
                        return kPlayPrioritySkip;  // 满血不打桃
                    return kPlayPriorityHeal;
                case card::CardEffectKind::Jink:
                case card::CardEffectKind::RevealPick:
                case card::CardEffectKind::Chain:
                    break;
                }
                return kPlayPriorityOther;
            }
        }
    }
}
