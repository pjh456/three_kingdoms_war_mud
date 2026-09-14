/**
 * @file   card.cpp
 * @brief  实体牌堆 `CardStack` 的存取与洗牌定义。
 * @ingroup tkw_card
 */

#include "card/card.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace tkw
{
    namespace card
    {
        Option<Card> CardStack::pop()
        {
            if (m_cards.empty())
                return Option<Card>::None();
            Card c = std::move(m_cards.back());
            m_cards.pop_back();
            return Option<Card>::Some(std::move(c));
        }

        Option<const Card *> CardStack::top() const
        {
            if (m_cards.empty())
                return Option<const Card *>::None();
            return Option<const Card *>::Some(&m_cards.back());
        }

        Option<Card> CardStack::remove(const std::string &instance_id)
        {
            for (auto it = m_cards.begin(); it != m_cards.end(); ++it)
            {
                if (it->instance_id == instance_id)
                {
                    Card c = std::move(*it);
                    m_cards.erase(it);
                    return Option<Card>::Some(std::move(c));
                }
            }
            return Option<Card>::None();
        }

        void CardStack::shuffle(Rng &rng)
        {
            for (std::size_t i = m_cards.size(); i > 1; --i)
            {
                const std::size_t j =
                    uniform_below(rng, static_cast<std::uint32_t>(i));
                std::swap(m_cards[i - 1], m_cards[j]);
            }
        }
    }
}
