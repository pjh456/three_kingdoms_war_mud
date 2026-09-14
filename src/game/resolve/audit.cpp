/**
 * @file   audit.cpp
 * @brief  牌堆能力审计的函数体定义。
 * @details 实现 `audit.hpp` 声明的两条审计路径：已加载目录枚举与原始牌堆容错
 *          扫描。
 * @ingroup tkw_game_resolve
 */

#include "game/resolve/audit.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace tkw
{
    namespace game
    {
        std::vector<std::string> unsupported_cards(
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

        cfg::ConfigResult<std::vector<UnsupportedCard>> unsupported_cards(
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
