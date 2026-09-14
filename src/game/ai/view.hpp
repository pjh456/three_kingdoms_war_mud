/**
 * @file   view.hpp
 * @brief  AiView：从某名玩家视角抽取的紧凑、只读局面观察。
 * @details 纯函数构造，不持有状态；决策只依赖它 + TurnContext，保证可回放/可存档。
 *          本层属只读 AI：只拷贝必要数据，不引用也不修改对局内部容器。
 * @ingroup tkw_game_ai
 */

#ifndef INCLUDE_TKW_GAME_VIEW_HPP
#define INCLUDE_TKW_GAME_VIEW_HPP

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
            /** @brief 对手视角条目（手牌仅可见数量，装备/判定区明置）。 */
            struct EnemyView
            {
                std::string id;              /**< 对手玩家 id */
                int seat = 0;                /**< 座位号 */
                int hp = 0;                  /**< 当前体力 */
                int max_hp = 0;              /**< 体力上限 */
                int hand_size = 0;           /**< 手牌数量（内容不可见） */
                int equip_count = 0;         /**< 装备区牌数 */
                bool has_weapon = false;     /**< 装备区是否含武器 */
                int distance = 0;          /**< 自己到该角色的调整后距离 */
                bool in_attack_range = false; /**< 自己能否用杀够到 */
                std::vector<card::Card> equip; /**< 装备区（明置，副本） */
                std::vector<card::Card> judge; /**< 判定区（明置，副本） */
                Role role = Role::None;        /**< 身份局角色（乱斗/未分配为 None） */
            };

            /** @brief 己方视角的完整观察。 */
            struct AiView
            {
                std::string self;      /**< 自己玩家 id */
                int self_seat = 0;     /**< 自己座位号 */
                int self_hp = 0;       /**< 自己当前体力 */
                int self_max_hp = 0;   /**< 自己体力上限 */
                std::vector<card::Card> hand;  /**< 自己的手牌（副本） */
                std::vector<card::Card> equip; /**< 自己的装备区（副本） */
                std::vector<card::Card> judge;  /**< 自己的判定区（副本） */
                std::vector<EnemyView> others; /**< 其他角色（创建序） */
                GameMode mode = GameMode::Brawl; /**< 对局模式（乱斗/未绑定为 Brawl） */
                Role self_role = Role::None;   /**< 自己的角色（乱斗/未分配为 None） */
            };

            /**
             * @brief 取某角色在观察中的角色；自己走 self_role，其他走 others 条目。
             * @param[in] view 观察。
             * @param[in] id   玩家 id。
             * @return 命中返回对应角色；不在观察中返回 Role::None。
             * @retval Role::None `id` 既非自己也不在 `others` 中。
             * @post 本接口不改变任何状态。
             */
            inline Role role_in_view(const AiView &view, const std::string &id)
            {
                if (id == view.self)
                    return view.self_role;
                for (const auto &e : view.others)
                    if (e.id == id)
                        return e.role;
                return Role::None;
            }

            /**
             * @brief 构造 player 的观察（拷贝必要数据，不引用对局内部容器）。
             * @param[in] ctx    只读容器视图（不含 EventBus/Rng）。
             * @param[in] player 观察者玩家 id。
             * @return `player` 视角的紧凑观察副本。
             * @post 不改变对局状态；返回值为独立副本，调用方可保留。
             */
            inline AiView make_view(const ReadOnlyContext &ctx, const std::string &player)
            {
                AiView v;
                v.self = player;
                v.mode = mode_of(ctx);
                v.self_role = role_of(ctx.roles, player);
                const auto me = ctx.entities->find(player);
                if (me.is_some())
                {
                    v.self_seat = me.unwrap()->get_seat();
                    v.self_hp = me.unwrap()->get_hp();
                    v.self_max_hp = me.unwrap()->get_hp_bar().get_max();
                }
                v.hand = ctx.cards->hand(player);
                v.equip = ctx.cards->equip(player);
                v.judge = ctx.cards->judge(player);

                for (const auto *e : ctx.entities->const_view())
                {
                    if (e->get_id() == player)
                        continue;
                    EnemyView ev;
                    ev.id = e->get_id();
                    ev.seat = e->get_seat();
                    ev.role = role_of(ctx.roles, ev.id);
                    ev.hp = e->get_hp();
                    ev.max_hp = e->get_hp_bar().get_max();
                    ev.hand_size = static_cast<int>(ctx.cards->hand_size(ev.id));
                    ev.equip_count = static_cast<int>(ctx.cards->equip_size(ev.id));
                    ev.has_weapon =
                        has_equip_slot(ctx, ev.id, card::EquipSlot::Weapon);
                    ev.distance =
                        DistanceQuery::distance_between(ctx, player, ev.id);
                    ev.in_attack_range =
                        DistanceQuery::in_attack_range(ctx, player, ev.id);
                    ev.equip = ctx.cards->equip(ev.id);
                    ev.judge = ctx.cards->judge(ev.id);
                    v.others.push_back(std::move(ev));
                }
                return v;
            }
        }
    }
}

#endif  // INCLUDE_TKW_GAME_VIEW_HPP
