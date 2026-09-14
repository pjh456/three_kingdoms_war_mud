/**
 * @file   deck_hash.hpp
 * @brief  牌表指纹：对目录语义字段做 FNV-1a（存读档一致性校验）。
 * @details 指纹是持久化契约：初值、质数与字段哈希顺序决定旧档能否读入，改动须
 *          同步存档版本与迁移策略；判定属性尚未数据化时期写出的历史值经降级模式
 *          重算，由读取端一并接受。
 * @ingroup tkw_save
 */

#ifndef INCLUDE_TKW_SAVE_DECK_HASH_HPP
#define INCLUDE_TKW_SAVE_DECK_HASH_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "card/catalog.hpp"

namespace tkw
{
    namespace save
    {
        namespace detail
        {
            /**
             * @brief  将字节串混入 FNV-1a 累加器。
             * @param[in,out] h 累加器；按字节异或后乘质数原地更新。
             * @param[in]     s 待混入的字节串。
             */
            inline void hash_bytes(std::uint64_t &h, std::string_view s)
            {
                for (unsigned char c : s)
                {
                    h ^= c;
                    h *= 1099511628211ULL;
                }
            }

            /**
             * @brief  将整数以十进制文本加分隔符混入 FNV-1a 累加器。
             * @param[in,out] h 累加器；原地更新。
             * @param[in]     v 待混入的整数值。
             */
            inline void hash_int(std::uint64_t &h, std::int64_t v)
            {
                hash_bytes(h, std::to_string(v));
                hash_bytes(h, "|");
            }
        }

        /**
         * @brief  牌表指纹：对目录语义字段做 FNV-1a（读档校验一致性）。
         * @param[in] catalog 待指纹的牌表目录。
         * @param[in] include_judge_damage_type 为真（默认）时 judge 的显式非普通属性
         *            参与哈希；为假时跳过该字段，用于重算「判定属性尚未数据化」时期
         *            写出的历史标准档指纹。
         * @return 目录语义字段的 64 位 FNV-1a 指纹。
         */
        inline std::uint64_t deck_hash(
            const card::CardDefCatalog &catalog,
            bool include_judge_damage_type = true)
        {
            std::uint64_t h = 1469598103934665603ULL;
            for (const auto &def : catalog)
            {
                detail::hash_bytes(h, def.id);
                detail::hash_bytes(h, "|");
                detail::hash_int(h, static_cast<int>(def.type));
                detail::hash_bytes(h, def.subtype);
                detail::hash_bytes(h, "|");
                detail::hash_int(h, def.rescue ? 1 : 0);
                detail::hash_int(h, def.counter ? 1 : 0);
                // 仅显式属性参与哈希：标准牌表全为 false，指纹逐位不变
                if (def.self_rescue)
                    detail::hash_int(h, 1);
                if (def.recast)
                    detail::hash_int(h, 1);
                for (const auto &c : def.copies)
                {
                    detail::hash_int(h, static_cast<int>(c.suit));
                    detail::hash_int(h, c.number);
                }
                if (def.effect.is_some())
                {
                    const auto &e = def.effect.unwrap();
                    detail::hash_int(h, static_cast<int>(e.kind));
                    detail::hash_int(h, e.amount);
                    detail::hash_int(h, e.count);
                    detail::hash_int(
                        h, e.scope.is_some() ? static_cast<int>(e.scope.unwrap()) : -1);
                    detail::hash_int(
                        h, e.response.is_some()
                               ? static_cast<int>(e.response.unwrap())
                               : -1);
                    detail::hash_int(h, e.range);
                    // 仅显式属性参与哈希：普通伤害不写字段，标准牌表指纹逐位不变
                    if (e.damage_type != card::DamageType::Normal)
                        detail::hash_int(h, static_cast<int>(e.damage_type));
                }
                if (def.equip.is_some())
                {
                    const auto &e = def.equip.unwrap();
                    detail::hash_int(h, static_cast<int>(e.slot));
                    detail::hash_int(h, e.range);
                }
                if (def.judge.is_some())
                {
                    const auto &j = def.judge.unwrap();
                    detail::hash_int(h, static_cast<int>(j.trigger));
                    detail::hash_int(h, static_cast<int>(j.success));
                    detail::hash_int(h, static_cast<int>(j.failure));
                    detail::hash_int(h, j.amount);
                    detail::hash_int(
                        h, j.scope.is_some() ? static_cast<int>(j.scope.unwrap()) : -1);
                    // 仅显式距离限制参与哈希：默认无限制不写字段，标准牌表指纹逐位不变
                    if (j.range > 0)
                        detail::hash_int(h, j.range);
                    // 同 effect：仅显式属性参与哈希；降级模式跳过该字段，
                    // 以重算判定属性数据化前的历史标准档指纹
                    if (include_judge_damage_type &&
                        j.damage_type != card::DamageType::Normal)
                        detail::hash_int(h, static_cast<int>(j.damage_type));
                }
                for (auto a : def.abilities)
                    detail::hash_int(h, static_cast<int>(a));
            }
            return h;
        }
    }
}

#endif  // INCLUDE_TKW_SAVE_DECK_HASH_HPP
