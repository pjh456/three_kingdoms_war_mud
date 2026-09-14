/**
 * @file   base.cpp
 * @brief  玩家实体状态入口与事件发布的定义。
 * @ingroup tkw_entity
 */

#include "entity/base.hpp"

#include <memory>
#include <string>
#include <utility>

namespace tkw
{
    namespace entity
    {
        Entity::Entity(
            std::string eid, int in_seat, Hp in_hp, EventBus &injected_bus,
            Gender in_gender, bool in_chained, std::string in_hero) :
            id(std::move(eid)),
            seat(in_seat),
            hp(std::move(in_hp)),
            bus(&injected_bus),
            gender(in_gender),
            hero(std::move(in_hero)),
            chained(in_chained)
        {
            bind_status_events();
        }

        int Entity::take_damage(
            const std::string &source, int amount, bool indirect,
            card::DamageType type)
        {
            if (amount <= 0)
                return 0;
            auto ev = std::make_shared<EntityDamagedEvent>();
            ev->source = source;
            ev->target = id;
            ev->amount = amount;
            ev->indirect = indirect;
            ev->damage_type = type;
            bus->publish(ev);

            return hp.sub(amount);
        }

        int Entity::heal(int amount)
        {
            if (amount <= 0)
                return 0;
            const int real = hp.add(amount);
            if (real <= 0)
                return 0;
            auto ev = std::make_shared<EntityHealedEvent>();
            ev->target = id;
            ev->amount = real;
            bus->publish(ev);
            return real;
        }

        void Entity::bind_status_events()
        {
            const std::string entity_id = id;
            EventBus *bus = this->bus;
            hp.on_change(
                [entity_id, bus](int old_cur, int cur, int max)
                {
                    auto ev = std::make_shared<EntityHpChangedEvent>();
                    ev->entity_id = entity_id;
                    ev->old_cur = old_cur;
                    ev->new_cur = cur;
                    ev->max = max;
                    bus->publish(ev);
                    // 不自动发 Died：体力变非正是濒死值状态，
                    // 救场窗口与死亡声明归 combat 流程
                });
        }
    }
}
