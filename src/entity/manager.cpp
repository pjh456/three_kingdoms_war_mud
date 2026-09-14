/**
 * @file   manager.cpp
 * @brief  对局作用域实体容器 `EntityManager` 的定义。
 * @ingroup tkw_entity
 */

#include "entity/manager.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace tkw
{
    entity::EntityResult<entity::Entity *> EntityManager::create(
        std::string id, int seat, entity::Hp hp, entity::Gender gender,
        bool chained, std::string hero)
    {
        if (contains(id))
            return entity::EntityResult<entity::Entity *>::Err(
                entity::EntityError::DuplicateId);
        const std::size_t at = entities.size();
        entities.push_back(
            std::make_unique<entity::Entity>(
                std::move(id), seat, std::move(hp), *bus, gender, chained,
                std::move(hero)));
        index.emplace(entities[at]->get_id(), at);
        return entity::EntityResult<entity::Entity *>::Ok(entities[at].get());
    }

    Option<entity::Entity *> EntityManager::find(const std::string &id)
    {
        auto it = index.find(id);
        if (it == index.end())
            return Option<entity::Entity *>::None();
        return Option<entity::Entity *>::Some(entities[it->second].get());
    }

    Option<const entity::Entity *> EntityManager::find(
        const std::string &id) const
    {
        auto it = index.find(id);
        if (it == index.end())
            return Option<const entity::Entity *>::None();
        return Option<const entity::Entity *>::Some(entities[it->second].get());
    }

    bool EntityManager::contains(const std::string &id) const
    {
        return index.find(id) != index.end();
    }

    void EntityManager::remove(const std::string &id)
    {
        auto it = index.find(id);
        if (it == index.end())
            return;
        entities.erase(entities.begin() + std::ptrdiff_t(it->second));
        rebuild_index();
    }

    std::vector<EntitySnapshot> EntityManager::snapshot() const
    {
        std::vector<EntitySnapshot> out;
        out.reserve(entities.size());
        for (const auto &e : entities)
            out.push_back(EntitySnapshot{
                e->get_id(), e->get_seat(), e->get_hp(),
                e->get_hp_bar().get_max(), e->get_gender(), e->get_chained(),
                e->get_hero()});
        return out;
    }

    void EntityManager::restore(const std::vector<EntitySnapshot> &in)
    {
        clear();
        for (const auto &s : in)
        {
            entity::Hp hp = entity::Hp::make(s.max_hp);
            hp.set_cur(s.hp);
            (void)create(s.id, s.seat, std::move(hp), s.gender, s.chained,
                         s.hero);
        }
    }

    void EntityManager::clear()
    {
        entities.clear();
        index.clear();
    }

    std::vector<std::string> EntityManager::ordered_ids() const
    {
        std::vector<std::pair<int, std::string>> tmp;
        tmp.reserve(entities.size());
        for (const auto &e : entities)
            tmp.emplace_back(e->get_seat(), e->get_id());
        std::stable_sort(
            tmp.begin(), tmp.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });
        std::vector<std::string> ids;
        ids.reserve(tmp.size());
        for (auto &p : tmp)
            ids.push_back(std::move(p.second));
        return ids;
    }

    std::string EntityManager::next(const std::string &id) const
    {
        const auto ids = ordered_ids();
        if (ids.empty())
            return id;
        auto it = std::find(ids.begin(), ids.end(), id);
        if (it == ids.end())
            return ids.front();
        ++it;
        return it == ids.end() ? ids.front() : *it;
    }

    std::vector<std::string> EntityManager::order_from(
        const std::string &start) const
    {
        auto ids = ordered_ids();
        const auto it = std::find(ids.begin(), ids.end(), start);
        if (it != ids.end())
            std::rotate(ids.begin(), it, ids.end());
        return ids;
    }

    std::vector<const entity::Entity *> EntityManager::const_view() const
    {
        std::vector<const entity::Entity *> out;
        out.reserve(entities.size());
        for (const auto &e : entities)
            out.push_back(e.get());
        return out;
    }

    void EntityManager::rebuild_index()
    {
        index.clear();
        for (std::size_t i = 0; i < entities.size(); ++i)
            index.emplace(entities[i]->get_id(), i);
    }
}
