/**
 * @file simple.hpp
 * @brief 确定性贪心策略：CLI 冒烟运行与回放测试用（无随机、无隐藏状态）。
 * @note 只做「合法且能推进」的动作：出杀（按回合上下文给的次数上限）、
 *       可结算锦囊、装备。不出无懈（避免自抵消）、不主动发动需额外选择的
 *       武器效果。杀次数由引擎经 TurnContext 告知，本类不维护跨调用状态。
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
#include "game/ai/legal.hpp"
#include "game/core/context.hpp"
#include "game/core/decision.hpp"
#include "game/resolve/response.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 确定性贪心 AI（CLI 与回放测试共用）。 */
        class SimpleAI : public DecisionSource
        {
        public:
            Option<std::string> play_response(
                const GameContext &ctx, const std::string &entity,
                card::ResponseKind kind) override
            {
                for (const auto &c : ctx.cards->hand(entity))
                {
                    const auto def = ctx.catalog->find(c.def_id);
                    if (def.is_some() && is_response_def(*def.unwrap(), kind))
                        return Option<std::string>::Some(c.instance_id);
                }
                return Option<std::string>::None();
            }

            Option<std::string> play_peach(
                const GameContext &ctx, const std::string &saver,
                const std::string &) override
            {
                for (const auto &c : ctx.cards->hand(saver))
                {
                    const auto def = ctx.catalog->find(c.def_id);
                    if (def.is_some() && def.unwrap()->rescue)
                        return Option<std::string>::Some(c.instance_id);
                }
                return Option<std::string>::None();
            }

            Option<std::string> play_counter(
                const GameContext &, const std::string &) override
            {
                return Option<std::string>::None();  // 不出无懈（避免自抵消）
            }

            bool trigger_effect(
                const GameContext &, const std::string &, card::Ability) override
            {
                return true;
            }

            Option<card::Card> pick_card_from_target(
                const GameContext &ctx, const std::string &,
                const std::string &target) override
            {
                if (!ctx.cards->hand(target).empty())
                    return Option<card::Card>::Some(ctx.cards->hand(target).front());
                if (!ctx.cards->equip(target).empty())
                    return Option<card::Card>::Some(ctx.cards->equip(target).front());
                if (!ctx.cards->judge(target).empty())
                    return Option<card::Card>::Some(ctx.cards->judge(target).front());
                return Option<card::Card>::None();
            }

            Option<card::Card> pick_from_revealed(
                const GameContext &, const std::string &,
                const std::vector<card::Card> &options) override
            {
                if (options.empty())
                    return Option<card::Card>::None();
                return Option<card::Card>::Some(options.front());
            }

            Option<PlayAction> choose_play(
                const GameContext &ctx, const TurnContext &turn) override
            {
                const std::string &player = turn.player;

                // legal_actions 已按手牌序产出全部合法动作；按牌分组以复现
                // 「取第一张可出的牌，再按贪心偏好选目标」的既有行为。
                const auto legal = legal_actions(ctx, player, turn);
                std::vector<std::string> order;
                std::vector<std::vector<LegalAction>> groups;
                for (const auto &a : legal)
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
                    const auto def_opt = ctx.catalog->find(c.def_id);
                    if (def_opt.is_none())
                        continue;
                    const card::CardDef &def = *def_opt.unwrap();

                    if (def.effect.is_none())
                    {
                        // 装备/延时锦囊：OneOther 集火最低体力，其余取唯一动作
                        const auto scope =
                            def.judge.is_some()
                                ? def.judge.unwrap().scope.unwrap_or(
                                      card::Scope::Self)
                                : card::Scope::Self;
                        if (scope == card::Scope::OneOther)
                            return Option<PlayAction>::Some(PlayAction{
                                c.instance_id, {lowest_hp_action(ctx, opts)}});
                        return Option<PlayAction>::Some(
                            PlayAction{c.instance_id, opts.front().targets});
                    }

                    const card::CardEffect &eff = def.effect.unwrap();
                    if (eff.kind == card::CardEffectKind::Heal &&
                        eff.scope.unwrap_or(card::Scope::Self) ==
                            card::Scope::Self)
                    {
                        const auto me = ctx.entities->find(player);
                        if (me.is_some() &&
                            me.unwrap()->get_hp() >=
                                me.unwrap()->get_hp_bar().get_max())
                            continue;  // 满血不打桃
                    }

                    if (eff.kind == card::CardEffectKind::BorrowedSword)
                    {
                        // 借刀：取「B = A 自身」的目标组合
                        for (const auto &a : opts)
                            if (a.targets.size() == 2 && a.targets[0] == a.targets[1])
                                return Option<PlayAction>::Some(
                                    PlayAction{c.instance_id, a.targets});
                    }

                    // 仅指定一名其他角色：集火最低体力（同血取列表序）
                    if (eff.scope.unwrap_or(card::Scope::Self) ==
                        card::Scope::OneOther)
                        return Option<PlayAction>::Some(PlayAction{
                            c.instance_id, {lowest_hp_action(ctx, opts)}});

                    return Option<PlayAction>::Some(
                        PlayAction{c.instance_id, opts.front().targets});
                }
                return Option<PlayAction>::None();
            }

            std::vector<std::string> choose_discards(
                const GameContext &ctx, const std::string &player, int count,
                DiscardReason) override
            {
                std::vector<std::string> out;
                for (const auto &c : ctx.cards->hand(player))
                {
                    if (static_cast<int>(out.size()) >= count)
                        break;
                    out.push_back(c.instance_id);
                }
                return out;
            }

        private:
            static int hp_of(const GameContext &ctx, const std::string &id)
            {
                const auto e = ctx.entities->find(id);
                return e.is_some() ? e.unwrap()->get_hp() : 0;
            }

            /** @brief 集火：在单目标动作里选体力最低的目标（同血取列表序）。 */
            static std::string lowest_hp_action(
                const GameContext &ctx, const std::vector<LegalAction> &opts)
            {
                std::string best = opts.front().targets.front();
                int best_hp = hp_of(ctx, best);
                for (const auto &a : opts)
                {
                    const int h = hp_of(ctx, a.targets.front());
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

#endif  // INCLUDE_TKW_GAME_AI_SIMPLE_HPP
