/**
 * @file   state.cpp
 * @brief  对局状态原语与只读状态谓词的定义。
 * @details 承载 `StateQuery` 的判定触发/目录查找/连环查询与 `StateOps` 的 12 个
 *          状态写方法；单表达式谓词与函数模板保留在 `state.hpp` 内联。
 * @ingroup tkw_game_core
 */

#include "game/core/state.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace tkw
{
    namespace game
    {
        bool StateQuery::judge_triggered(card::JudgeTrigger t, const card::Card &c)
        {
            switch (t)
            {
            case card::JudgeTrigger::Red:
                return is_red_suit(c.suit);
            case card::JudgeTrigger::Black:
                return is_black_suit(c.suit);
            case card::JudgeTrigger::Heart:
                return c.suit == card::Suit::Heart;
            case card::JudgeTrigger::NotHeart:
                return c.suit != card::Suit::Heart;
            case card::JudgeTrigger::Spade2to9:
                return c.suit == card::Suit::Spade && c.number >= 2 && c.number <= 9;
            case card::JudgeTrigger::NotClub:
                return c.suit != card::Suit::Club;
            }
            return false;
        }

        const card::CardDef *StateQuery::def_of(
            const ReadOnlyContext &ctx, const std::string &def_id)
        {
            if (!ctx.catalog)
                return nullptr;
            const auto def = ctx.catalog->find(def_id);
            return def.is_some() ? def.unwrap() : nullptr;
        }

        bool StateQuery::is_chained(const ReadOnlyContext &ctx, const std::string &id)
        {
            const auto e = ctx.entities->find(id);
            return e.is_some() && e.unwrap()->get_chained();
        }

        void StateOps::apply_heal(const std::string &target, int amount)
        {
            const auto e = m_ctx.entities->find(target);
            if (e.is_some())
                e.unwrap()->heal(amount);
        }

        void StateOps::set_chained(const std::string &id, bool chained)
        {
            const auto e = m_ctx.entities->find(id);
            if (e.is_some())
                e.unwrap()->set_chained(chained);
        }

        void StateOps::apply_equip_lost(
            const std::string &owner, const card::Card &card)
        {
            const card::CardDef *def = StateQuery::def_of(m_ctx, card.def_id);
            if (def == nullptr)
                return;
            if (std::find(def->abilities.begin(), def->abilities.end(),
                          card::Ability::SilverLion) == def->abilities.end())
                return;
            apply_heal(owner, 1);
        }

        int StateOps::consume_jiu_sha_bonus(const std::string &attacker)
        {
            if (m_ctx.jiu_damage_owner != attacker)
                return 0;
            m_ctx.jiu_damage_owner.clear();
            return 1;
        }

        Option<card::Card> StateOps::draw_with_refill()
        {
            if (m_ctx.cards->draw_size() == 0)
            {
                if (m_ctx.cards->discard_size() == 0)
                    return Option<card::Card>::None();
                if (m_ctx.rng)
                    m_ctx.cards->refill_draw(*m_ctx.rng);
            }
            return m_ctx.cards->draw();
        }

        int StateOps::apply_draw(
            const std::string &player, int count, DrawKind kind)
        {
            int drew = 0;
            for (int i = 0; i < count; ++i)
            {
                auto c = draw_with_refill();
                if (c.is_none())
                    break;
                card::Card card = std::move(c).unwrap();
                m_ctx.cards->add_to_hand(player, card);
                emit_card_drawn(m_ctx, player, card, kind);
                ++drew;
            }
            return drew;
        }

        bool StateOps::remove_card_from_zones(
            const std::string &entity_id, const std::string &instance_id,
            card::Card &out, Zone *from_zone)
        {
            auto c = m_ctx.cards->remove_from_any(entity_id, instance_id, from_zone);
            if (c.is_none())
                return false;
            out = std::move(c).unwrap();
            return true;
        }

        void StateOps::discard_and_emit(
            const std::string &owner, const card::Card &c, DiscardKind kind)
        {
            m_ctx.cards->discard(c);
            emit_card_discarded(m_ctx, owner, c, kind);
        }

        Option<card::Card> StateOps::remove_and_discard(
            const std::string &owner, const std::string &instance_id)
        {
            auto removed = m_ctx.cards->remove_from_hand(owner, instance_id);
            if (removed.is_none())
                return Option<card::Card>::None();
            card::Card card = std::move(removed).unwrap();
            discard_and_emit(owner, card);
            return Option<card::Card>::Some(std::move(card));
        }

        Option<card::Card> StateOps::remove_any_and_discard(
            const std::string &owner, const std::string &instance_id)
        {
            card::Card card;
            Zone from = Zone::Limbo;
            if (!remove_card_from_zones(owner, instance_id, card, &from))
                return Option<card::Card>::None();
            discard_and_emit(owner, card);

            // 装备区失去触发（过河/寒冰选装备区时）：弃置后结算回血
            if (from == Zone::Equip)
                apply_equip_lost(owner, card);
            return Option<card::Card>::Some(std::move(card));
        }

        Option<card::Card> StateOps::perform_judgement()
        {
            return draw_with_refill();
        }

        Option<card::Card> StateOps::resolve_target_pick(
            const std::string &target, const TargetPick &pick)
        {
            if (pick.card.is_some())
                return pick.card;

            const auto hand = m_ctx.cards->hand(target);
            if (hand.empty())
                return Option<card::Card>::None();

            std::size_t i = pick.index < hand.size() ? pick.index : 0;
            if (m_ctx.rng)
                i = uniform_below(*m_ctx.rng, static_cast<std::uint32_t>(hand.size()));
            return Option<card::Card>::Some(hand[i]);
        }
    }
}
