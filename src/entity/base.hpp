/**
 * @file   base.hpp
 * @brief  玩家实体：身份、座位、血条与连环状态的哑状态持有者。
 * @details 实体只保存状态，并经注入的 `EventBus` 发布状态变化事件；
 *          濒死/死亡等流程判定由 combat 域负责。
 * @ingroup tkw_entity
 */

#ifndef INCLUDE_TKW_ENTITY_BASE_HPP
#define INCLUDE_TKW_ENTITY_BASE_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "entity/event.hpp"
#include "entity/hp.hpp"
#include "event/event_bus.hpp"

namespace tkw
{
    namespace entity
    {
        /** @brief 性别（装备能力按异性/同性区分效果）。 */
        enum class Gender : std::uint8_t
        {
            Male,   /**< 男性。 */
            Female, /**< 女性。 */
        };

        /**
         * @brief  玩家实体：id、座位、性别、武将 id、血条与连环状态的哑状态持有者。
         * @details 构造时自动绑定体力监听：`cur` 变化经注入的 `EventBus` 发布
         *          `EntityHpChangedEvent`；`take_damage`/`heal` 另发布原因层事件。
         * @note   事件总线须比实体存活更久（实体析构不发布事件，但存活期间的状态
         *         变化都会发布到该总线）。
         * @warning 本类是哑状态持有者：体力可扣到非正（濒死值状态），但 Dying /
         *          Died 事件都不由本类发布——救场窗口与死亡声明是 combat 的流程职责。
         */
        class Entity
        {
        private:
            std::string m_id;
            int m_seat = 0;
            Hp m_hp;
            EventBus *m_bus;
            Gender m_gender = Gender::Male;
            std::string m_hero;
            bool m_chained = false;

        public:
            /**
             * @brief  构造实体并绑定体力变化监听。
             * @param[in] eid          实体 id；对局内唯一，创建后不可变。
             * @param[in] in_seat      座位号；距离计算与回合序的基础。
             * @param[in] in_hp        初始血条（体力/上限）。
             * @param[in] injected_bus 事件总线；其生命周期必须覆盖本实体。
             * @param[in] in_gender    性别（缺省 `Gender::Male`）。
             * @param[in] in_chained   初始连环状态（缺省 `false`，未横置）。
             * @param[in] in_hero      武将 id（缺省空 = 无名/通用座位）。
             * @post   已注册体力监听；此后 `cur` 的任何变化都会发布
             *         `EntityHpChangedEvent`。
             * @warning `injected_bus` 存活时长不足会使监听回调悬空。
             */
            Entity(
                std::string eid,
                int in_seat,
                Hp in_hp,
                EventBus &injected_bus,
                Gender in_gender = Gender::Male,
                bool in_chained = false,
                std::string in_hero = {});

            /**
             * @brief  返回实体 id。
             * @return 构造时确定的 id 字符串；创建后不变。
             */
            const std::string &get_id() const noexcept { return m_id; }

            /**
             * @brief  返回座位号。
             * @return 座位号；距离计算与回合序的基础。
             */
            int get_seat() const noexcept { return m_seat; }

            /**
             * @brief  返回性别。
             * @return 性别；未显式指定时为 `Gender::Male`。
             */
            Gender get_gender() const noexcept { return m_gender; }

            /**
             * @brief  返回武将 id（空 = 无名/通用座位）。
             * @return 武将目录中的 id 字符串；静态身份，创建后不可变。
             * @note   行为由 game/query 层按目录解析，实体本身不承载规则。
             */
            const std::string &get_hero() const noexcept { return m_hero; }

            /**
             * @brief  查询是否处于连环状态。
             * @return `true` 表示已横置，属性伤害会沿铁索传导。
             */
            bool get_chained() const noexcept { return m_chained; }

            /**
             * @brief  设置连环状态（横置/重置）。
             * @param[in] value `true` 为横置，`false` 为重置。
             * @note   哑状态：不发事件，展示与传导判定由写层负责。
             */
            void set_chained(bool value) noexcept { m_chained = value; }

            /**
             * @brief  返回当前体力。
             * @return 当前体力值；可为非正 = 濒死值状态。
             */
            int get_hp() const noexcept { return m_hp.get_cur(); }

            /**
             * @brief  返回可写血条对象。
             * @return 血条引用，含上限与 `set_cur`/`set_max`/`add`/`sub` 入口。
             */
            Hp &get_hp_bar() noexcept { return m_hp; }

            /**
             * @brief  返回只读血条对象。
             * @return 血条常量引用。
             */
            const Hp &get_hp_bar() const noexcept { return m_hp; }

            /**
             * @brief  承受伤害（战斗结算入口）。
             * @details 先发布 `EntityDamagedEvent`（原因层），再扣体力
             *          （状态层自动发布 `EntityHpChangedEvent`）。
             * @param[in] source   伤害来源 id；仅记录，不要求属于本管理器。
             * @param[in] amount   伤害点数；`amount <= 0` 时直接返回不改状态。
             * @param[in] indirect 是否间接伤害（连环传导等）。
             * @param[in] type     伤害属性（默认普通；火焰/雷电用于防具与传导判定）。
             * @return 实际血量损失。
             * @retval 0     `amount <= 0` 或体力未变化。
             * @retval 正数  实际扣减的点数。
             * @post   先发布伤害事件，再更新体力；`cur` 可变为非正（濒死值状态）。
             */
            int take_damage(
                const std::string &source, int amount, bool indirect,
                card::DamageType type = card::DamageType::Normal);

            /**
             * @brief  恢复 hp（含桃自濒死拉回）。
             * @details 按上限钳制后发布 `EntityHealedEvent`，`amount` 记实际恢复量。
             * @param[in] amount 期望恢复量；`amount <= 0` 时直接返回不改状态。
             * @return 实际恢复量。
             * @retval 0     `amount <= 0`、已满血或钳制后无变化。
             * @retval 正数  实际恢复的点数。
             * @note   濒死者体力为负时，每次 +1 后仍可能非正 → combat 进入下一轮
             *         濒死判定；拉回正数即视为救回。
             * @post   仅在恢复量大于 0 时发布 `EntityHealedEvent`。
             */
            int heal(int amount);

        private:
            void bind_status_events();
        };
    }
}

#endif  // INCLUDE_TKW_ENTITY_BASE_HPP
