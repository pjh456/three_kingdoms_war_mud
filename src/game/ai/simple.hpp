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
#include "game/ai/evaluator.hpp"
#include "game/ai/legal.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        namespace ai
        {
            /** @brief 贪心决策逻辑（AI 状态机的参考实现）。 */
            class SimpleDecider : public Decider
            {
            public:
                DecisionChoice decide(const DecisionRequest &req) override
                {
                    switch (req.kind)
                    {
                    case DecisionKind::Play:
                        return decide_play(req);
                    case DecisionKind::Response:
                        return decide_response(req);
                    case DecisionKind::Peach:
                        return decide_first_id(req);
                    case DecisionKind::Counter:
                        return decide_counter(req);
                    case DecisionKind::Trigger:
                        return decide_trigger(req);
                    case DecisionKind::PickCard:
                    case DecisionKind::PickRevealed:
                        return decide_first_card(req);
                    case DecisionKind::Discard:
                        return decide_discard(req);
                    }
                    return DecisionChoice{};
                }

            private:
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

                static DecisionChoice decide_first_card(const DecisionRequest &req)
                {
                    DecisionChoice out;
                    if (!req.options.empty())
                        out.card = Option<card::Card>::Some(req.options.front());
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
                 * @brief 出牌：取第一张可出的牌，再按贪心偏好选目标。
                 * @note legal_actions 已按手牌序产出；按牌分组以复现「首张可出」
                 *       语义。OneOther 集火最低体力（方天画戟取目标最多者），
                 *       借刀取「B = A 自身」。丈八蛇矛两张当杀（legal 仅在无真杀
                 *       时产出）按杀优先：先于锦囊打出，集火/多目标规则同杀。
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

                        if (def->effect.is_none())
                        {
                            // 装备/延时锦囊：OneOther 集火，其余取唯一动作
                            const auto scope =
                                def->judge.is_some()
                                    ? def->judge.unwrap().scope.unwrap_or(
                                          card::Scope::Self)
                                    : card::Scope::Self;
                            if (scope == card::Scope::OneOther)
                                return pick_single(req.view, out, c.instance_id, opts);
                            return pick_all(out, c.instance_id, opts.front().targets);
                        }

                        const card::CardEffect &eff = def->effect.unwrap();
                        if (eff.kind == card::CardEffectKind::Heal &&
                            eff.scope.unwrap_or(card::Scope::Self) ==
                                card::Scope::Self &&
                            req.view.self_hp >= req.view.self_max_hp)
                            continue;  // 满血不打桃

                        if (eff.kind == card::CardEffectKind::BorrowedSword)
                        {
                            for (const auto &a : opts)
                                if (a.targets.size() == 2 &&
                                    a.targets[0] == a.targets[1])
                                    return pick_all(out, c.instance_id, a.targets);
                        }

                        if (eff.scope.unwrap_or(card::Scope::Self) ==
                            card::Scope::OneOther)
                        {
                            // 方天画戟：存在多目标动作时优先选目标最多者
                            //（否则贪心会把它丢成单目标）
                            const LegalAction *most = &opts.front();
                            for (const auto &a : opts)
                                if (a.targets.size() > most->targets.size())
                                    most = &a;
                            if (most->targets.size() > 1)
                                return pick_all(out, c.instance_id, most->targets);
                            return pick_single(req.view, out, c.instance_id, opts);
                        }

                        return pick_all(out, c.instance_id, opts.front().targets);
                    }
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

        /** @brief 兼容既有调用点：SimpleAI 保留在 game 命名空间。 */
        using ai::SimpleAI;
    }
}

#endif  // INCLUDE_TKW_GAME_AI_SIMPLE_HPP
