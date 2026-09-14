/**
 * @file   manager.hpp
 * @brief  对局作用域的实体容器与 id 索引。
 * @details 容器按创建序保存实体，另维护 id → 下标索引；总线由拥有它的上下文注入，
 *          `create` 出的实体绑定该总线。
 * @ingroup tkw_entity
 */

#ifndef INCLUDE_TKW_ENTITY_MANAGER_HPP
#define INCLUDE_TKW_ENTITY_MANAGER_HPP

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "entity/base.hpp"
#include "entity/error.hpp"
#include "entity/hp.hpp"
#include "event/event_bus.hpp"
#include "util/types.hpp"

namespace tkw
{
    /** @brief 实体快照：身份 + 座位 + 体力 + 性别 + 连环状态 + 武将 id（按创建序导出）。 */
    struct EntitySnapshot
    {
        std::string id;                               /**< 实体 id。 */
        int seat = 0;                                 /**< 座位号。 */
        int hp = 0;                                   /**< 当前体力（可为非正 = 濒死值状态）。 */
        int max_hp = 0;                               /**< 体力上限。 */
        entity::Gender gender = entity::Gender::Male; /**< 性别。 */
        bool chained = false;                         /**< 是否处于连环状态。 */
        std::string hero;                             /**< 武将 id；空 = 无名/通用座位。 */
    };

    /**
     * @brief  对局作用域的实体容器 + id 索引。
     * @details 总线由拥有它的上下文注入（须比本管理器存活更久），`create` 出的实体
     *          绑定该总线。创建顺序 = 容器迭代序；座位序/回合序请用
     *          `ordered_ids()`/`next()`/`order_from()`（按 `seat` 排序，与创建序
     *          解耦）；`find` 为 O(1)。本类不关心敌我关系与死亡规则：死实体由调用方
     *          在安静时刻（如回合结算后）显式 `remove`，避免事件分发中途改动容器。
     * @note   不可拷贝、不可移动：须原地锚定在对局运行时内。
     * @warning 实体 id 创建后不可变，且须在对局内唯一；索引在各突变接口内同步维护，
     *          调用方不得在迭代中途 `create`/`remove`。
     */
    class EntityManager
    {
    public:
        /**
         * @brief  构造管理器并绑定事件总线。
         * @param[in] injected_bus 事件总线；必须比本管理器及其中实体存活更久。
         * @warning 总线存活时长不足会使实体发布事件时悬空。
         */
        explicit EntityManager(EventBus &injected_bus) : bus(&injected_bus) {}

        EntityManager(const EntityManager &) = delete;
        EntityManager &operator=(const EntityManager &) = delete;
        EntityManager(EntityManager &&) = delete;
        EntityManager &operator=(EntityManager &&) = delete;

        /**
         * @brief  注册新玩家实体（绑定到注入总线）。
         * @param[in] id      实体 id；对局内唯一，创建后不可变。
         * @param[in] seat    座位号（距离计算与回合序的基础）。
         * @param[in] hp      初始血条（体力/上限）。
         * @param[in] gender  性别（缺省 `Male`）。
         * @param[in] chained 初始连环状态（缺省 `false`，未横置）。
         * @param[in] hero    武将 id（缺省空 = 无名/通用座位）。
         * @return 创建结果。
         * @retval Ok  实体指针（与 `find` 同稳定性：未 `remove` 前有效）。
         * @retval Err(`EntityError::DuplicateId`) 层内已存在相同 id。
         * @post   成功时新实体位于创建序末尾，索引同步且 `size()` 增一。
         */
        entity::EntityResult<entity::Entity *> create(
            std::string id, int seat, entity::Hp hp,
            entity::Gender gender = entity::Gender::Male,
            bool chained = false,
            std::string hero = {})
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

        /**
         * @brief  O(1) 按 id 查询（可写容器）。
         * @param[in] id 目标实体 id。
         * @return 查询结果。
         * @retval Some 指向可变实体的指针；未 `remove` 前有效。
         * @retval None 不存在该 id。
         * @note   与 const 重载按 `this` 的 cv 选择：非 const 容器得到可变实体，
         *         const 容器得到只读实体。
         */
        Option<entity::Entity *> find(const std::string &id)
        {
            auto it = index.find(id);
            if (it == index.end())
                return Option<entity::Entity *>::None();
            return Option<entity::Entity *>::Some(entities[it->second].get());
        }

        /**
         * @brief  O(1) 按 id 查询（只读容器）。
         * @param[in] id 目标实体 id。
         * @return 查询结果。
         * @retval Some 指向只读实体的指针；未 `remove` 前有效。
         * @retval None 不存在该 id。
         */
        Option<const entity::Entity *> find(const std::string &id) const
        {
            auto it = index.find(id);
            if (it == index.end())
                return Option<const entity::Entity *>::None();
            return Option<const entity::Entity *>::Some(entities[it->second].get());
        }

        /**
         * @brief  判断是否存在指定 id 的实体。
         * @param[in] id 目标实体 id。
         * @return `true` 表示存在；本查询为 O(1)。
         */
        bool contains(const std::string &id) const
        {
            return index.find(id) != index.end();
        }

        /**
         * @brief  按 id 移除（不存在时幂等无操作）。
         * @param[in] id 目标实体 id。
         * @post   被移除实体的指针失效；其余实体的 `Entity*` 不受影响
         *         （堆对象不移动），索引重建。
         * @note   应在事件分发之外的安静时刻调用，避免迭代中改动容器。
         */
        void remove(const std::string &id)
        {
            auto it = index.find(id);
            if (it == index.end())
                return;
            entities.erase(entities.begin() + std::ptrdiff_t(it->second));
            rebuild_index();
        }

        /**
         * @brief  返回当前实体数量。
         * @return 容器内实体个数。
         */
        std::size_t size() const noexcept { return entities.size(); }

        /**
         * @brief  判断容器是否为空。
         * @return `true` 表示无实体。
         */
        bool empty() const noexcept { return entities.empty(); }

        /**
         * @brief  按创建序导出快照。
         * @return 快照列表，含体力/上限、性别、连环状态与武将 id；顺序 = 创建序。
         */
        std::vector<EntitySnapshot> snapshot() const
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

        /**
         * @brief  清空后按快照顺序重建。
         * @param[in] in 快照列表；id/座位/体力/性别/连环状态/武将原样恢复。
         * @post   原实体与索引全部丢弃，实体按 `in` 顺序重建并绑定当前总线；
         *         `in` 中重复 id 会被 `create` 拒绝而跳过。
         */
        void restore(const std::vector<EntitySnapshot> &in)
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

        /**
         * @brief  清空全部实体与索引。
         * @post   所有 `Entity*` 失效。
         */
        void clear()
        {
            entities.clear();
            index.clear();
        }

        /**
         * @brief  按座位序返回全部实体 id。
         * @return id 列表，按 `seat` 升序、同座位按创建序稳定。
         * @note   座位是回合序与距离的唯一事实源：创建顺序与座位号不一致时，
         *         回合序仍以座位为准。
         */
        std::vector<std::string> ordered_ids() const
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

        /**
         * @brief  座位序下家（环绕）。
         * @param[in] id 当前实体 id。
         * @return 下一个实体 id。
         * @retval 下一个 id  `id` 在集合内且非末位。
         * @retval 首个 id    `id` 在末位（环绕）或不在集合内。
         * @note   集合为空时原样返回 `id`。
         */
        std::string next(const std::string &id) const
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

        /**
         * @brief  从 `start` 起按座位序环绕的 id 列表。
         * @param[in] start 起始实体 id。
         * @return 以 `start` 开头、按座位序环绕的 id 列表。
         * @retval 原序列表  `start` 不在集合内。
         */
        std::vector<std::string> order_from(const std::string &start) const
        {
            auto ids = ordered_ids();
            const auto it = std::find(ids.begin(), ids.end(), start);
            if (it != ids.end())
                std::rotate(ids.begin(), it, ids.end());
            return ids;
        }

        /**
         * @brief  按创建序导出只读实体视图（副本）。
         * @return 指向各实体的 `const` 指针列表；未 `remove` 前有效。
         * @note   直接迭代 `unique_ptr` 容器经 `operator->` 仍会泄漏可变
         *         `Entity*`；只读路径用本视图取 `const` 实体。
         */
        std::vector<const entity::Entity *> const_view() const
        {
            std::vector<const entity::Entity *> out;
            out.reserve(entities.size());
            for (const auto &e : entities)
                out.push_back(e.get());
            return out;
        }

        /**
         * @brief  按创建序迭代（仅供写层使用）。
         * @return 指向容器首元素的迭代器；与座位序解耦（座位序用
         *         `ordered_ids()`/`next()`/`order_from()`）。
         * @warning 有意不提供 const 重载：const 管理器经 `unique_ptr` 迭代仍会得到
         *          可变 `Entity*`，故只读路径一律走 `const_view()`，从类型上封住出口。
         */
        auto begin() noexcept { return entities.begin(); }

        /**
         * @brief  返回按创建序迭代的结束哨兵。
         * @return 指向容器末后位置的迭代器。
         */
        auto end() noexcept { return entities.end(); }

    private:
        EventBus *bus;
        std::vector<std::unique_ptr<entity::Entity>> entities;
        std::unordered_map<std::string, std::size_t> index;

        void rebuild_index()
        {
            index.clear();
            for (std::size_t i = 0; i < entities.size(); ++i)
                index.emplace(entities[i]->get_id(), i);
        }
    };
}

#endif  // INCLUDE_TKW_ENTITY_MANAGER_HPP
