/**
 * @file deck_hash.hpp
 * @brief 牌表指纹：对目录语义字段做 FNV-1a（存读档一致性校验）。
 * @note 指纹是持久化契约：初值、质数与字段哈希顺序决定旧档能否读入，改动须
 *       同步存档版本与迁移策略。
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
            inline void hash_bytes(std::uint64_t &h, std::string_view s)
            {
                for (unsigned char c : s)
                {
                    h ^= c;
                    h *= 1099511628211ULL;
                }
            }

            inline void hash_int(std::uint64_t &h, std::int64_t v)
            {
                hash_bytes(h, std::to_string(v));
                hash_bytes(h, "|");
            }
        }

        /** @brief 牌表指纹：对目录语义字段做 FNV-1a（读档校验一致性）。 */
        inline std::uint64_t deck_hash(const card::CardDefCatalog &catalog)
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
                }
                for (auto a : def.abilities)
                    detail::hash_int(h, static_cast<int>(a));
            }
            return h;
        }
    }
}

#endif  // INCLUDE_TKW_SAVE_DECK_HASH_HPP
