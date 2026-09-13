/**
 * @file def.hpp
 * @brief 武将域值类型：技能封闭枚举 + 不可变 HeroDef（武将定义）。
 * @note 本文件不含任何 JSON 解析（归 catalog.hpp）。HeroDef 是纯值类型：
 *       解析器产出后即独立于 Document 生命周期，可直接值拷贝/比较。
 * @note skills 是「配置数据 ↔ 代码语义」的接缝：JSON 技能名在加载期解析为
 *       枚举，未知技能名直接 InvalidValue 失败；行为实现状态见
 *       game/core/effect.hpp 的 hero_skill_traits，本支撑域只承载数据。
 */

#ifndef INCLUDE_TKW_HERO_DEF_HPP
#define INCLUDE_TKW_HERO_DEF_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "entity/base.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace hero
    {
        /**
         * @brief 武将技能类别（封闭枚举，与 JSON 的 skills 名一一对应）。
         * @note 枚举只承载「有哪些技能」；是否已在引擎结算中实现由
         *       game::hero_skill_traits 声明，未实现技能在审计面显式暴露。
         */
        enum class HeroSkill : std::uint8_t
        {
            PaoXiao, /**< 咆哮：锁定技，使用【杀】无次数限制 */
            WuSheng, /**< 武圣：转化技，将一张红色牌当【杀】使用或打出 */
            YingZi,  /**< 英姿：锁定技，摸牌阶段多摸一张牌 */
            FanKui,  /**< 反馈：触发技，受到伤害后可获得伤害来源一张牌 */
        };

        /** @brief 不可变武将定义：由 HeroCatalog 从 JSON 解析产出。 */
        struct HeroDef
        {
            std::string id;   /**< 武将 id（= 文件名） */
            std::string name; /**< 武将中文名 */
            Option<entity::Gender> gender =
                Option<entity::Gender>::None(); /**< 性别；缺失不覆盖座位占位 */
            int hp = 0; /**< 体力上限；0 = 不覆盖 rules.base_hp */
            std::vector<HeroSkill> skills; /**< 技能列表（引用序） */
            std::string text;              /**< 技能文案（仅展示） */

            bool operator==(const HeroDef &) const = default;
        };

        /**
         * @brief 武将展示名：name 非空取 name，否则回落 id。
         * @return 引用指向 def 自身字段（name 或 id），生命周期同 def。
         */
        inline const std::string &display_hero_name(const HeroDef &def) noexcept
        {
            return def.name.empty() ? def.id : def.name;
        }
    }
}

#endif  // INCLUDE_TKW_HERO_DEF_HPP
