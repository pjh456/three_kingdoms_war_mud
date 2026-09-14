/**
 * @file   simple.cpp
 * @brief  贪心决策档出牌逻辑的定义。
 * @ingroup tkw_game_ai
 */

#include "game/ai/simple.hpp"

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
            DecisionChoice SimpleDecider::decide_first_card(
                const DecisionRequest &req)
            {
                DecisionChoice out;
                if (!req.options.empty())
                    out.option_index = Option<std::size_t>::Some(0);
                return out;
            }

            DecisionChoice SimpleDecider::decide_play(const DecisionRequest &req)
            {
                // 手牌有真杀：剔除单张转化动作，交回普通分组（真杀正常打出）
                const bool real_sha = has_real_sha(req);
                std::vector<LegalAction> acts;
                acts.reserve(req.legal.size());
                for (const auto &a : req.legal)
                    if (!(real_sha && a.converted_sha))
                        acts.push_back(a);

                // 虚拟杀：手牌无真杀时按杀优先（丈八两张 / 武圣单张）
                std::vector<LegalAction> virtual_sha;
                for (const auto &a : acts)
                    if (!a.second_instance_id.empty() || a.converted_sha)
                        virtual_sha.push_back(a);
                if (!virtual_sha.empty())
                    return decide_zhangba(req, virtual_sha);

                DecisionChoice out;
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

                for (std::size_t i = 0; i < order.size(); ++i)
                {
                    const auto &opts = groups[i];
                    const card::Card &c = opts.front().card;
                    const card::CardDef *def = find_def(req, c.def_id);
                    if (!def)
                        continue;

                    // 身份局避让：有害组全部候选只打友方时整组跳过，
                    // 继续看下一组；乱斗/无角色该谓词恒 false，行为不变
                    if (is_harmful_def(*def) &&
                        group_avoids_all_targets(req, opts))
                        continue;

                    // 满血自疗：跳过本组，继续看下一组（档位分歧，留派生）
                    if (def->effect.is_some())
                    {
                        const card::CardEffect &eff = def->effect.unwrap();
                        if (eff.kind == card::CardEffectKind::Heal &&
                            eff.scope.unwrap_or(card::Scope::Self) ==
                                card::Scope::Self &&
                            req.view.self_hp >= req.view.self_max_hp)
                            continue;
                    }

                    return select_group_targets(req, opts, *def);
                }
                return out;
            }
        }
    }
}
