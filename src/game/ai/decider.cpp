/**
 * @file   decider.cpp
 * @brief  统一决策接缝 RequestDecisionSource 的定义。
 * @ingroup tkw_game_ai
 */

#include "game/ai/decider.hpp"

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
            Option<PlayAction> RequestDecisionSource::play_response(
                const ReadOnlyContext &ctx, const std::string &entity,
                card::ResponseKind kind, const ResponsePrompt &prompt)
            {
                DecisionRequest req = base_request(ctx, entity);
                req.kind = DecisionKind::Response;
                req.response_kind = kind;
                req.response_source = prompt.source_def_id;
                req.response_user = prompt.source_user;
                req.response_damage = prompt.damage;
                for (const auto &c : ctx.cards->hand(entity))
                    if (is_response_card(ctx, c, kind))
                        req.options.push_back(c);
                // 单张转化当杀（武圣红牌 / 龙胆闪）：杀响应窗口并入单牌
                // 候选（真响应牌优先在前，同张真杀不重复；转化合法性由
                // 引擎按武将 + 牌面识别）
                if (kind == card::ResponseKind::Sha)
                    for (const auto &c : HeroQuery::sha_conversion_cards(ctx, entity))
                        req.options.push_back(c);
                // 单张转化当闪（龙胆杀）：闪响应窗口并入单牌候选
                if (kind == card::ResponseKind::Jink)
                    for (const auto &c : HeroQuery::jink_conversion_cards(ctx, entity))
                        req.options.push_back(c);
                // 杀响应窗口：携带两张当杀 pair 候选（与主动侧同一枚举口径）
                if (kind == card::ResponseKind::Sha)
                    for (const auto &[first, second] :
                         two_cards_as_sha_pairs(ctx, entity))
                        req.legal.push_back(LegalAction{first, {}, second.instance_id});
                const auto choice = m_decider->decide(req);
                if (choice.instance_id.is_none())
                    return Option<PlayAction>::None();
                return Option<PlayAction>::Some(PlayAction{
                    choice.instance_id.unwrap(), {}, choice.second_instance_id});
            }

            Option<std::string> RequestDecisionSource::play_peach(
                const ReadOnlyContext &ctx, const std::string &saver,
                const std::string &dying)
            {
                DecisionRequest req = base_request(ctx, saver);
                req.kind = DecisionKind::Peach;
                req.dying = dying;
                const bool is_self = (saver == dying);
                for (const auto &c : ctx.cards->hand(saver))
                    if (is_rescue_card(ctx, c, is_self))
                        req.options.push_back(c);
                return m_decider->decide(req).instance_id;
            }

            Option<std::string> RequestDecisionSource::play_counter(
                const ReadOnlyContext &ctx, const std::string &player,
                const std::string &trick_user,
                const std::vector<std::string> &trick_targets,
                const std::string &trick_def_id, int counter_played)
            {
                DecisionRequest req = base_request(ctx, player);
                req.kind = DecisionKind::Counter;
                req.counter_user = trick_user;
                req.counter_targets = trick_targets;
                req.counter_trick = trick_def_id;
                req.counter_played = counter_played;
                for (const auto &c : ctx.cards->hand(player))
                    if (is_counter_card(ctx, c))
                        req.options.push_back(c);
                return m_decider->decide(req).instance_id;
            }

            bool RequestDecisionSource::trigger_effect(
                const ReadOnlyContext &ctx, const std::string &player,
                card::Ability ability)
            {
                DecisionRequest req = base_request(ctx, player);
                req.kind = DecisionKind::Trigger;
                req.ability = ability;
                return m_decider->decide(req).accepted;
            }

            bool RequestDecisionSource::trigger_hero_skill(
                const ReadOnlyContext &ctx, const std::string &player,
                hero::HeroSkill skill, const std::string &cause)
            {
                DecisionRequest req = base_request(ctx, player);
                req.kind = DecisionKind::Trigger;
                req.hero_trigger = true;
                req.hero_skill = skill;
                req.trigger_cause = cause;
                return m_decider->decide(req).accepted;
            }

            Option<TargetPick> RequestDecisionSource::pick_card_from_target(
                const ReadOnlyContext &ctx, const std::string &source,
                const std::string &target, PickCardScope scope)
            {
                DecisionRequest req = base_request(ctx, source);
                req.kind = DecisionKind::PickCard;
                req.target = target;

                // 隐藏手牌：只放无身份占位槽，数量与真实手牌一致，绝不拷贝身份
                for (std::size_t i = 0; i < ctx.cards->hand_size(target); ++i)
                {
                    req.options.push_back(card::Card{});
                    req.zone_labels.push_back(card::Zone::Hand);
                }
                append_zone(
                    req.options, req.zone_labels, ctx.cards->equip(target),
                    card::Zone::Equip);
                if (scope == PickCardScope::HandEquipJudge)
                    append_zone(
                        req.options, req.zone_labels, ctx.cards->judge(target),
                        card::Zone::Judge);

                const auto choice = m_decider->decide(req);
                if (choice.option_index.is_none())
                    return Option<TargetPick>::None();
                const std::size_t i = choice.option_index.unwrap();
                if (i >= req.options.size())
                    return Option<TargetPick>::None();

                const card::Zone zone = req.zone_labels[i];
                // 隐藏手牌只回传槽位，真实身份由引擎随机暗抽
                if (zone == card::Zone::Hand)
                    return Option<TargetPick>::Some(
                        TargetPick{card::Zone::Hand, i, Option<card::Card>::None()});
                return Option<TargetPick>::Some(
                    TargetPick{zone, i, Option<card::Card>::Some(req.options[i])});
            }

            Option<card::Card> RequestDecisionSource::pick_from_revealed(
                const ReadOnlyContext &ctx, const std::string &player,
                const std::vector<card::Card> &options, RevealSource source)
            {
                DecisionRequest req = base_request(ctx, player);
                req.kind = DecisionKind::PickRevealed;
                req.options = options;
                req.reveal_source = source;

                const auto choice = m_decider->decide(req);
                if (choice.option_index.is_none())
                    return Option<card::Card>::None();
                const std::size_t i = choice.option_index.unwrap();
                if (i >= req.options.size())
                    return Option<card::Card>::None();
                return Option<card::Card>::Some(req.options[i]);
            }

            Option<PlayAction> RequestDecisionSource::choose_play(
                const ReadOnlyContext &ctx, const TurnContext &turn)
            {
                DecisionRequest req = base_request(ctx, turn.player);
                req.kind = DecisionKind::Play;
                req.turn = turn;
                req.legal = legal_actions(ctx, turn.player, turn);
                const auto choice = m_decider->decide(req);
                if (choice.instance_id.is_none())
                    return Option<PlayAction>::None();
                return Option<PlayAction>::Some(PlayAction{
                    choice.instance_id.unwrap(), choice.targets,
                    choice.second_instance_id, choice.recast,
                    choice.converted_sha});
            }

            std::vector<std::string> RequestDecisionSource::choose_discards(
                const ReadOnlyContext &ctx, const std::string &player, int count,
                DiscardReason reason)
            {
                DecisionRequest req = base_request(ctx, player);
                req.kind = DecisionKind::Discard;
                req.count = count;
                req.discard_reason = reason;
                req.options = ctx.cards->hand(player);
                return m_decider->decide(req).discards;
            }

            DecisionRequest RequestDecisionSource::base_request(
                const ReadOnlyContext &ctx, const std::string &actor)
            {
                DecisionRequest req;
                req.actor = actor;
                req.catalog = ctx.catalog;
                req.view = make_view(ctx, actor);
                return req;
            }

            bool RequestDecisionSource::is_response_card(
                const ReadOnlyContext &ctx, const card::Card &c,
                card::ResponseKind kind)
            {
                return StateQuery::hand_card_matching(
                    ctx, c, [kind](const card::CardDef &def)
                    { return is_response_def(def, kind); });
            }

            bool RequestDecisionSource::is_rescue_card(
                const ReadOnlyContext &ctx, const card::Card &c, bool is_self)
            {
                return StateQuery::hand_card_matching(
                    ctx, c,
                    [is_self](const card::CardDef &def)
                    { return can_rescue_def(def, is_self); });
            }

            bool RequestDecisionSource::is_counter_card(
                const ReadOnlyContext &ctx, const card::Card &c)
            {
                return StateQuery::hand_card_matching(
                    ctx, c,
                    [](const card::CardDef &def) { return is_counter_def(def); });
            }

            void RequestDecisionSource::append_zone(
                std::vector<card::Card> &out,
                std::vector<card::Zone> &labels,
                const std::vector<card::Card> &zone, card::Zone label)
            {
                out.insert(out.end(), zone.begin(), zone.end());
                labels.insert(labels.end(), zone.size(), label);
            }
        }
    }
}
