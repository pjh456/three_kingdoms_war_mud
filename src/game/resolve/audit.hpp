/**
 * @file audit.hpp
 * @brief 牌堆能力审计：找出引擎未认识或尚未实现的卡（主动效果/装备能力）。
 * @details 语义归属 game 层，catalog 保持不依赖 resolver；应用层在开局前
 *          用它提示「牌堆含未实现卡」。
 * @note 两条审计路径：已加载目录（枚举 implemented 状态，未来「先加枚举、
 *       后补结算」的安全网）；原始牌堆（容错扫机制名，未知机制名记为未实现，
 *       不要求牌堆能被严格加载）。对局建局仍只走严格加载。
 * @ingroup tkw_game_resolve
 */

#ifndef INCLUDE_TKW_GAME_AUDIT_HPP
#define INCLUDE_TKW_GAME_AUDIT_HPP

#include <string>
#include <string_view>
#include <vector>

#include "card/catalog.hpp"
#include "card/def.hpp"
#include "config/resource.hpp"
#include "game/core/effect.hpp"

namespace tkw
{
    namespace game
    {
        namespace cfg = tkw::config;

        /** @brief 未实现卡：deck 序 id + 展示名。 */
        struct UnsupportedCard
        {
            std::string id;   /**< 卡 id（= 文件名）。 */
            std::string name; /**< 卡中文名；牌表未给名称时回落 id。 */

            /**
             * @brief  逐字段比较 `id` 与 `name` 是否相等。
             * @return `true` 表示两字段均相等。
             */
            bool operator==(const UnsupportedCard &) const = default;
        };

        /**
         * @brief 返回牌堆中「本应可用但引擎尚未实现」的卡 id（deck 序）。
         * @param[in] catalog 已加载的卡牌定义目录。
         * @return 未实现卡 id 列表（按 deck 序，去重后每卡至多一次）。
         * @note 闪/无懈为响应牌、延时锦囊可于出牌阶段放置且判定阶段已实现，
         *       均不计入；装备打出即装备，但含未实现装备能力时计入（一张卡至多
         *       列一次）。
         */
        std::vector<std::string> unsupported_cards(
            const card::CardDefCatalog &catalog);

        /**
         * @brief 返回牌堆中「机制名未被引擎认识或尚未实现」的卡（deck 序）。
         * @param[in] store     资源目录句柄。
         * @param[in] deck_name 牌堆资源名（通常 "deck"）。
         * @return 未实现卡清单或资源错误。
         * @retval Ok  未实现卡清单（可为空）。
         * @retval Err 牌堆文件/结构错误（与严格加载同级，detail 带路径）。
         * @note 走容错扫描：未知 effect.kind / abilities 名不使扫描失败，而是
         *       逐卡记为未实现；建局仍走严格加载，未知机制在对局入口直接失败。
         */
        cfg::ConfigResult<std::vector<UnsupportedCard>> unsupported_cards(
            const cfg::ResourceStore &store, std::string_view deck_name);
    }
}

#endif  // INCLUDE_TKW_GAME_AUDIT_HPP
