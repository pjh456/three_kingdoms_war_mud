/**
 * @file simple.hpp
 * @brief 确定性贪心策略：CLI 冒烟运行与回放测试用（无随机、无隐藏状态）。
 * @note 逻辑集中在 SimpleDecider::decide（只读 DecisionRequest）；SimpleAI 把
 *       它经 RequestDecisionSource 适配成引擎可用的 DecisionSource。只做「合法
 *       且能推进」的动作：出杀（按回合上下文给的次数上限）、可结算锦囊、装备。
 *       无懈不抵消自己的锦囊；敌人锦囊冲自己或自己判定区有延时锦囊时出第一张，
 *       其余不出；武器效果按代价可付性决定是否发动。弃牌按牌价值升序取（先弃
 *       最低价值，同价值保持手牌序）。响应窗口取第一张真响应牌，杀响应无真杀
 *       时用两张手牌当杀（丈八蛇矛）。身份局在以上基础上叠加阵营意识：无懈只挡
 *       敌方冲自己/友方、不拆友方锦囊，救桃按阵营取舍，出牌避开同阵营友方
 *       （内奸避让并救主公，直到主公成为最后一名非内奸）；乱斗与无角色时全部
 *       回落上述旧口径。
 */

#ifndef INCLUDE_TKW_GAME_SIMPLE_HPP
#define INCLUDE_TKW_GAME_SIMPLE_HPP

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "card/card.hpp"
#include "card/def.hpp"
#include "game/ai/decider.hpp"
#include "game/ai/decider_base.hpp"
#include "game/ai/legal.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        namespace ai
        {
            /** @brief 贪心决策逻辑（AI 状态机的参考实现）。 */
            class SimpleDecider : public DeciderBase
            {
            public:
                DecisionChoice decide(const DecisionRequest &req) override
                {
                    return dispatch(req, decide_play, decide_first_card);
                }

            private:
                static DecisionChoice decide_first_card(const DecisionRequest &req)
                {
                    DecisionChoice out;
                    if (!req.options.empty())
                        out.option_index = Option<std::size_t>::Some(0);
                    return out;
                }

                /**
                 * @brief 出牌：取第一张可出的牌，再按贪心偏好选目标。
                 * @note legal_actions 已按手牌序产出；按牌分组以复现「首张可出」
                 *       语义。OneOther 集火最低体力（方天画戟取目标最多者），
                 *       借刀对合法 {持武器者, 受害者} 对选集火对象体力最低者。
                 *       虚拟杀（丈八蛇矛两张当杀 / 武圣红牌当杀）按杀优先：
                 *       先于锦囊打出，集火/多目标规则同杀；手牌有真杀时不作此
                 *       优先，且转化动作不参与分组（避免烧牌与非法选择）。
                 */
                static DecisionChoice decide_play(const DecisionRequest &req)
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
            };

            /** @brief 引擎可用的贪心决策源：SimpleDecider 经适配器接入。 */
            class SimpleAI : private SimpleDecider, public RequestDecisionSource
            {
            public:
                SimpleAI() :
                    RequestDecisionSource(static_cast<SimpleDecider &>(*this))
                {
                }
            };
        }

        /** @brief SimpleAI 在 game 命名空间的别名。 */
        using ai::SimpleAI;
    }
}

#endif  // INCLUDE_TKW_GAME_SIMPLE_HPP
