/**
 * @file   card_event.cpp
 * @brief  卡牌域事件发布辅助的定义。
 * @details 实现摸牌/打出/弃置/区域转移四个 `emit_card_*`；事件类型与总线模板
 *          保留在头内，默认实参只在 `card_event.hpp` 声明处出现一次。
 * @ingroup tkw_game_core
 */

#include "game/core/card_event.hpp"

#include <memory>
#include <string>

namespace tkw
{
    namespace game
    {
        void emit_card_drawn(
            GameContext &ctx, const std::string &entity, const card::Card &c,
            DrawKind kind)
        {
            if (!ctx.bus)
                return;
            auto ev = std::make_shared<CardDrawnEvent>();
            ev->entity = entity;
            ev->instance_id = c.instance_id;
            ev->def_id = c.def_id;
            ev->kind = kind;
            ctx.bus->publish(ev);
        }

        void emit_card_played(
            GameContext &ctx, const std::string &user, const card::Card &c)
        {
            if (!ctx.bus)
                return;
            auto ev = std::make_shared<CardPlayedEvent>();
            ev->user = user;
            ev->instance_id = c.instance_id;
            ev->def_id = c.def_id;
            ctx.bus->publish(ev);
        }

        void emit_card_discarded(
            GameContext &ctx, const std::string &entity, const card::Card &c,
            DiscardKind kind)
        {
            if (!ctx.bus)
                return;
            auto ev = std::make_shared<CardDiscardedEvent>();
            ev->entity = entity;
            ev->instance_id = c.instance_id;
            ev->def_id = c.def_id;
            ev->kind = kind;
            ctx.bus->publish(ev);
        }

        void emit_card_moved(
            GameContext &ctx, const std::string &from_entity,
            const std::string &to_entity, const card::Card &c, Zone from, Zone to)
        {
            if (!ctx.bus)
                return;
            auto ev = std::make_shared<CardMovedEvent>();
            ev->from_entity = from_entity;
            ev->to_entity = to_entity;
            ev->instance_id = c.instance_id;
            ev->def_id = c.def_id;
            ev->from = from;
            ev->to = to;
            ctx.bus->publish(ev);
        }
    }
}
