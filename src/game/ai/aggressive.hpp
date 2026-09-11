/**
 * @file aggressive.hpp
 * @brief 攻击优先策略：伤害/多目标先行的决策档，供 CLI `--ai aggressive` 使用。
 * @note 逻辑集中在 AggressiveDecider::decide（只读 DecisionRequest）；AggressiveAI
 *       把它经 RequestDecisionSource 适配成引擎可用的 DecisionSource。相对贪心档
 *       的难度定位：出牌阶段按「卡类优先级」选组打出（小者先打，顺序见
 *       kPlayPriority* 常量，同分取 legal 序靠前者）；选牌分支
 *       （PickCard/PickRevealed）取最高牌价值而非首张。其余六个分支（响应/救桃/
 *       无懈/触发/弃牌/丈八组目标）与贪心档同逻辑。无随机、无隐藏状态，确定性可回放。
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
                    return dispatch(req, decide_play, decide_best_card);
                }

            private:
                // 卡类出牌优先级（小者先打；同分取 legal 序靠前者）；-1 = 跳过该组。
                static constexpr int kPlayPriorityDamage = 1;        /**< 伤害/丈八两张当杀 */
                static constexpr int kPlayPriorityAoe = 2;           /**< 群体伤害 */
                static constexpr int kPlayPriorityDelayed = 3;       /**< 延时锦囊 */
                static constexpr int kPlayPriorityDuel = 4;          /**< 决斗 */
                static constexpr int kPlayPriorityDiscard = 5;       /**< 过河拆桥 */
                static constexpr int kPlayPrioritySteal = 6;         /**< 顺手牵羊 */
                static constexpr int kPlayPriorityDraw = 7;          /**< 无中生有 */
                static constexpr int kPlayPriorityBorrowedSword = 8; /**< 借刀杀人 */
                static constexpr int kPlayPriorityHeal = 9;          /**< 桃（满血跳过） */
                static constexpr int kPlayPriorityOther = 10;        /**< 装备/无效果/亮牌/未知兜底 */
                static constexpr int kPlayPrioritySkip = -1;         /**< 跳过（缺 def / 满血自疗） */

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
                    int best_value = card_value_of(req, *best);
                    for (std::size_t i = 1; i < req.options.size(); ++i)
                    {
                        const int v = card_value_of(req, req.options[i]);
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
                 * @note 丈八组按伤害接管；其余目标选择委托共享尾段，组内丈八
                 *       扫描范围是本档与贪心档的有意分歧，故留在本处。
                 */
                static DecisionChoice decide_play_group(
                    const DecisionRequest &req, const std::vector<LegalAction> &opts,
                    const card::CardDef *def)
                {
                    // 丈八蛇矛：组内存在两张当杀动作（第二张非空）即按伤害接管
                    std::vector<LegalAction> zhangba;
                    for (const auto &a : opts)
                        if (!a.second_instance_id.empty())
                            zhangba.push_back(a);
                    if (!zhangba.empty())
                        return decide_zhangba(req, zhangba);

                    return select_group_targets(req, opts, *def);
                }

                /**
                 * @brief 卡组优先级：含两张当杀动作的组按伤害档；否则按组
                 *        首张卡的定义分派。
                 * @return kPlayPrioritySkip = 该组跳过（目录缺失；满血自疗）。
                 * @note 丈八 pair 仅在无真杀时产出，与真杀同优先级无冲突。
                 */
                static int group_priority(
                    const DecisionRequest &req,
                    const std::vector<LegalAction> &opts)
                {
                    for (const auto &a : opts)
                        if (!a.second_instance_id.empty())
                            return kPlayPriorityDamage;
                    const card::CardDef *def =
                        find_def(req, opts.front().card.def_id);
                    if (!def)
                        return kPlayPrioritySkip;
                    return play_priority(req, *def);
                }

                /**
                 * @brief 卡类优先级（小者先打）：顺序见 kPlayPriority* 常量。
                 * @return kPlayPrioritySkip = 跳过（满血自疗，同贪心档「满血不打桃」）。
                 * @note 无主动效果的锦囊（延时判定牌）排装备之前；响应牌不会
                 *       进入 legal，归入兜底档。
                 */
                static int play_priority(
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
                    case card::CardEffectKind::Damage:
                        return kPlayPriorityDamage;
                    case card::CardEffectKind::AoeDamage:
                        return kPlayPriorityAoe;
                    case card::CardEffectKind::Duel:
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
                        break;
                    }
                    return kPlayPriorityOther;
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
