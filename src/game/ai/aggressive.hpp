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

#include "card/def.hpp"
#include "game/ai/decider.hpp"
#include "game/ai/evaluator.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        namespace ai
        {
            /** @brief 攻击优先决策逻辑（卡类优先级出牌 + 集火最低体力）。 */
            class AggressiveDecider : public Decider
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
                 *       最低体力；OneOther 效果牌同规则；借刀取「B = A 自身」
                 *       动作，否则首动作；AOE/自作用取首动作完整 targets。
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

                /** @brief 选牌候选的单牌价值；目录缺失或 def 未命中时回 0。 */
                static int pick_value(const DecisionRequest &req, const card::Card &c)
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
