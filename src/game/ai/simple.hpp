/**
 * @file simple.hpp
 * @brief 确定性贪心策略：CLI 冒烟运行与回放测试用（无随机、无隐藏状态）。
 * @note 只做「合法且能推进」的动作：出杀（按回合上下文给的次数上限）、
 *       可结算锦囊、装备。不出无懈（避免自抵消）、不主动发动需额外选择的
 *       武器效果。杀次数由引擎经 TurnContext 告知，本类不维护跨调用状态。
 */

#ifndef INCLUDE_TKW_GAME_AI_SIMPLE_HPP
#define INCLUDE_TKW_GAME_AI_SIMPLE_HPP

#include <string>
#include <utility>
#include <vector>

#include "card/card.hpp"
#include "card/def.hpp"
#include "game/context.hpp"
#include "game/decision.hpp"
#include "game/effect.hpp"
#include "game/resolver.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 确定性贪心 AI（CLI 与回放测试共用）。 */
        class SimpleAI : public DecisionSource
        {
        public:
            bool play_response(
                const GameContext &, const std::string &, card::ResponseKind) override
            {
                return true;
            }

            bool play_peach(
                const GameContext &, const std::string &,
                const std::string &) override
            {
                return true;
            }

            bool play_counter(const GameContext &, const std::string &) override
            {
                return false;
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

            Option<PlayAction> choose_play(
                const GameContext &ctx, const TurnContext &turn) override
            {
                const std::string &player = turn.player;
                const bool sha_blocked = turn.sha_played >= turn.sha_limit;

                for (const auto &c : ctx.cards->hand(player))
                {
                    const auto def_opt = ctx.catalog->find(c.def_id);
                    if (def_opt.is_none())
                        continue;
                    const card::CardDef &def = *def_opt.unwrap();

                    if (def.type == card::CardType::Equipment)
                        return Option<PlayAction>::Some(PlayAction{c.instance_id, {}});

                    if (def.type == card::CardType::Trick &&
                        def.effect.is_none() && def.judge.is_some())
                    {
                        // 延时锦囊：按 judge.scope 选目标，跳过已有同名者
                        const auto scope =
                            def.judge.unwrap().scope.unwrap_or(card::Scope::Self);
                        std::vector<std::string> targets;
                        if (scope == card::Scope::Self)
                            targets.push_back(player);
                        else
                            for (const auto &e : *ctx.entities)
                                if (e->get_id() != player)
                                    targets.push_back(e->get_id());

                        std::vector<std::string> ok;
                        for (const auto &t : targets)
                        {
                            bool dup = false;
                            for (const auto &jc : ctx.cards->judge(t))
                                if (jc.def_id == def.id)
                                {
                                    dup = true;
                                    break;
                                }
                            if (!dup)
                                ok.push_back(t);
                        }
                        if (ok.empty())
                            continue;
                        if (scope == card::Scope::OneOther)
                            ok = {lowest_hp(ctx, ok)};
                        return Option<PlayAction>::Some(
                            PlayAction{c.instance_id, std::move(ok)});
                    }

                    if (def.effect.is_none())
                        continue;
                    const auto kind = def.effect.unwrap().kind;

                    if (kind == card::CardEffectKind::Damage)
                    {
                        if (sha_blocked)
                            continue;
                        const auto targets = valid_targets(ctx, player, def);
                        if (targets.empty())
                            continue;
                        return Option<PlayAction>::Some(
                            PlayAction{c.instance_id, {lowest_hp(ctx, targets)}});
                    }

                    const auto scope =
                        def.effect.unwrap().scope.unwrap_or(card::Scope::Self);
                    if (kind == card::CardEffectKind::Heal &&
                        scope == card::Scope::Self)
                    {
                        const auto me = ctx.entities->find(player);
                        if (me.is_some() &&
                            me.unwrap()->get_hp() >=
                                me.unwrap()->get_hp_bar().get_max())
                            continue;  // 满血不打桃
                    }

                    if (!is_ai_active_kind(kind))
                        continue;

                    auto targets = valid_targets(ctx, player, def);
                    if (targets.empty())
                        continue;

                    if (effect_traits(kind).target_card)
                    {
                        std::vector<std::string> with_cards;
                        for (const auto &t : targets)
                            if (has_any_card(ctx, t))
                                with_cards.push_back(t);
                        if (with_cards.empty())
                            continue;
                        targets = std::move(with_cards);
                    }

                    // 仅指定一名其他角色：集火最低体力（同血取列表序）
                    if (scope == card::Scope::OneOther)
                        targets = {lowest_hp(ctx, targets)};

                    return Option<PlayAction>::Some(
                        PlayAction{c.instance_id, std::move(targets)});
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

            /** @brief 集火：选体力最低的目标（同血取列表序，保证确定性）。 */
            static std::string lowest_hp(
                const GameContext &ctx, const std::vector<std::string> &targets)
            {
                std::string best = targets.front();
                int best_hp = hp_of(ctx, best);
                for (const auto &t : targets)
                {
                    const int h = hp_of(ctx, t);
                    if (h < best_hp)
                    {
                        best = t;
                        best_hp = h;
                    }
                }
                return best;
            }

            static bool has_any_card(const GameContext &ctx, const std::string &id)
            {
                return ctx.cards->hand_size(id) > 0 || ctx.cards->equip_size(id) > 0 ||
                       ctx.cards->judge_size(id) > 0;
            }
        };
    }
}

#endif  // INCLUDE_TKW_GAME_AI_SIMPLE_HPP
