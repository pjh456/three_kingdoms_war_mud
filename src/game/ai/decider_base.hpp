/**
 * @file decider_base.hpp
 * @brief AI 决策档共享助手：响应/救桃/无懈/触发/弃牌/丈八/目标选择等纯函数。
 * @note 只承载与档位无关、只读 DecisionRequest/AiView 的静态助手；出牌组选择、
 *       选牌策略与优先级表是档位语义，保留在各派生 Decider。助手全为静态，
 *       不新增虚函数、无实例状态，header-only 多 TU 包含无 ODR 问题。
 */

#ifndef INCLUDE_TKW_GAME_AI_DECIDER_BASE_HPP
#define INCLUDE_TKW_GAME_AI_DECIDER_BASE_HPP

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "card/card.hpp"
#include "card/def.hpp"
#include "game/ai/decider.hpp"
#include "game/ai/evaluator.hpp"
#include "game/ai/legal.hpp"
#include "game/ai/view.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        namespace ai
        {
            /**
             * @class DeciderBase
             * @brief 决策档共享基类：集中各档逐字相同的静态决策助手。
             * @note 仍为抽象类（decide 未实现）；派生档只实现档位特有逻辑并复用
             *       此处助手，规则修复改一处即两档同步。
             */
            class DeciderBase : public Decider
            {
            protected:
                /**
                 * @brief 响应窗口：真响应牌（闪/杀）取第一张候选；杀响应无真杀时
                 *        取首个两张当杀 pair（丈八蛇矛，真杀优先与主动侧一致）。
                 */
                static DecisionChoice decide_response(const DecisionRequest &req)
                {
                    DecisionChoice out;
                    if (!req.options.empty())
                        out.instance_id = Option<std::string>::Some(
                            req.options.front().instance_id);
                    else if (!req.legal.empty())
                    {
                        const auto &pair = req.legal.front();
                        out.instance_id =
                            Option<std::string>::Some(pair.card.instance_id);
                        out.second_instance_id = pair.second_instance_id;
                    }
                    return out;
                }

                /** 救桃取第一张候选。 */
                static DecisionChoice decide_first_id(const DecisionRequest &req)
                {
                    DecisionChoice out;
                    if (!req.options.empty())
                        out.instance_id = Option<std::string>::Some(
                            req.options.front().instance_id);
                    return out;
                }

                /**
                 * @brief 无懈窗口：自己的锦囊不自我抵消；锦囊目标含决策者
                 *        （敌人锦囊冲我 / 我判定区的延时锦囊）时出第一张无懈；
                 *        其余（敌人自益锦囊、第三方锦囊）不出。
                 * @note 候选已由适配器滤为无懈牌且窗口询问前保证非空；直调
                 *       decide 时按不出处理空候选。
                 */
                static DecisionChoice decide_counter(const DecisionRequest &req)
                {
                    DecisionChoice out;
                    if (req.counter_user == req.actor)
                        return out;
                    if (std::find(req.counter_targets.begin(),
                                  req.counter_targets.end(), req.actor) ==
                        req.counter_targets.end())
                        return out;
                    if (req.options.empty())
                        return out;
                    out.instance_id = Option<std::string>::Some(
                        req.options.front().instance_id);
                    return out;
                }

                /**
                 * @brief 触发：按代价可付性决定是否发动装备能力。
                 * @note 贯石斧需弃两张，手牌不足 2 张不发动；免费能力一律发动
                 *       （目标侧代价在接缝内不可知，由引擎侧预检兜底）。
                 */
                static DecisionChoice decide_trigger(const DecisionRequest &req)
                {
                    DecisionChoice out;
                    out.accepted = true;
                    if (req.ability == card::Ability::DiscardTwoForceDamage)
                        out.accepted = req.view.hand.size() >= 2;
                    return out;
                }

                /**
                 * @brief 弃牌：候选按牌价值升序稳定排序（先弃最低价值），
                 *        同价值保持手牌序，取前 count 张。
                 * @note 目录缺失时全部价值为 0，稳定排序退化回手牌原序。
                 */
                static DecisionChoice decide_discard(const DecisionRequest &req)
                {
                    std::vector<const card::Card *> order;
                    order.reserve(req.options.size());
                    for (const auto &c : req.options)
                        order.push_back(&c);
                    std::stable_sort(
                        order.begin(), order.end(),
                        [&req](const card::Card *a, const card::Card *b)
                        { return discard_value(req, *a) < discard_value(req, *b); });

                    DecisionChoice out;
                    for (std::size_t i = 0;
                         i < order.size() &&
                         static_cast<int>(out.discards.size()) < req.count;
                         ++i)
                        out.discards.push_back(order[i]->instance_id);
                    return out;
                }

                /**
                 * @brief 丈八：多目标动作取目标最多者（方天画戟），否则集火
                 *        最低体力；牌对取首个枚举 pair（确定性）。
                 */
                static DecisionChoice decide_zhangba(
                    const DecisionRequest &req, const std::vector<LegalAction> &acts)
                {
                    DecisionChoice out;
                    const LegalAction *most = &acts.front();
                    for (const auto &a : acts)
                        if (a.targets.size() > most->targets.size())
                            most = &a;
                    out.instance_id = Option<std::string>::Some(most->card.instance_id);
                    out.second_instance_id = most->second_instance_id;
                    if (most->targets.size() > 1)
                        out.targets = most->targets;
                    else
                        out.targets = {lowest_hp_action(req.view, acts)};
                    return out;
                }

                static DecisionChoice pick_single(
                    const AiView &view, DecisionChoice out, const std::string &id,
                    const std::vector<LegalAction> &opts)
                {
                    out.instance_id = Option<std::string>::Some(id);
                    out.targets = {lowest_hp_action(view, opts)};
                    return out;
                }

                static DecisionChoice pick_all(
                    DecisionChoice out, const std::string &id,
                    const std::vector<std::string> &targets)
                {
                    out.instance_id = Option<std::string>::Some(id);
                    out.targets = targets;
                    return out;
                }

                /**
                 * @brief 借刀杀人：在合法 {持武器者, 受害者} 对里选集火对象
                 *        体力最低者（同血取列表序），整对作为目标传回。
                 * @note 目标是双元素对，不能走按 targets.front() 选目标的
                 *       单目标路径（那会把持武器者本身当目标）。
                 */
                static DecisionChoice pick_borrowed_sword(
                    const AiView &view, DecisionChoice out, const std::string &id,
                    const std::vector<LegalAction> &opts)
                {
                    const LegalAction *best = &opts.front();
                    int best_hp = hp_of(view, best->targets[1]);
                    for (const auto &a : opts)
                    {
                        const int h = hp_of(view, a.targets[1]);
                        if (h < best_hp)
                        {
                            best = &a;
                            best_hp = h;
                        }
                    }
                    return pick_all(out, id, best->targets);
                }

                static const card::CardDef *find_def(
                    const DecisionRequest &req, const std::string &def_id)
                {
                    if (!req.catalog)
                        return nullptr;
                    const auto d = req.catalog->find(def_id);
                    return d.is_some() ? d.unwrap() : nullptr;
                }

                /** @brief 弃牌排序用的单牌价值；目录缺失或 def 未命中时回 0。 */
                static int discard_value(
                    const DecisionRequest &req, const card::Card &c)
                {
                    const card::CardDef *def = find_def(req, c.def_id);
                    return def ? card_value(*def) : 0;
                }

                static int hp_of(const AiView &view, const std::string &id)
                {
                    if (id == view.self)
                        return view.self_hp;
                    for (const auto &e : view.others)
                        if (e.id == id)
                            return e.hp;
                    return 0;
                }

                /** @brief 集火：在单目标动作里选体力最低者（同血取列表序）。 */
                static std::string lowest_hp_action(
                    const AiView &view, const std::vector<LegalAction> &opts)
                {
                    std::string best = opts.front().targets.front();
                    int best_hp = hp_of(view, best);
                    for (const auto &a : opts)
                    {
                        const int h = hp_of(view, a.targets.front());
                        if (h < best_hp)
                        {
                            best = a.targets.front();
                            best_hp = h;
                        }
                    }
                    return best;
                }
            };
        }
    }
}

#endif  // INCLUDE_TKW_GAME_AI_DECIDER_BASE_HPP
