/**
 * @file audit.hpp
 * @brief 牌堆能力审计：找出引擎未认识或尚未实现的卡（主动效果/装备能力）。
 * @note 语义归属 game 层，catalog 保持不依赖 resolver；应用层在开局前
 *       用它提示「牌堆含未实现卡」。
 * @note 两条审计路径：已加载目录（枚举 implemented 状态，未来「先加枚举、
 *       后补结算」的安全网）；原始牌堆（容错扫机制名，未知机制名记为未实现，
 *       不要求牌堆能被严格加载）。对局建局仍只走严格加载。
 */

#ifndef INCLUDE_TKW_GAME_AUDIT_HPP
#define INCLUDE_TKW_GAME_AUDIT_HPP

#include <string>
#include <string_view>
#include <vector>

#include "card/catalog.hpp"
#include "card/def.hpp"
#include "config/resource.hpp"
#include "game/resolve/resolver.hpp"

namespace tkw
{
    namespace game
    {
        namespace cfg = tkw::config;

        /** @brief 未实现卡：deck 序 id + 展示名。 */
        struct UnsupportedCard
        {
            std::string id;   /**< 卡 id（= 文件名） */
            std::string name; /**< 卡中文名；牌表未给名称时回落 id */

            bool operator==(const UnsupportedCard &) const = default;
        };

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

        /**
         * @brief 返回牌堆中「机制名未被引擎认识或尚未实现」的卡（deck 序）。
         * @param store     资源目录句柄。
         * @param deck_name 牌堆资源名（通常 "deck"）。
         * @return Ok 为未实现卡清单（可为空）；Err 为牌堆文件/结构错误
         *         （与严格加载同级，detail 带路径）。
         * @note 走容错扫描：未知 effect.kind / abilities 名不使扫描失败，而是
         *       逐卡记为未实现；建局仍走严格加载，未知机制在对局入口直接失败。
         */
        inline cfg::ConfigResult<std::vector<UnsupportedCard>> unsupported_cards(
            const cfg::ResourceStore &store, std::string_view deck_name)
        {
            auto raws = card::scan_mechanisms(store, deck_name);
            if (raws.is_err())
                return cfg::ConfigResult<std::vector<UnsupportedCard>>::Err(
                    raws.unwrap_err());

            std::vector<UnsupportedCard> out;
            for (const auto &raw : raws.unwrap())
            {
                bool unsupported = false;

                // 未知效果名一律计入；已知名按 implemented 属性判定
                if (raw.effect_kind.is_some())
                {
                    const auto kind =
                        card::effect_kind_from_name(raw.effect_kind.unwrap());
                    unsupported = kind.is_none() ||
                                  is_unimplemented_active_kind(kind.unwrap());
                }

                // 主动效果已判定为未实现时无需再看能力；一张卡至多列一次
                if (!unsupported)
                {
                    for (const auto &name : raw.abilities)
                    {
                        const auto ability = card::ability_from_name(name);
                        if (ability.is_none() ||
                            is_unimplemented_ability(ability.unwrap()))
                        {
                            unsupported = true;
                            break;
                        }
                    }
                }

                if (unsupported)
                    out.push_back(UnsupportedCard{raw.id, raw.name});
            }
            return cfg::ConfigResult<std::vector<UnsupportedCard>>::Ok(std::move(out));
        }
    }
}

#endif  // INCLUDE_TKW_GAME_AUDIT_HPP
