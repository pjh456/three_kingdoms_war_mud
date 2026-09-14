/**
 * @file   response.cpp
 * @brief  响应窗口的函数体定义。
 * @details 实现 `response.hpp` 声明的响应牌存在性与消费；消费的牌移除+弃置+
 *          事件语义由状态原语完成。
 * @ingroup tkw_game_resolve
 */

#include "game/resolve/response.hpp"

#include <string>

namespace tkw
{
    namespace game
    {
        bool has_response_card(
            const GameContext &ctx, const std::string &entity_id, card::ResponseKind kind)
        {
            if (StateQuery::any_hand_card_matching(
                    ctx, entity_id,
                    [kind](const card::CardDef &def)
                    { return is_response_def(def, kind); }))
                return true;
            if (kind != card::ResponseKind::Sha)
                return !HeroQuery::jink_conversion_cards(ctx, entity_id).empty();
            if (EquipQuery::has_ability(ctx, entity_id, card::Ability::TwoCardsAsSha) &&
                ctx.cards->hand_size(entity_id) >= 2)
                return true;
            return !HeroQuery::sha_conversion_cards(ctx, entity_id).empty();
        }

        Option<card::Card> consume_response(
            GameContext &ctx, DecisionSource &ai,
            const std::string &entity_id, card::ResponseKind kind,
            const ResponsePrompt &prompt)
        {
            if (!has_response_card(ctx, entity_id, kind))
                return Option<card::Card>::None();
            const auto chosen = ai.play_response(ctx, entity_id, kind, prompt);
            if (chosen.is_none())
                return Option<card::Card>::None();

            return StateOps(ctx).consume_hand_card_matching(entity_id, chosen.unwrap().instance_id,
                [&ctx, &entity_id, kind](const card::CardDef &def,
                                         const card::Card &c)
                {
                    return is_response_def(def, kind) ||
                           (kind == card::ResponseKind::Jink &&
                            HeroQuery::can_convert_card_to_jink(ctx, entity_id, c, def));
                },
                DiscardKind::Response);
        }
    }
}
