/**
 * @file   visibility.hpp
 * @brief  TUI 可见性红线：按 viewer 视角抽取牌区，对手手牌只出数量。
 * @details 只读展示层，不改变对局状态；与 `game/ai/view.hpp` 的手牌红线同口径
 *          （对手 `hand_size`、己方 `hand` 展开），供面板/快照共用，避免各处
 *          直接读 `CardManager::hand` 造成信息泄漏。
 * @ingroup tkw_tui
 */
#ifndef INCLUDE_TKW_TUI_VISIBILITY_HPP
#define INCLUDE_TKW_TUI_VISIBILITY_HPP

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "card/card.hpp"
#include "card/catalog.hpp"
#include "card/def.hpp"
#include "game/core/context.hpp"
#include "game/core/roles.hpp"

namespace tkw
{
    namespace tui
    {
        /**
         * @brief  一张牌的展示行：实例 id + 定义 id + 已解析中文名 + 花色点数。
         * @note   展示名由目录在构造时解析后落为纯值，本行不持目录指针。
         */
        struct CardRow
        {
            std::string instance_id;  /**< 对局内唯一实体牌 id。 */
            std::string def_id;       /**< 卡牌定义 id。 */
            std::string display_name; /**< `card::display_name` 结果。 */
            tkw::card::Suit suit = tkw::card::Suit::Spade; /**< 花色。 */
            int number = 1;                                /**< 点数（1~13）。 */

            bool operator==(const CardRow &) const = default; /**< 逐字段相等；@return 全等。 */
        };

        /**
         * @brief  一个牌区的可见视图：可展开的牌 + 真实张数。
         * @note   `count` 恒为区域真实张数；`cards` 仅在 `revealed` 为真时携带内容。
         */
        struct ZoneView
        {
            std::vector<CardRow> cards; /**< 可见实体牌；他人手牌恒为空。 */
            std::size_t count = 0;      /**< 区域真实张数（手牌 = 只出数量的红线）。 */
            bool revealed = false;      /**< `cards` 是否可展开（己方手牌/明置区）。 */

            bool operator==(const ZoneView &) const = default; /**< 逐字段相等；@return 全等。 */
        };

        /**
         * @brief  把一个明置牌区渲染为「卡名/卡名」；空区回落「无」。
         * @param[in] zone 牌区视图；须已展开，装备/判定经 `public_zone` 填充。
         * @return 斜杠分隔的中文展示名；`cards` 为空返回「无」。
         * @pre   `zone` 必须已展开。
         * @note  与 CLI status 的装备/判定口径一致。
         * @warning 不得传入未展开的对手手牌：`count > 0` 但 `cards` 空会误显「无」。
         */
        inline std::string zone_names(const ZoneView &zone)
        {
            if (zone.cards.empty())
                return "无";

            std::string out;
            for (std::size_t i = 0; i < zone.cards.size(); ++i)
            {
                if (i > 0)
                    out += "/";
                out += zone.cards[i].display_name;
            }
            return out;
        }

        /**
         * @brief  身份局角色是否向人类视角可见。
         * @param[in] humans 本会话真人座位 id 集合；空 = 无真人视角。
         * @param[in] over   对局是否已结束（终局全公开）。
         * @param[in] id     待判定角色的玩家 id。
         * @param[in] role   待判定角色的身份。
         * @return 无真人或终局 → 全可见；否则主公与真人座位自身可见，其余隐藏。
         * @retval true  该角色身份可展示。
         * @retval false 该角色身份须隐藏为未知。
         * @note   与 CLI status 的身份可见性同口径（主公 ∪ 真人 ∪ 终局），
         *         非身份局/全 AI 局不受影响。
         */
        inline bool role_visible(const std::vector<std::string> &humans, bool over,
                                 const std::string &id, tkw::game::Role role)
        {
            if (humans.empty() || over)
                return true;
            if (role == tkw::game::Role::Lord)
                return true;
            return std::find(humans.begin(), humans.end(), id) != humans.end();
        }

        /**
         * @brief  按 viewer 视角取 target 手牌：仅 viewer == target 时展开实体牌。
         * @param[in] ctx    只读上下文；`cards` 为空时返回空视图。
         * @param[in] viewer 观察者 id。
         * @param[in] target 手牌所属 id。
         * @return `count` 恒为真实手牌数；`viewer == target` 时 `cards` 为手牌副本且
         *         `revealed` 为真，否则 `cards` 为空且 `revealed` 为假。
         * @post  本接口不改变对局状态。
         * @warning 可见性红线单点：对手只取 `hand_size`，绝不拷贝 `hand()`。
         */
        inline ZoneView visible_hand(
            const tkw::game::ReadOnlyContext &ctx, const std::string &viewer,
            const std::string &target)
        {
            ZoneView view;
            if (ctx.cards == nullptr)
                return view;

            view.count = ctx.cards->hand_size(target);
            if (viewer != target)
                return view;

            view.revealed = true;
            view.cards.reserve(view.count);
            for (const auto &c : ctx.cards->hand(target))
                view.cards.push_back(CardRow{c.instance_id, c.def_id,
                                             tkw::card::display_name(
                                                 ctx.catalog, c.def_id),
                                             c.suit, c.number});
            return view;
        }

        /**
         * @brief  装备区/判定区明置：任何 viewer 均可展开。
         * @param[in] ctx  只读上下文（取目录解析展示名）。
         * @param[in] zone 待转换的实体牌区。
         * @return `cards` 为整区牌副本，`count` 为区大小，`revealed` 恒为真。
         */
        inline ZoneView public_zone(
            const tkw::game::ReadOnlyContext &ctx,
            const std::vector<tkw::card::Card> &zone)
        {
            ZoneView view;
            view.count = zone.size();
            view.revealed = true;
            view.cards.reserve(zone.size());
            for (const auto &c : zone)
                view.cards.push_back(CardRow{c.instance_id, c.def_id,
                                             tkw::card::display_name(
                                                 ctx.catalog, c.def_id),
                                             c.suit, c.number});
            return view;
        }
    }  // namespace tui
}  // namespace tkw

#endif  // INCLUDE_TKW_TUI_VISIBILITY_HPP
