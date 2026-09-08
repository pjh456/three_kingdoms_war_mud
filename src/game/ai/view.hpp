/**
 * @file view.hpp
 * @brief AiView：从某名玩家视角抽取的紧凑、只读局面观察。
 * @note 纯函数，不持有状态；决策只依赖它 + TurnContext，保证可回放/可存档。
 */

#ifndef INCLUDE_TKW_GAME_AI_VIEW_HPP
#define INCLUDE_TKW_GAME_AI_VIEW_HPP

#include <string>
#include <utility>
#include <vector>

#include "card/card.hpp"
#include "card/def.hpp"
#include "card/manager.hpp"
#include "game/core/context.hpp"
#include "game/query/distance.hpp"
#include "game/query/equip.hpp"

namespace tkw
{
    namespace game
    {
        namespace ai
        {
            /** @brief 对手视角条目。 */
            struct EnemyView
            {
                std::string id;
                int hp = 0;
                int max_hp = 0;
                int hand_size = 0;
                int equip_count = 0;
                bool has_weapon = false;
                int distance = 0;          /**< 自己到该角色的调整后距离 */
                bool in_attack_range = false; /**< 自己能否用杀够到 */
            };

            /** @brief 己方视角的完整观察。 */
            struct AiView
            {
                std::string self;
                int self_hp = 0;
                int self_max_hp = 0;
                std::vector<card::Card> hand;   /**< 自己的手牌（副本） */
                std::vector<EnemyView> others;  /**< 其他角色（创建序） */
            };

            /** @brief 构造 player 的观察（拷贝必要数据，不引用对局内部容器）。 */
            inline AiView make_view(const GameContext &ctx, const std::string &player)
            {
                AiView v;
                v.self = player;
                const auto me = ctx.entities->find(player);
                if (me.is_some())
                {
                    v.self_hp = me.unwrap()->get_hp();
                    v.self_max_hp = me.unwrap()->get_hp_bar().get_max();
                }
                v.hand = ctx.cards->hand(player);

                for (const auto &e : *ctx.entities)
                {
                    if (e->get_id() == player)
                        continue;
                    EnemyView ev;
                    ev.id = e->get_id();
                    ev.hp = e->get_hp();
                    ev.max_hp = e->get_hp_bar().get_max();
                    ev.hand_size = static_cast<int>(ctx.cards->hand_size(ev.id));
                    ev.equip_count = static_cast<int>(ctx.cards->equip_size(ev.id));
                    ev.has_weapon =
                        has_equip_slot(ctx, ev.id, card::EquipSlot::Weapon);
                    ev.distance = distance_between(ctx, player, ev.id);
                    ev.in_attack_range = in_attack_range(ctx, player, ev.id);
                    v.others.push_back(std::move(ev));
                }
                return v;
            }
        }
    }
}

#endif  // INCLUDE_TKW_GAME_AI_VIEW_HPP
