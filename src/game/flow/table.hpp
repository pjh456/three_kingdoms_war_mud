/**
 * @file table.hpp
 * @brief 对局运行时（应用层）：持有事件总线、实体、牌、目录与随机源。
 * @details 产出各域指针保证非空的 `GameContext`。构造顺序：`bus` → `entities`
 *          （绑定 `bus`）→ `cards` → `catalog` → `rng`。
 * @ingroup tkw_game_flow
 */

#ifndef INCLUDE_TKW_GAME_TABLE_HPP
#define INCLUDE_TKW_GAME_TABLE_HPP

#include <memory>
#include <string>
#include <utility>

#include "card/catalog.hpp"
#include "card/manager.hpp"
#include "entity/hp.hpp"
#include "entity/manager.hpp"
#include "event/event_bus.hpp"
#include "game/core/context.hpp"
#include "game/core/roles.hpp"
#include "hero/catalog.hpp"
#include "util/rng.hpp"

namespace tkw
{
    namespace game
    {
        /**
         * @class Game
         * @brief 一局对战的运行时容器。
         * @details 各域成员公开，供应用/测试直接取用；`entities` 在声明处绑定
         *          `bus`，`cards`/`catalog` 由构造函数接管。
         * @warning 不可拷贝、不可移动：`EntityManager` 必须原地锚定，移动会破坏
         *          容器内部引用与注册关系。
         */
        class Game
        {
        public:
            /**
             * @brief  构造：接管卡牌目录与随机源。
             * @param[in] in_catalog 本局卡牌目录。
             * @param[in] in_rng     本局随机源；可为空（空 = 不洗牌，供确定性测试）。
             */
            Game(card::CardDefCatalog in_catalog, std::unique_ptr<Rng> in_rng) :
                catalog(std::move(in_catalog)), rng(std::move(in_rng))
            {
            }

            Game(const Game &) = delete; /**< 拷贝构造：删除（`EntityManager` 须原地锚定）。 */
            Game &operator=(const Game &) = delete; /**< 拷贝赋值：删除。 */
            Game(Game &&) = delete; /**< 移动构造：删除（移动会破坏容器内部锚定）。 */
            Game &operator=(Game &&) = delete; /**< 移动赋值：删除。 */

            /**
             * @brief  注册玩家实体（绑定本局总线）。
             * @param[in] id     实体 id（对局内唯一，如 `P0`）。
             * @param[in] seat   座位下标。
             * @param[in] hp     初始体力构造器。
             * @param[in] gender 性别（缺省 `Male`）。
             * @param[in] hero   武将 id（缺省空 = 无名/通用座位）。
             * @return 创建结果。
             * @retval Ok  新实体指针（未 `remove` 前有效）。
             * @retval Err(EntityError::DuplicateId) 已存在相同 id 的实体。
             */
            entity::EntityResult<entity::Entity *> add_player(
                std::string id, int seat, entity::Hp hp,
                entity::Gender gender = entity::Gender::Male,
                std::string hero = {})
            {
                return entities.create(std::move(id), seat, std::move(hp), gender,
                                       false, std::move(hero));
            }

            /**
             * @brief  构造 `GameContext`。
             * @return 本局上下文：`bus`/`entities`/`cards`/`catalog` 保证非空，
             *         `rng` 可空（空 = 不洗牌，供确定性测试）。
             * @post  返回的上下文指向本对象成员，生命周期不得超过本对象。
             */
            GameContext context()
            {
                GameContext ctx;
                ctx.bus = &bus;
                ctx.entities = &entities;
                ctx.cards = &cards;
                ctx.catalog = &catalog;
                ctx.rng = rng.get();
                ctx.rules = &rules;
                ctx.mode = &mode;
                ctx.roles = &roles;
                ctx.heroes = &hero_catalog;
                return ctx;
            }

            EventBus bus;                          /**< 本局事件总线。 */
            EntityManager entities{bus};           /**< 实体容器（绑定本局总线）。 */
            card::CardManager cards;               /**< 卡牌容器。 */
            card::CardDefCatalog catalog;          /**< 本局卡牌目录。 */
            hero::HeroCatalog hero_catalog;        /**< 本局武将目录（默认空）。 */
            std::unique_ptr<Rng> rng;              /**< 本局随机源；空 = 不洗牌。 */
            RulesConfig rules;                     /**< 本局规则数值（可调参）。 */
            GameMode mode = GameMode::Brawl;       /**< 本局对局模式。 */
            RoleTable roles;                       /**< 身份局角色表（`Brawl` 为空）。 */
        };
    }
}

#endif  // INCLUDE_TKW_GAME_TABLE_HPP
