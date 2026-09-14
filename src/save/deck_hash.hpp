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
            void hash_bytes(std::uint64_t &h, std::string_view s);

            /**
             * @brief  将整数以十进制文本加分隔符混入 FNV-1a 累加器。
             * @param[in,out] h 累加器；原地更新。
             * @param[in]     v 待混入的整数值。
             */
            void hash_int(std::uint64_t &h, std::int64_t v);
        }

        /**
         * @brief  牌表指纹：对目录语义字段做 FNV-1a（读档校验一致性）。
         * @param[in] catalog 待指纹的牌表目录。
         * @param[in] include_judge_damage_type 为真（默认）时 judge 的显式非普通属性
         *            参与哈希；为假时跳过该字段，用于重算「判定属性尚未数据化」时期
         *            写出的历史标准档指纹。
         * @return 目录语义字段的 64 位 FNV-1a 指纹。
         */
        std::uint64_t deck_hash(
            const card::CardDefCatalog &catalog,
            bool include_judge_damage_type = true);
    }
}

#endif  // INCLUDE_TKW_SAVE_DECK_HASH_HPP
