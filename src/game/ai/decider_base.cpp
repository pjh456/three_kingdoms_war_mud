/**
 * @file   decider_base.cpp
 * @brief  AI 决策档共享静态助手的定义。
 * @ingroup tkw_game_ai
 */

#include "game/ai/decider_base.hpp"

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
            DecisionChoice DeciderBase::decide_response(const DecisionRequest &req)
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

            DecisionChoice DeciderBase::decide_first_id(const DecisionRequest &req)
            {
                DecisionChoice out;
                if (!req.options.empty())
                    out.instance_id = Option<std::string>::Some(
                        req.options.front().instance_id);
                return out;
            }

            DecisionChoice DeciderBase::dispatch(
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

            DecisionChoice DeciderBase::decide_counter(const DecisionRequest &req)
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

            DecisionChoice DeciderBase::legacy_counter(const DecisionRequest &req)
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

            DecisionChoice DeciderBase::decide_peach(const DecisionRequest &req)
            {
                if (!should_save(req.view, req.dying))
                    return DecisionChoice{};
                return decide_first_id(req);
            }

            bool DeciderBase::should_save(const AiView &view, const std::string &dying)
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

            DecisionChoice DeciderBase::decide_trigger(const DecisionRequest &req)
            {
                DecisionChoice out;
                out.accepted = true;
                if (req.hero_trigger)
                    return out;
                if (req.ability == card::Ability::DiscardTwoForceDamage)
                    out.accepted = req.view.hand.size() >= 2;
                return out;
            }

            DecisionChoice DeciderBase::decide_discard(const DecisionRequest &req)
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

            DecisionChoice DeciderBase::decide_zhangba(
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

            DecisionChoice DeciderBase::select_group_targets(
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

            DecisionChoice DeciderBase::pick_single(
                const AiView &view, DecisionChoice out, const std::string &id,
                const std::vector<LegalAction> &opts)
            {
                out.instance_id = Option<std::string>::Some(id);
                out.targets = {best_single_target(view, opts)};
                return out;
            }

            DecisionChoice DeciderBase::pick_all(
                DecisionChoice out, const std::string &id,
                const std::vector<std::string> &targets)
            {
                out.instance_id = Option<std::string>::Some(id);
                out.targets = targets;
                return out;
            }

            DecisionChoice DeciderBase::pick_borrowed_sword(
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

            const card::CardDef *DeciderBase::find_def(
                const DecisionRequest &req, const std::string &def_id)
            {
                if (!req.catalog)
                    return nullptr;
                const auto d = req.catalog->find(def_id);
                return d.is_some() ? d.unwrap() : nullptr;
            }

            bool DeciderBase::has_real_sha(const DecisionRequest &req)
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

            int DeciderBase::card_value_of(
                const DecisionRequest &req, const card::Card &c)
            {
                if (c.def_id.empty())  // 隐藏手牌占位：不可知，按期望估值
                    return kCardValueHiddenHand;
                const card::CardDef *def = find_def(req, c.def_id);
                return def ? card_value(*def) : 0;
            }

            int DeciderBase::hp_of(const AiView &view, const std::string &id)
            {
                if (id == view.self)
                    return view.self_hp;
                for (const auto &e : view.others)
                    if (e.id == id)
                        return e.hp;
                return 0;
            }

            bool DeciderBase::is_friend(Role self, Role other)
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

            bool DeciderBase::is_enemy(Role self, Role other)
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

            bool DeciderBase::any_rebel_alive(const AiView &view)
            {
                if (view.self_role == Role::Rebel)
                    return true;
                for (const auto &e : view.others)
                    if (e.role == Role::Rebel)
                        return true;
                return false;
            }

            bool DeciderBase::any_loyalist_alive(const AiView &view)
            {
                if (view.self_role == Role::Loyalist)
                    return true;
                for (const auto &e : view.others)
                    if (e.role == Role::Loyalist)
                        return true;
                return false;
            }

            bool DeciderBase::traitor_keeps_lord(const AiView &view)
            {
                return any_rebel_alive(view) || any_loyalist_alive(view);
            }

            bool DeciderBase::traitor_may_kill_lord(const AiView &view)
            {
                return view.mode == GameMode::Identity &&
                       view.self_role == Role::Traitor &&
                       !traitor_keeps_lord(view);
            }

            bool DeciderBase::protects(const AiView &view, const std::string &id)
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

            int DeciderBase::avoid_rank(const AiView &view, const std::string &id)
            {
                return protects(view, id) ? 1 : 0;
            }

            std::vector<std::string> DeciderBase::window_order(
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

            bool DeciderBase::is_protector_of(
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

            bool DeciderBase::is_first_protector(
                const AiView &view, const std::string &target,
                const std::string &start)
            {
                for (const auto &c : window_order(view, start))
                    if (is_protector_of(view, c, target))
                        return c == view.self;
                return false;
            }

            bool DeciderBase::is_harmful_def(const card::CardDef &def)
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

            bool DeciderBase::group_avoids_all_targets(
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

            int DeciderBase::target_priority(const AiView &view, const std::string &id)
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

            std::string DeciderBase::lowest_hp_action(
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

            std::string DeciderBase::best_single_target(
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
        }
    }
}
