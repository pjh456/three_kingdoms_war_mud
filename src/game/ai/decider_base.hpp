/**
 * @file decider_base.hpp
 * @brief AI 决策档共享助手：响应/救桃/无懈/触发/弃牌/丈八/目标选择等纯函数。
 * @note 只承载与档位无关、只读 DecisionRequest/AiView 的静态助手；出牌组选择、
 *       选牌策略与优先级表是档位语义，保留在各派生 Decider。助手全为静态，
 *       不新增虚函数、无实例状态，header-only 多 TU 包含无 ODR 问题。
 */

#ifndef INCLUDE_TKW_GAME_DECIDER_BASE_HPP
#define INCLUDE_TKW_GAME_DECIDER_BASE_HPP

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
                 * @brief 决策分派：按 DecisionKind 收敛八类决策的公共骨架。
                 * @param play 派生的出牌选择（档位语义：手牌序首组 / 卡类优先级）。
                 * @param pick 派生的选牌策略（档位语义：首张 / 最高价值）。
                 * @return 对应分支的决策；未匹配值回落空 DecisionChoice。
                 * @note 公共分支（响应/救桃/无懈/触发/弃牌）两档逐字相同，
                 *       仅出牌与选牌按档位策略分流，故以函数指针注入。
                 */
                static DecisionChoice dispatch(
                    const DecisionRequest &req,
                    DecisionChoice (*play)(const DecisionRequest &),
                    DecisionChoice (*pick)(const DecisionRequest &))
                {
                    switch (req.kind)
                    {
                    case DecisionKind::Play:
                        return play(req);
                    case DecisionKind::Response:
                        return decide_response(req);
                    case DecisionKind::Peach:
                        return decide_peach(req);
                    case DecisionKind::Counter:
                        return decide_counter(req);
                    case DecisionKind::Trigger:
                        return decide_trigger(req);
                    case DecisionKind::PickCard:
                    case DecisionKind::PickRevealed:
                        return pick(req);
                    case DecisionKind::Discard:
                        return decide_discard(req);
                    }
                    return DecisionChoice{};
                }

                /**
                 * @brief 无懈窗口：乱斗或无角色时回落旧口径（仅当锦囊冲自己
                 *        时出）；身份局按阵营判断——使用者为空（判定窗）或为
                 *        敌方时，敌方锦囊冲自己必出、冲友方时仅窗内首位保护者
                 *        出；友方锦囊一律不出（不拆自家人的牌）。敌方有益自益
                 *        锦囊（窗口唯一目标 = 使用者）按窗口奇偶补一张：仅当本
                 *        窗尚未被抵消（已出张数为偶）时出手。
                 * @note 候选已由适配器滤为无懈牌且窗口询问前保证非空；直调
                 *       decide 时按不出处理空候选。多目标窗（借刀等）与使用者
                 *       角色未知时回落旧口径，避免误读奇偶链。
                 */
                static DecisionChoice decide_counter(const DecisionRequest &req)
                {
                    DecisionChoice out;
                    if (req.counter_user == req.actor)
                        return out;

                    const AiView &view = req.view;
                    if (view.mode != GameMode::Identity ||
                        view.self_role == Role::None ||
                        (!req.counter_user.empty() &&
                         role_in_view(view, req.counter_user) == Role::None) ||
                        req.counter_targets.size() != 1)
                        return legacy_counter(req);
                    if (req.options.empty())
                        return out;

                    const std::string &t = req.counter_targets.front();

                    // 极性判定：回血/摸牌/亮牌选牌对目标有益，抵消它只在目标
                    // 为己方时才划算。仅当目录能解析出定义时判极性；空/未知
                    // def 保持旧行为，避免打断不携定义的直调用例。
                    const card::CardDef *trick = find_def(req, req.counter_trick);
                    bool beneficial = false;
                    if (trick && trick->effect.is_some())
                    {
                        using E = card::CardEffectKind;
                        switch (trick->effect.unwrap().kind)
                        {
                        case E::Heal:
                        case E::Draw:
                        case E::RevealPick:
                            beneficial = true;
                            break;
                        default:
                            break;
                        }
                    }

                    const bool hostile =
                        req.counter_user.empty() ||
                        is_enemy(view.self_role,
                                 role_in_view(view, req.counter_user));

                    // 自益窗（窗口唯一目标 = 使用者）且为有益效果：敌方在本窗
                    // 尚未被抵消（窗口奇偶为偶）时补一张无懈；友方自益不拆台，
                    // 已被抵消（奇数）则收手，同时消除同窗双无懈与跨轮重放。
                    if (!req.counter_user.empty() && t == req.counter_user &&
                        beneficial)
                    {
                        if (hostile && req.counter_played % 2 == 0)
                            out.instance_id = Option<std::string>::Some(
                                req.options.front().instance_id);
                        return out;
                    }

                    // 敌方有益锦囊冲友方：抵消只会伤害己方（或浪费无懈），故不出
                    if (beneficial)
                        return out;

                    const std::string start =
                        req.counter_user.empty() ? t : req.counter_user;

                    // 冲自己或友方：使用者敌对且本决策者是窗内首位保护者时
                    // 出一张；同一轮至多一名保护者出手
                    if (hostile && protects(view, t) &&
                        is_first_protector(view, t, start))
                        out.instance_id = Option<std::string>::Some(
                            req.options.front().instance_id);
                    return out;
                }

                /**
                 * @brief 旧无懈口径：锦囊目标含决策者时出第一张候选，其余不出。
                 * @note 乱斗、身份局角色缺失、多目标窗与未知使用者时的回退，
                 *       保持既有逐字节行为。
                 */
                static DecisionChoice legacy_counter(const DecisionRequest &req)
                {
                    DecisionChoice out;
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
                 * @brief 濒死救场：乱斗或无角色回落旧口径（有救场牌就出）；
                 *        身份局按自身与濒死者的阵营决定是否救。
                 * @note 濒死者身份公开，角色经既有观察取得，不新增隐藏信息面。
                 */
                static DecisionChoice decide_peach(const DecisionRequest &req)
                {
                    if (!should_save(req.view, req.dying))
                        return DecisionChoice{};
                    return decide_first_id(req);
                }

                /**
                 * @brief 是否应救濒死者：乱斗/无角色恒救；身份局主公阵营救
                 *        主公与忠臣，反贼只救反贼，内奸救自己并（主公尚未成为
                 *        最后一名非内奸时）救主公以维持制衡。
                 * @param view 救者观察（含自身与其他角色）。
                 * @param dying 濒死者 id。
                 * @return true = 打出救场牌。
                 */
                static bool should_save(const AiView &view, const std::string &dying)
                {
                    if (view.mode != GameMode::Identity ||
                        view.self_role == Role::None)
                        return true;

                    const Role r = role_in_view(view, dying);
                    switch (view.self_role)
                    {
                    case Role::Lord:
                    case Role::Loyalist:
                        return r == Role::Lord || r == Role::Loyalist;
                    case Role::Rebel:
                        return r == Role::Rebel;
                    case Role::Traitor:
                        return dying == view.self ||
                               (r == Role::Lord && !traitor_may_kill_lord(view));
                    case Role::None:
                        return true;
                    }
                    return true;
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
                        { return card_value_of(req, *a) < card_value_of(req, *b); });

                    DecisionChoice out;
                    for (std::size_t i = 0;
                         i < order.size() &&
                         static_cast<int>(out.discards.size()) < req.count;
                         ++i)
                        out.discards.push_back(order[i]->instance_id);
                    return out;
                }

                /**
                 * @brief 虚拟杀（丈八两张 / 武圣单张）：多目标动作取目标最多者
                 *        （方天画戟），否则集火最低体力；牌取首个枚举动作
                 *        （确定性）。
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
                    out.converted_sha = most->converted_sha;
                    if (most->targets.size() > 1)
                        out.targets = most->targets;
                    else
                        out.targets = {lowest_hp_action(req.view, acts)};
                    return out;
                }

                /**
                 * @brief 已选出牌组的目标选择尾段：装备/延时锦囊、借刀、单目标、
                 *        多目标。
                 * @param opts 该组全部合法动作（组首卡 = opts.front().card）。
                 * @param def 组首卡定义，调用前保证非空（见各档前置检查）。
                 * @return 该组的决策。
                 * @note 不含丈八扫描与满血自疗跳过（两档分歧，留在派生类）；
                 *       多目标动作（方天画戟）取目标最多者，否则集火最低体力。
                 * @note 重铸候选（空目标）与正常动作同卡同组：目标选择只看带目标的
                 *       候选，避免对空 targets 取 front；组内全无带目标候选时该卡无
                 *       正常动作，唯一合法解即重铸，按空目标动作返回。
                 */
                static DecisionChoice select_group_targets(
                    const DecisionRequest &req, const std::vector<LegalAction> &opts,
                    const card::CardDef &def)
                {
                    DecisionChoice out;
                    const card::Card &c = opts.front().card;

                    // 目标选择候选：剔除空目标的重铸动作；整组全空 = 仅重铸
                    std::vector<LegalAction> targeted;
                    for (const auto &a : opts)
                        if (!a.targets.empty())
                            targeted.push_back(a);
                    if (targeted.empty())
                        return pick_all(out, c.instance_id, opts.front().targets);

                    if (def.effect.is_none())
                    {
                        // 装备/延时锦囊：OneOther 集火，其余取唯一动作
                        const auto scope =
                            def.judge.is_some()
                                ? def.judge.unwrap().scope.unwrap_or(
                                      card::Scope::Self)
                                : card::Scope::Self;
                        if (scope == card::Scope::OneOther)
                            return pick_single(
                                req.view, out, c.instance_id, targeted);
                        return pick_all(out, c.instance_id, targeted.front().targets);
                    }

                    const card::CardEffect &eff = def.effect.unwrap();
                    if (eff.kind == card::CardEffectKind::BorrowedSword)
                        return pick_borrowed_sword(
                            req.view, out, c.instance_id, targeted);

                    const auto single_scope =
                        eff.scope.unwrap_or(card::Scope::Self);
                    if (single_scope == card::Scope::OneOther ||
                        single_scope == card::Scope::AnyOne)
                    {
                        // 方天画戟：存在多目标动作时优先选目标最多者
                        //（否则贪心会把它丢成单目标）
                        const LegalAction *most = &targeted.front();
                        for (const auto &a : targeted)
                            if (a.targets.size() > most->targets.size())
                                most = &a;
                        if (most->targets.size() > 1)
                            return pick_all(out, c.instance_id, most->targets);
                        return pick_single(req.view, out, c.instance_id, targeted);
                    }

                    if (eff.scope.unwrap_or(card::Scope::Self) ==
                        card::Scope::OneOrTwo)
                    {
                        // 优先双目标：取目标最多者，同多取列表序（确定性）
                        const LegalAction *most = &targeted.front();
                        for (const auto &a : targeted)
                            if (a.targets.size() > most->targets.size())
                                most = &a;
                        return pick_all(out, c.instance_id, most->targets);
                    }

                    return pick_all(out, c.instance_id, targeted.front().targets);
                }

                static DecisionChoice pick_single(
                    const AiView &view, DecisionChoice out, const std::string &id,
                    const std::vector<LegalAction> &opts)
                {
                    out.instance_id = Option<std::string>::Some(id);
                    out.targets = {best_single_target(view, opts)};
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
                 *        （先按阵营优先度、再体力最低；同档同血取列表序），
                 *        整对作为目标传回。
                 * @note 目标是双元素对，不能走按 targets.front() 选目标的
                 *       单目标路径（那会把持武器者本身当目标）。
                 */
                static DecisionChoice pick_borrowed_sword(
                    const AiView &view, DecisionChoice out, const std::string &id,
                    const std::vector<LegalAction> &opts)
                {
                    const LegalAction *best = &opts.front();
                    int best_rank = target_priority(view, best->targets[1]);
                    int best_hp = hp_of(view, best->targets[1]);
                    for (const auto &a : opts)
                    {
                        const int rank = target_priority(view, a.targets[1]);
                        const int h = hp_of(view, a.targets[1]);
                        if (rank < best_rank || (rank == best_rank && h < best_hp))
                        {
                            best = &a;
                            best_rank = rank;
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

                /**
                 * @brief req.legal 中是否存在真杀动作（手牌有真杀）。
                 * @return 任一动作的卡定义为「杀」时 true；目录缺失/未命中不算。
                 * @note 与两张当杀 pair 的「手牌有真杀」同口径：有真杀时虚拟杀
                 *       不作优先，单张转化动作也不参与分组（避免烧牌与非法选择）。
                 */
                static bool has_real_sha(const DecisionRequest &req)
                {
                    for (const auto &a : req.legal)
                    {
                        const card::CardDef *def = find_def(req, a.card.def_id);
                        if (def && def->effect.is_some() &&
                            is_sha_kind(def->effect.unwrap().kind))
                            return true;
                    }
                    return false;
                }

                /**
                 * @brief 单牌价值：弃牌排序「先弃最低价值」与选牌「取最高价值」
                 *        共用的估价。
                 * @note 空 def_id 为隐藏手牌占位槽（身份不可知），按期望常量估值，
                 *       不得据真实牌面排序；目录缺失或 def 未命中时回 0。
                 */
                static int card_value_of(
                    const DecisionRequest &req, const card::Card &c)
                {
                    if (c.def_id.empty())  // 隐藏手牌占位：不可知，按期望估值
                        return kCardValueHiddenHand;
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

                // ── 身份局阵营判定 ──────────────────────────────────────────

                /**
                 * @brief 同阵营（对称）：主公/忠臣互认，反贼互认，内奸无友。
                 * @param self 决策者角色；other 待判角色。
                 * @return true = 同阵营；任一方 None 或内奸恒 false。
                 */
                static bool is_friend(Role self, Role other)
                {
                    switch (self)
                    {
                    case Role::Lord:
                    case Role::Loyalist:
                        return other == Role::Lord || other == Role::Loyalist;
                    case Role::Rebel:
                        return other == Role::Rebel;
                    case Role::Traitor:
                    case Role::None:
                        return false;
                    }
                    return false;
                }

                /**
                 * @brief 敌对（非对称）：主公/忠臣视反贼与内奸为敌，反贼视
                 *        主公与忠臣为敌，内奸只视反贼为敌（需借主公制衡反贼）。
                 * @return true = 敌对；None 恒 false。
                 */
                static bool is_enemy(Role self, Role other)
                {
                    switch (self)
                    {
                    case Role::Lord:
                    case Role::Loyalist:
                        return other == Role::Rebel || other == Role::Traitor;
                    case Role::Rebel:
                        return other == Role::Lord || other == Role::Loyalist;
                    case Role::Traitor:
                        return other == Role::Rebel;
                    case Role::None:
                        return false;
                    }
                    return false;
                }

                /**
                 * @brief 是否仍有存活反贼（内奸保主制衡的开关）。
                 * @note 观察的 others 只含存活者，已阵亡者不在此列。
                 */
                static bool any_rebel_alive(const AiView &view)
                {
                    if (view.self_role == Role::Rebel)
                        return true;
                    for (const auto &e : view.others)
                        if (e.role == Role::Rebel)
                            return true;
                    return false;
                }

                /**
                 * @brief 是否仍有存活忠臣（内奸清场相位的对称判据）。
                 * @note 观察的 others 只含存活者，已阵亡者不在此列。
                 */
                static bool any_loyalist_alive(const AiView &view)
                {
                    if (view.self_role == Role::Loyalist)
                        return true;
                    for (const auto &e : view.others)
                        if (e.role == Role::Loyalist)
                            return true;
                    return false;
                }

                /**
                 * @brief 内奸是否仍须保主：场上还有任一派系（反贼或忠臣）
                 *        存活，主公尚非最后一名非内奸。
                 * @note 只看阵容存在性，与决策者自身角色无关；供保护者判定
                 *       使用，保证「内奸候选是否保主」由阵容而非视角决定。
                 */
                static bool traitor_keeps_lord(const AiView &view)
                {
                    return any_rebel_alive(view) || any_loyalist_alive(view);
                }

                /**
                 * @brief 内奸的落刀相位开关：无反贼且无忠臣存活时，主公已是
                 *        最后一名非内奸，杀主公即内奸独胜；在此之前内奸必须保
                 *        主：借主公制衡反贼，并先清光忠臣。
                 * @note 只看阵容存在性，不看体力/手牌，纯函数可回放；乱斗与
                 *       非内奸角色恒 false，乱斗分支因此逐字节不变。
                 */
                static bool traitor_may_kill_lord(const AiView &view)
                {
                    return view.mode == GameMode::Identity &&
                           view.self_role == Role::Traitor &&
                           !traitor_keeps_lord(view);
                }

                /**
                 * @brief 是否应保护/避让该目标：自己、同阵营友方；内奸在反贼
                 *        或忠臣尚存时额外包含主公（主公成为最后一名非内奸前不
                 *        落刀）。
                 * @note 乱斗/无角色只保护自己，其余恒 false。
                 */
                static bool protects(const AiView &view, const std::string &id)
                {
                    if (id == view.self)
                        return true;
                    if (view.mode != GameMode::Identity ||
                        view.self_role == Role::None)
                        return false;

                    const Role r = role_in_view(view, id);
                    if (is_friend(view.self_role, r))
                        return true;
                    return view.self_role == Role::Traitor && r == Role::Lord &&
                           !traitor_may_kill_lord(view);
                }

                /** @brief 避让档：应保护目标记 1，其余记 0（小者优先）。 */
                static int avoid_rank(const AiView &view, const std::string &id)
                {
                    return protects(view, id) ? 1 : 0;
                }

                /**
                 * @brief 复刻结算侧的座位序（从 start 起环绕），仅用观察中的
                 *        自身与其他存活角色的座位号。
                 * @note 与 EntityManager::order_from 同口径（按座位稳定排序后
                 *       旋转到 start）；start 不在观察中时保持原序。
                 */
                static std::vector<std::string> window_order(
                    const AiView &view, const std::string &start)
                {
                    std::vector<std::pair<int, std::string>> tmp;
                    tmp.reserve(view.others.size() + 1);
                    tmp.emplace_back(view.self_seat, view.self);
                    for (const auto &e : view.others)
                        tmp.emplace_back(e.seat, e.id);
                    std::stable_sort(
                        tmp.begin(), tmp.end(),
                        [](const auto &a, const auto &b)
                        { return a.first < b.first; });
                    std::vector<std::string> ids;
                    ids.reserve(tmp.size());
                    for (auto &p : tmp)
                        ids.push_back(std::move(p.second));

                    const auto it = std::find(ids.begin(), ids.end(), start);
                    if (it != ids.end())
                        std::rotate(ids.begin(), it, ids.end());
                    return ids;
                }

                /**
                 * @brief candidate 是否是 target 的保护者：本人、同阵营友方，
                 *        或（尚有其他派系存活时）保主的内奸。
                 * @note 内奸候选是否保主只看阵容存在性，不能按决策者角色判定，
                 *       否则主公视角会误判内奸为首位保护者而拒绝自己的无懈。
                 */
                static bool is_protector_of(
                    const AiView &view, const std::string &candidate,
                    const std::string &target)
                {
                    if (candidate == target)
                        return true;
                    if (view.mode != GameMode::Identity)
                        return false;

                    const Role cr = role_in_view(view, candidate);
                    const Role tr = role_in_view(view, target);
                    if (is_friend(cr, tr))
                        return true;
                    return cr == Role::Traitor && tr == Role::Lord &&
                           traitor_keeps_lord(view);
                }

                /**
                 * @brief 决策者是否是 target 在窗口序中的首位保护者：同一轮
                 *        至多一名保护者出手；首位保护者持多张无懈时仍会被逐轮
                 *        重问（已知残差）。
                 */
                static bool is_first_protector(
                    const AiView &view, const std::string &target,
                    const std::string &start)
                {
                    for (const auto &c : window_order(view, start))
                        if (is_protector_of(view, c, target))
                            return c == view.self;
                    return false;
                }

                // ── 有害出牌组过滤 ────────────────────────────────────────

                /**
                 * @brief 是否是对单一目标有害的效果：伤害/决斗/弃牌/顺牌与
                 *        延时锦囊；自益（摸牌/回复）、群体与借刀不算。
                 */
                static bool is_harmful_def(const card::CardDef &def)
                {
                    if (def.effect.is_none())
                        return def.judge.is_some();

                    switch (def.effect.unwrap().kind)
                    {
                    case card::CardEffectKind::Damage:
                    case card::CardEffectKind::Duel:
                    case card::CardEffectKind::DiscardTarget:
                    case card::CardEffectKind::Steal:
                    case card::CardEffectKind::FireAttack:
                        return true;
                    case card::CardEffectKind::Jink:
                    case card::CardEffectKind::Heal:
                    case card::CardEffectKind::Draw:
                    case card::CardEffectKind::AoeDamage:
                    case card::CardEffectKind::RevealPick:
                    case card::CardEffectKind::BorrowedSword:
                    case card::CardEffectKind::Analeptic:
                    case card::CardEffectKind::Chain:
                        return false;
                    }
                    return false;
                }

                /**
                 * @brief 组内动作是否全部只打应保护目标：身份局下用于整组跳过
                 *        （无目标动作与对自己使用的牌不参与过滤，直接判否）。
                 * @note 乱斗/无角色恒 false，保证乱斗逐字节不变。
                 */
                static bool group_avoids_all_targets(
                    const DecisionRequest &req, const std::vector<LegalAction> &opts)
                {
                    if (req.view.mode != GameMode::Identity ||
                        req.view.self_role == Role::None)
                        return false;

                    for (const auto &a : opts)
                    {
                        if (a.targets.empty())
                            return false;
                        for (const auto &t : a.targets)
                        {
                            // 对自己用的牌（如闪电置于自身判定区）由决策者自担，
                            // 不属避让友方范畴
                            if (t == req.view.self)
                                return false;
                            if (!protects(req.view, t))
                                return false;
                        }
                    }
                    return true;
                }

                /**
                 * @brief 阵营目标优先度：身份局按决策者阵营给敌对目标降档，
                 *        数值小者优先；乱斗或无角色时全目标恒 0（退回最低体力）。
                 * @param view 观察（含自身角色与其他角色）。
                 * @param id 待评估目标。
                 * @return 优先度：主公/忠臣视反贼与内奸同档最优先、其余后置；
                 *         反贼视主公最优先、忠臣次之、其余最后；内奸或自身角色
                 *         缺失一律 0。
                 * @note 目标角色未知时按非敌意处理（主公阵营后置、反贼归最后档）。
                 */
                static int target_priority(const AiView &view, const std::string &id)
                {
                    if (view.mode != GameMode::Identity ||
                        view.self_role == Role::None)
                        return 0;

                    const Role r = role_in_view(view, id);
                    if (view.self_role == Role::Lord ||
                        view.self_role == Role::Loyalist)
                        return (r == Role::Rebel || r == Role::Traitor) ? 0 : 1;
                    if (view.self_role == Role::Rebel)
                    {
                        if (r == Role::Lord)
                            return 0;
                        if (r == Role::Loyalist)
                            return 1;
                        return 2;
                    }
                    return 0;
                }

                /**
                 * @brief 集火：在单目标动作里先按阵营优先度、再按体力最低者
                 *        （同档同血取列表序）。
                 * @note 乱斗或无角色时优先度恒 0，比较器退化为原「体力最低」。
                 */
                static std::string lowest_hp_action(
                    const AiView &view, const std::vector<LegalAction> &opts)
                {
                    std::string best = opts.front().targets.front();
                    int best_rank = target_priority(view, best);
                    int best_hp = hp_of(view, best);
                    for (const auto &a : opts)
                    {
                        const std::string &t = a.targets.front();
                        const int rank = target_priority(view, t);
                        const int h = hp_of(view, t);
                        if (rank < best_rank || (rank == best_rank && h < best_hp))
                        {
                            best = t;
                            best_rank = rank;
                            best_hp = h;
                        }
                    }
                    return best;
                }

                /**
                 * @brief 单目标选择：身份局先避开应保护目标（同阵营友方与
                 *        内奸保主），再按阵营优先度、体力最低（同档同血取列表序）。
                 * @note 乱斗/无角色时避让档恒 0，比较器退化为原「阵营优先度 +
                 *       最低体力」；丈八 pair 的组内目标不经过本函数，保持既有
                 *       集火/最低血口径。
                 */
                static std::string best_single_target(
                    const AiView &view, const std::vector<LegalAction> &opts)
                {
                    const std::string &first = opts.front().targets.front();
                    std::string best = first;
                    int best_avoid = avoid_rank(view, first);
                    int best_rank = target_priority(view, first);
                    int best_hp = hp_of(view, first);
                    for (const auto &a : opts)
                    {
                        const std::string &t = a.targets.front();
                        const int avoid = avoid_rank(view, t);
                        const int rank = target_priority(view, t);
                        const int h = hp_of(view, t);
                        if (avoid < best_avoid ||
                            (avoid == best_avoid && rank < best_rank) ||
                            (avoid == best_avoid && rank == best_rank &&
                             h < best_hp))
                        {
                            best = t;
                            best_avoid = avoid;
                            best_rank = rank;
                            best_hp = h;
                        }
                    }
                    return best;
                }
            };
        }
    }
}

#endif  // INCLUDE_TKW_GAME_DECIDER_BASE_HPP
