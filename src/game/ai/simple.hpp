/**
 * @file simple.hpp
 * @brief 确定性贪心策略：CLI 冒烟运行与回放测试用（无随机、无隐藏状态）。
 * @note 逻辑集中在 SimpleDecider::decide（只读 DecisionRequest）；SimpleAI 把
 *       它经 RequestDecisionSource 适配成引擎可用的 DecisionSource。只做「合法
 *       且能推进」的动作：出杀（按回合上下文给的次数上限）、可结算锦囊、装备。
 *       无懈不抵消自己的锦囊；敌人锦囊冲自己或自己判定区有延时锦囊时出第一张，
 *       其余不出；武器效果按代价可付性决定是否发动。弃牌按牌价值升序取（先弃
 *       最低价值，同价值保持手牌序）。响应窗口取第一张真响应牌，杀响应无真杀
 *       时用两张手牌当杀（丈八蛇矛）。
 */

#ifndef INCLUDE_TKW_GAME_AI_SIMPLE_HPP
#define INCLUDE_TKW_GAME_AI_SIMPLE_HPP

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
                        out.card = Option<card::Card>::Some(req.options.front());
                    return out;
                }

                /**
                 * @brief 出牌：取第一张可出的牌，再按贪心偏好选目标。
                 * @note legal_actions 已按手牌序产出；按牌分组以复现「首张可出」
                 *       语义。OneOther 集火最低体力（方天画戟取目标最多者），
                 *       借刀对合法 {持武器者, 受害者} 对选集火对象体力最低者。
                 *       丈八蛇矛两张当杀（legal 仅在无真杀时产出）按杀优先：
                 *       先于锦囊打出，集火/多目标规则同杀。
                 */
                static DecisionChoice decide_play(const DecisionRequest &req)
                {
                    // 丈八蛇矛：手牌无真杀时两张当杀（杀优先于锦囊）
                    std::vector<LegalAction> zhangba;
                    for (const auto &a : req.legal)
                        if (!a.second_instance_id.empty())
                            zhangba.push_back(a);
                    if (!zhangba.empty())
                        return decide_zhangba(req, zhangba);

                    DecisionChoice out;
                    std::vector<std::string> order;
                    std::vector<std::vector<LegalAction>> groups;
                    for (const auto &a : req.legal)
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

#endif  // INCLUDE_TKW_GAME_AI_SIMPLE_HPP
