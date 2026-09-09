/**
 * @file audit.hpp
 * @brief 牌堆能力审计：找出引擎尚未实现的卡（未实现的主动效果/装备能力）。
 * @note 语义归属 game 层，catalog 保持不依赖 resolver；应用层在开局前
 *       用它提示「牌堆含未实现卡」。
 */

#ifndef INCLUDE_TKW_GAME_AUDIT_HPP
#define INCLUDE_TKW_GAME_AUDIT_HPP

#include <string>
#include <vector>

#include "card/catalog.hpp"
#include "card/def.hpp"
#include "game/resolve/resolver.hpp"

namespace tkw
{
    namespace game
    {
        /**
         * @brief 返回牌堆中「本应可用但引擎尚未实现」的卡 id（deck 序）。
         * @note 闪/无懈为响应牌、延时锦囊可于出牌阶段放置且判定阶段已实现，
         *       均不计入；装备打出即装备，但含未实现装备能力时计入（一张卡至多
         *       列一次）。
         */
        inline std::vector<std::string> unsupported_cards(
            const card::CardDefCatalog &catalog)
        {
            std::vector<std::string> out;
            for (const auto &def : catalog)
            {
                // 主动效果未实现
                if (def.effect.is_some() &&
                    is_unimplemented_active_kind(def.effect.unwrap().kind))
                {
                    out.push_back(def.id);
                    continue;
                }
                // 装备能力任一未实现即计入（多个未实现只列一次）
                for (const auto a : def.abilities)
                {
                    if (is_unimplemented_ability(a))
                    {
                        out.push_back(def.id);
                        break;
                    }
                }
            }
            return out;
        }
    }
}

#endif  // INCLUDE_TKW_GAME_AUDIT_HPP
