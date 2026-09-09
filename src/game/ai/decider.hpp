/**
 * @file decider.hpp
 * @brief 统一决策接缝：把 DecisionSource 的 8 个分散回调收敛成单个
 *        decide(DecisionRequest)，并附带只读观察与候选枚举。
 * @note 状态机只需实现 Decider::decide；RequestDecisionSource 负责从
 *       GameContext 抽取观察/候选并适配回引擎的 DecisionSource。请求只含
 *       只读数据（观察 + 候选 + 卡牌目录），故决策是纯函数、可回放、可单测。
 */

#ifndef INCLUDE_TKW_GAME_AI_DECIDER_HPP
#define INCLUDE_TKW_GAME_AI_DECIDER_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "card/card.hpp"
#include "card/catalog.hpp"
#include "card/def.hpp"
#include "game/ai/legal.hpp"
#include "game/ai/view.hpp"
#include "game/core/decision.hpp"
#include "game/resolve/combat.hpp"
#include "game/resolve/counter.hpp"
#include "game/resolve/response.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        namespace ai
        {
            /** @brief 决策点类别（与 DecisionSource 的回调一一对应）。 */
            enum class DecisionKind : std::uint8_t
            {
                Play,         /**< 出牌阶段选动作 */
                Response,     /**< 响应窗口（杀/闪） */
                Peach,        /**< 濒死救场 */
                Counter,      /**< 无懈窗口 */
                Trigger,      /**< 可选装备能力是否发动 */
                PickCard,     /**< 从目标区域选牌 */
                PickRevealed, /**< 从亮出的牌中选牌 */
                Discard,      /**< 弃牌阶段/能力代价 */
            };

            /** @brief 一次决策请求：类别 + 决策者 + 只读观察 + 候选。 */
            struct DecisionRequest
            {
                DecisionKind kind = DecisionKind::Play;
                std::string actor; /**< 决策者 id */
                AiView view;       /**< 决策者视角观察 */
                const card::CardDefCatalog *catalog = nullptr; /**< 只读卡牌目录 */

                // Play
                TurnContext turn;

                // Play / Response
                std::vector<LegalAction> legal; /**< Play 合法出牌动作；Response 两张当杀 pair 候选（丈八） */

                // Response / Peach / Counter / PickCard / PickRevealed / Discard
                std::vector<card::Card> options;

                // Response
                card::ResponseKind response_kind = card::ResponseKind::Sha;

                // Peach
                std::string dying;

                // Counter
                std::string counter_user;              /**< 锦囊使用者（延时判定窗口 = 空串哨兵） */
                std::vector<std::string> counter_targets; /**< 锦囊目标集合（判定窗口 = 被判定玩家一人） */

                // PickCard
                std::string target;
                std::vector<card::Zone> zone_labels; /**< PickCard：与 options 等长的来源分区标签；其余类别为空 */

                // Trigger
                card::Ability ability = card::Ability::NoShaLimit;

                // Discard
                int count = 0;
                DiscardReason discard_reason = DiscardReason::TurnLimit;
            };

            /** @brief 决策结果（按类别取用相应字段）。 */
            struct DecisionChoice
            {
                bool accepted = false; /**< Trigger：是否发动 */
                Option<std::string> instance_id = Option<std::string>::None();
                Option<card::Card> card = Option<card::Card>::None();
                std::vector<std::string> targets;  /**< Play：目标 */
                std::vector<std::string> discards; /**< Discard：要弃的牌 */
                std::string second_instance_id;   /**< Play/Response：第二张手牌（丈八蛇矛两张当杀；空 = 普通） */
            };

            /** @brief 状态机接口：实现单个 decide 即可接入引擎。 */
            class Decider
            {
            public:
                virtual ~Decider() = default;
                virtual DecisionChoice decide(const DecisionRequest &request) = 0;
            };

            /**
             * @class RequestDecisionSource
             * @brief 把 Decider 适配回引擎的 DecisionSource：负责从 GameContext
             *        抽取观察与候选，构造 DecisionRequest，再翻译 DecisionChoice。
             */
            class RequestDecisionSource : public DecisionSource
            {
            public:
                explicit RequestDecisionSource(Decider &decider) : decider_(&decider) {}

                Option<PlayAction> play_response(
                    const GameContext &ctx, const std::string &entity,
                    card::ResponseKind kind) override
                {
                    DecisionRequest req = base_request(ctx, entity);
                    req.kind = DecisionKind::Response;
                    req.response_kind = kind;
                    for (const auto &c : ctx.cards->hand(entity))
                        if (is_response_card(ctx, c, kind))
                            req.options.push_back(c);
                    // 杀响应窗口：携带两张当杀 pair 候选（与主动侧同一枚举口径）
                    if (kind == card::ResponseKind::Sha)
                        for (const auto &[first, second] :
                             two_cards_as_sha_pairs(ctx, entity))
                            req.legal.push_back(LegalAction{first, {}, second.instance_id});
                    const auto choice = decider_->decide(req);
                    if (choice.instance_id.is_none())
                        return Option<PlayAction>::None();
                    return Option<PlayAction>::Some(PlayAction{
                        choice.instance_id.unwrap(), {}, choice.second_instance_id});
                }

                Option<std::string> play_peach(
                    const GameContext &ctx, const std::string &saver,
                    const std::string &dying) override
                {
                    DecisionRequest req = base_request(ctx, saver);
                    req.kind = DecisionKind::Peach;
                    req.dying = dying;
                    for (const auto &c : ctx.cards->hand(saver))
                        if (is_rescue_card(ctx, c))
                            req.options.push_back(c);
                    return decider_->decide(req).instance_id;
                }

                Option<std::string> play_counter(
                    const GameContext &ctx, const std::string &player,
                    const std::string &trick_user,
                    const std::vector<std::string> &trick_targets) override
                {
                    DecisionRequest req = base_request(ctx, player);
                    req.kind = DecisionKind::Counter;
                    req.counter_user = trick_user;
                    req.counter_targets = trick_targets;
                    for (const auto &c : ctx.cards->hand(player))
                        if (is_counter_card(ctx, c))
                            req.options.push_back(c);
                    return decider_->decide(req).instance_id;
                }

                bool trigger_effect(
                    const GameContext &ctx, const std::string &player,
                    card::Ability ability) override
                {
                    DecisionRequest req = base_request(ctx, player);
                    req.kind = DecisionKind::Trigger;
                    req.ability = ability;
                    return decider_->decide(req).accepted;
                }

                Option<card::Card> pick_card_from_target(
                    const GameContext &ctx, const std::string &source,
                    const std::string &target) override
                {
                    DecisionRequest req = base_request(ctx, source);
                    req.kind = DecisionKind::PickCard;
                    req.target = target;
                    append_zone(
                        req.options, req.zone_labels, ctx.cards->hand(target),
                        card::Zone::Hand);
                    append_zone(
                        req.options, req.zone_labels, ctx.cards->equip(target),
                        card::Zone::Equip);
                    append_zone(
                        req.options, req.zone_labels, ctx.cards->judge(target),
                        card::Zone::Judge);
                    return decider_->decide(req).card;
                }

                Option<card::Card> pick_from_revealed(
                    const GameContext &ctx, const std::string &player,
                    const std::vector<card::Card> &options) override
                {
                    DecisionRequest req = base_request(ctx, player);
                    req.kind = DecisionKind::PickRevealed;
                    req.options = options;
                    return decider_->decide(req).card;
                }

                Option<PlayAction> choose_play(
                    const GameContext &ctx, const TurnContext &turn) override
                {
                    DecisionRequest req = base_request(ctx, turn.player);
                    req.kind = DecisionKind::Play;
                    req.turn = turn;
                    req.legal = legal_actions(ctx, turn.player, turn);
                    const auto choice = decider_->decide(req);
                    if (choice.instance_id.is_none())
                        return Option<PlayAction>::None();
                    return Option<PlayAction>::Some(PlayAction{
                        choice.instance_id.unwrap(), choice.targets,
                        choice.second_instance_id});
                }

                std::vector<std::string> choose_discards(
                    const GameContext &ctx, const std::string &player, int count,
                    DiscardReason reason) override
                {
                    DecisionRequest req = base_request(ctx, player);
                    req.kind = DecisionKind::Discard;
                    req.count = count;
                    req.discard_reason = reason;
                    req.options = ctx.cards->hand(player);
                    return decider_->decide(req).discards;
                }

            private:
                Decider *decider_;

                static DecisionRequest base_request(
                    const GameContext &ctx, const std::string &actor)
                {
                    DecisionRequest req;
                    req.actor = actor;
                    req.catalog = ctx.catalog;
                    req.view = make_view(ctx, actor);
                    return req;
                }

                static bool is_response_card(
                    const GameContext &ctx, const card::Card &c,
                    card::ResponseKind kind)
                {
                    const auto def = ctx.catalog->find(c.def_id);
                    return def.is_some() && is_response_def(*def.unwrap(), kind);
                }

                static bool is_rescue_card(const GameContext &ctx, const card::Card &c)
                {
                    const auto def = ctx.catalog->find(c.def_id);
                    return def.is_some() && is_rescue_def(*def.unwrap());
                }

                static bool is_counter_card(
                    const GameContext &ctx, const card::Card &c)
                {
                    const auto def = ctx.catalog->find(c.def_id);
                    return def.is_some() && is_counter_def(*def.unwrap());
                }

                /** @brief 追加一个区域的候选牌，并为每张牌记录来源分区。 */
                static void append_zone(
                    std::vector<card::Card> &out,
                    std::vector<card::Zone> &labels,
                    const std::vector<card::Card> &zone, card::Zone label)
                {
                    out.insert(out.end(), zone.begin(), zone.end());
                    labels.insert(labels.end(), zone.size(), label);
                }
            };
        }
    }
}

#endif  // INCLUDE_TKW_GAME_AI_DECIDER_HPP
