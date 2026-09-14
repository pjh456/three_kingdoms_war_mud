/**
 * @file   view.cpp
 * @brief  AiView 构造与视角角色查询的定义。
 * @ingroup tkw_game_ai
 */

#include "game/ai/view.hpp"

#include <string>
#include <utility>
#include <vector>

namespace tkw
{
    namespace game
    {
        namespace ai
        {
            Role role_in_view(const AiView &view, const std::string &id)
            {
                if (id == view.self)
                    return view.self_role;
                for (const auto &e : view.others)
                    if (e.id == id)
                        return e.role;
                return Role::None;
            }

            AiView make_view(const ReadOnlyContext &ctx, const std::string &player)
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
                        EquipQuery::has_equip_slot(ctx, ev.id, card::EquipSlot::Weapon);
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
