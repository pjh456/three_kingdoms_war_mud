/**
 * @file context.hpp
 * @brief 对局上下文：把 entity / card / catalog 三个域的容器捆绑成一个
 *        引用集，作为 gameplay 各结算函数的入口。
 * @note 不持有所有权：GameContext 只聚合指针，生命周期由调用方保证
 *       （bus/entities/cards/catalog 均须比本对象存活更久）。跨域引用
 *       沿用 id 字符串约定，本结构不依赖 entity 类的具体形态。
 * @note 推荐经 Game::context() 构造：它保证 bus/entities/cards/catalog
 *       非空（rng 可空 = 不洗牌），并绑定同一局的生命周期。
 */

#ifndef INCLUDE_TKW_GAME_CONTEXT_HPP
#define INCLUDE_TKW_GAME_CONTEXT_HPP

#include <string>

#include "card/catalog.hpp"
#include "card/manager.hpp"
#include "entity/manager.hpp"
#include "event/event_bus.hpp"
#include "game/core/roles.hpp"
#include "game/core/rules.hpp"
#include "hero/catalog.hpp"
#include "util/rng.hpp"

namespace tkw
{
    namespace game
    {
        /**
         * @brief 决策接缝的只读上下文：聚合 const 容器指针。
         * @note 不暴露事件总线与随机源：决策源不得发事件、不得推进随机；
         *       所有容器成员均为 const 指针，实体只读遍历走
         *       EntityManager::const_view()，故传入本类型即在类型上排除改状态。
         */
        struct ReadOnlyContext
        {
            const EntityManager *entities = nullptr;
            const card::CardManager *cards = nullptr;
            const card::CardDefCatalog *catalog = nullptr;
            const RulesConfig *rules = nullptr;
            const GameMode *mode = nullptr;    /**< 对局模式（由对局持有） */
            const RoleTable *roles = nullptr;  /**< 身份局角色表（由对局持有） */
            const hero::HeroCatalog *heroes = nullptr; /**< 武将目录（由对局持有） */
        };

        /** @brief 对局上下文（引用捆绑，不持有）。 */
        struct GameContext
        {
            EventBus *bus = nullptr;
            EntityManager *entities = nullptr;
            card::CardManager *cards = nullptr;
            const card::CardDefCatalog *catalog = nullptr;
            Rng *rng = nullptr;                  /**< 判定/洗牌随机源（由对局持有） */
            const RulesConfig *rules = nullptr;  /**< 规则数值（由对局持有） */
            const GameMode *mode = nullptr;      /**< 对局模式（由对局持有） */
            const RoleTable *roles = nullptr;    /**< 身份局角色表（由对局持有） */
            const hero::HeroCatalog *heroes = nullptr; /**< 武将目录（由对局持有） */
            std::string turn_player;             /**< 当前回合角色 id；空 = 无回合上下文 */
            std::string jiu_damage_owner;       /**< 本回合下一张使用的「杀」享有酒加成的玩家 id；空 = 无加成 */
            bool jiu_used = false;               /**< 本回合出牌阶段是否已使用过酒（限一次） */

            /** @brief 隐式转出只读视图（值拷贝七个 const 指针），供决策接缝使用。 */
            operator ReadOnlyContext() const
            {
                return ReadOnlyContext{entities, cards, catalog, rules, mode, roles,
                                       heroes};
            }
        };

        /** @brief 取规则数值；ctx 未绑定规则时回落到默认值（测试便利）。 */
        inline const RulesConfig &rules_of(const ReadOnlyContext &ctx)
        {
            static const RulesConfig fallback{};
            return ctx.rules ? *ctx.rules : fallback;
        }

        /** @brief 取对局模式；ctx 未绑定模式时回落 Brawl（测试便利）。 */
        inline GameMode mode_of(const GameContext &ctx)
        {
            return ctx.mode ? *ctx.mode : GameMode::Brawl;
        }

        /** @brief 取对局模式（只读视图）；ctx 未绑定模式时回落 Brawl。 */
        inline GameMode mode_of(const ReadOnlyContext &ctx)
        {
            return ctx.mode ? *ctx.mode : GameMode::Brawl;
        }

        /** @brief 取玩家角色；ctx 未绑定角色表或未命中时回落 Role::None。 */
        inline Role role_of(const GameContext &ctx, const std::string &id)
        {
            return role_of(ctx.roles, id);
        }
    }
}

#endif  // INCLUDE_TKW_GAME_CONTEXT_HPP