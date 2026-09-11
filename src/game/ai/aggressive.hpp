/**
 * @file aggressive.hpp
 * @brief 攻击优先策略：伤害/多目标先行的决策档，供 CLI `--ai aggressive` 使用。
 * @note 逻辑集中在 AggressiveDecider::decide（只读 DecisionRequest）；AggressiveAI
 *       把它经 RequestDecisionSource 适配成引擎可用的 DecisionSource。相对贪心档
 *       的难度定位：出牌阶段按「卡类优先级」选组打出（伤害 1 > AOE 2 > 延时 3 >
 *       决斗 4 > 拆 5 > 顺 6 > 摸 7 > 借刀 8 > 自疗 9 > 装备 10），同分取 legal
 *       序靠前者；选牌分支（PickCard/PickRevealed）取最高牌价值而非首张。其余
 *       六个分支（响应/救桃/无懈/触发/弃牌/丈八组目标）与贪心档同逻辑。无随机、
 *       无隐藏状态，确定性可回放。
 */

#ifndef INCLUDE_TKW_GAME_AI_AGGRESSIVE_HPP
#define INCLUDE_TKW_GAME_AI_AGGRESSIVE_HPP

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "card/card.hpp"
#include "card/def.hpp"
#include "game/ai/decider.hpp"
#include "game/ai/decider_base.hpp"
#include "game/ai/evaluator.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        namespace ai
        {
            /** @brief 攻击优先决策逻辑（卡类优先级出牌 + 集火最低体力）。 */
            class AggressiveDecider : public DeciderBase
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
                        return decide_best_card(req);
                    case DecisionKind::Discard:
                        return decide_discard(req);
                    }
                    return DecisionChoice{};
                }

            private:
                /**
                 * @brief 选牌（PickCard/PickRevealed）：取牌价值最高者；同值保持
                 *        候选序靠前者；空候选返回 None（PickRevealed 由引擎回落
                 *        第一张）。
                 * @note 顺手牵羊/过河拆桥抢高价值牌（桃 > 杀 > 闪 > 装备）；
                 *        五谷丰登高价值优先。
                 */
                static DecisionChoice decide_best_card(const DecisionRequest &req)
                {
                    DecisionChoice out;
                    if (req.options.empty())
                        return out;

                    const card::Card *best = &req.options.front();
                    int best_value = pick_value(req, *best);
                    for (std::size_t i = 1; i < req.options.size(); ++i)
                    {
                        const int v = pick_value(req, req.options[i]);
                        if (v > best_value)
                        {
                            best_value = v;
                            best = &req.options[i];
                        }
                    }
                    out.card = Option<card::Card>::Some(*best);
                    return out;
                }

                /**
                 * @brief 出牌：按牌分组（legal 首现序 = 手牌序），按卡类优先级
                 *        选出牌组（小者先打，同分取 legal 序靠前者），再组内选目标。
                 * @note 丈八组（含第二张手牌动作）按伤害对待；满血自疗与目录缺失
                 *       的组跳过；全跳过时返回空 Choice = 结束出牌阶段。
                 */
                static DecisionChoice decide_play(const DecisionRequest &req)
                {
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

                /**
                 * @brief 已选组内选目标。
                 * @param def 组代表卡的定义；丈八组允许为空（按伤害结算，
                 *        不查目录），非丈八组调用前已保证非空。
                 * @note 丈八组：多目标动作取目标最多者（方天画戟），否则集火
                 *       最低体力；OneOther 效果牌同规则；借刀取受害者体力最低
                 *       的合法对；AOE/自作用取首动作完整 targets。
                 */
                static DecisionChoice decide_play_group(
                    const DecisionRequest &req, const std::vector<LegalAction> &opts,
                    const card::CardDef *def)
                {
                    DecisionChoice out;
                    const card::Card &c = opts.front().card;

                    // 丈八蛇矛：组内存在两张当杀动作（第二张非空）即按伤害接管
                    std::vector<LegalAction> zhangba;
                    for (const auto &a : opts)
                        if (!a.second_instance_id.empty())
                            zhangba.push_back(a);
                    if (!zhangba.empty())
                        return decide_zhangba(req, zhangba);

                    if (def->effect.is_none())
                    {
                        // 装备/延时锦囊：OneOther 集火，其余取唯一动作
                        const auto scope =
                            def->judge.is_some()
                                ? def->judge.unwrap().scope.unwrap_or(
                                      card::Scope::Self)
                                : card::Scope::Self;
                        if (scope == card::Scope::OneOther)
                            return pick_single(
                                req.view, out, c.instance_id, opts);
                        return pick_all(out, c.instance_id, opts.front().targets);
                    }

                    const card::CardEffect &eff = def->effect.unwrap();
                    if (eff.kind == card::CardEffectKind::BorrowedSword)
                        return pick_borrowed_sword(
                            req.view, out, c.instance_id, opts);

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

                /**
                 * @brief 卡组优先级：含两张当杀动作的组按伤害（1）；否则按组
                 *        首张卡的定义分派。
                 * @return -1 = 该组跳过（目录缺失；满血自疗）。
                 * @note 丈八 pair 仅在无真杀时产出，与真杀同优先级无冲突。
                 */
                static int group_priority(
                    const DecisionRequest &req,
                    const std::vector<LegalAction> &opts)
                {
                    for (const auto &a : opts)
                        if (!a.second_instance_id.empty())
                            return 1;
                    const card::CardDef *def =
                        find_def(req, opts.front().card.def_id);
                    if (!def)
                        return -1;
                    return play_priority(req, *def);
                }

                /**
                 * @brief 卡类优先级（小者先打）：伤害 1 / AOE 2 / 延时锦囊 3 /
                 *        决斗 4 / 拆 5 / 顺 6 / 摸 7 / 借刀 8 / 自疗 9 / 装备等 10。
                 * @return -1 = 跳过（满血自疗，同贪心档「满血不打桃」）。
                 * @note 无主动效果的锦囊（延时判定牌）排装备之前；响应牌不会
                 *       进入 legal，归入兜底档。
                 */
                static int play_priority(
                    const DecisionRequest &req, const card::CardDef &def)
                {
                    if (def.effect.is_none())
                        return (def.type == card::CardType::Trick &&
                                def.judge.is_some())
                            ? 3
                            : 10;
                    const card::CardEffect &eff = def.effect.unwrap();
                    switch (eff.kind)
                    {
                    case card::CardEffectKind::Damage:
                        return 1;
                    case card::CardEffectKind::AoeDamage:
                        return 2;
                    case card::CardEffectKind::Duel:
                        return 4;
                    case card::CardEffectKind::DiscardTarget:
                        return 5;
                    case card::CardEffectKind::Steal:
                        return 6;
                    case card::CardEffectKind::Draw:
                        return 7;
                    case card::CardEffectKind::BorrowedSword:
                        return 8;
                    case card::CardEffectKind::Heal:
                        if (eff.scope.unwrap_or(card::Scope::Self) ==
                                card::Scope::Self &&
                            req.view.self_hp >= req.view.self_max_hp)
                            return -1;  // 满血不打桃
                        return 9;
                    case card::CardEffectKind::Jink:
                    case card::CardEffectKind::RevealPick:
                        break;
                    }
                    return 10;
                }

                /** @brief 选牌候选的单牌价值；目录缺失或 def 未命中时回 0。 */
                static int pick_value(const DecisionRequest &req, const card::Card &c)
                {
                    const card::CardDef *def = find_def(req, c.def_id);
                    return def ? card_value(*def) : 0;
                }
            };

            /** @brief 引擎可用的攻击优先决策源：AggressiveDecider 经适配器接入。 */
            class AggressiveAI : private AggressiveDecider,
                                  public RequestDecisionSource
            {
            public:
                AggressiveAI() :
                    RequestDecisionSource(static_cast<AggressiveDecider &>(*this))
                {
                }
            };
        }

        /** @brief 兼容 CLI 调用点：AggressiveAI 提升到 game 命名空间。 */
        using ai::AggressiveAI;
    }
}

#endif  // INCLUDE_TKW_GAME_AI_AGGRESSIVE_HPP
