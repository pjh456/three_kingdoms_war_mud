/**
 * @file   card.hpp
 * @brief  实体牌（运行时）与牌堆（摸牌堆/弃牌堆）。
 * @details 与 `CardDef` 的区别：`CardDef` 是「卡牌定义」（目录里的一张），
 *          `Card` 是牌堆里的一张实体牌（携带唯一 `instance_id` + 花色点数）。
 * @note   本文件不含任何规则/事件：堆满不自动洗回、不发布事件，规则归 gameplay。
 * @ingroup tkw_card
 */

#ifndef INCLUDE_TKW_CARD_CARD_HPP
#define INCLUDE_TKW_CARD_CARD_HPP

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "card/def.hpp"
#include "util/rng.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace card
    {
        /**
         * @class Card
         * @brief 实体牌：由牌堆构建按 CardDef.copies 逐张生成。
         * @note 只携带 def_id（引用 CardDefCatalog 中的定义）+ 花色点数；
         *       需要 type/effect/equip 时经 catalog 解析，本类不持有定义。
         */
        struct Card
        {
            std::string instance_id; /**< 对局内唯一（构建时分配，形如 "sha#12"） */
            std::string def_id;      /**< 卡牌定义 id（= 卡牌文件名） */
            Suit suit = Suit::Spade; /**< 花色。 */
            int number = 1;          /**< 点数（1~13）。 */

            bool operator==(const Card &) const = default; /**< 逐字段相等；@return 全等。 */
        };

        /**
         * @class CardStack
         * @brief 牌堆容器：vector 尾 = 堆顶。
         * @note 不自动洗回弃牌堆：摸空返回 None，由 gameplay 决定补牌规则。
         * @warning 不可拷贝：牌堆唯一持有牌序列；仅可移动。
         */
        class CardStack
        {
        public:
            /** @brief 构造空牌堆。 */
            CardStack() = default;
            CardStack(const CardStack &) = delete; /**< 不可拷贝。 */
            CardStack &operator=(const CardStack &) = delete; /**< 不可拷贝赋值。 */
            CardStack(CardStack &&) noexcept = default; /**< 移动构造；源对象变为空。 */
            CardStack &operator=(CardStack &&) noexcept = default; /**< 移动赋值；@return 自身引用。 */

            /**
             * @brief  当前牌数。
             * @return 牌序列中的张数。
             */
            std::size_t size() const noexcept { return cards.size(); }

            /**
             * @brief  是否为空。
             * @return `true` = 无牌。
             */
            bool empty() const noexcept { return cards.empty(); }

            /**
             * @brief  底层牌序列（堆底→堆顶）只读视图，供快照/存档。
             * @return 引用指向内部序列，生命周期同本对象。
             */
            const std::vector<Card> &view() const noexcept { return cards; }

            /**
             * @brief  置顶。
             * @param[in] card 要放入堆顶的牌。
             * @note   弃牌/置入牌堆底语义由调用方决定。
             */
            void push(Card card) { cards.push_back(std::move(card)); }

            /**
             * @brief  取顶。
             * @return 堆顶牌；`None` = 空堆。
             * @retval Some 已从堆中移除的牌。
             * @retval None 空堆，不做任何改变。
             */
            Option<Card> pop()
            {
                if (cards.empty())
                    return Option<Card>::None();
                Card c = std::move(cards.back());
                cards.pop_back();
                return Option<Card>::Some(std::move(c));
            }

            /**
             * @brief  看顶（不取走）。
             * @return 堆顶指针；`None` = 空堆。
             * @retval Some 指针在下次 `pop`/`push`/`shuffle` 前稳定。
             * @retval None 空堆。
             */
            Option<const Card *> top() const
            {
                if (cards.empty())
                    return Option<const Card *>::None();
                return Option<const Card *>::Some(&cards.back());
            }

            /**
             * @brief  按 `instance_id` 移除一张牌（结算回滚用）。
             * @param[in] instance_id 目标实体牌 id。
             * @return 被移除的牌；`None` = 未找到。
             * @retval Some 已从堆中移除该牌。
             * @retval None 堆中无此 `instance_id`。
             */
            Option<Card> remove(const std::string &instance_id)
            {
                for (auto it = cards.begin(); it != cards.end(); ++it)
                {
                    if (it->instance_id == instance_id)
                    {
                        Card c = std::move(*it);
                        cards.erase(it);
                        return Option<Card>::Some(std::move(c));
                    }
                }
                return Option<Card>::None();
            }

            /**
             * @brief  Fisher–Yates 原地洗牌。
             * @param[in,out] rng 随机源；被调用以产生全部交换。
             */
            void shuffle(Rng &rng)
            {
                for (std::size_t i = cards.size(); i > 1; --i)
                {
                    const std::size_t j =
                        uniform_below(rng, static_cast<std::uint32_t>(i));
                    std::swap(cards[i - 1], cards[j]);
                }
            }

        private:
            std::vector<Card> cards;
        };
    }
}

#endif  // INCLUDE_TKW_CARD_CARD_HPP