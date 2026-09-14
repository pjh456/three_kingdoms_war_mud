/**
 * @file   error.hpp
 * @brief  存档模块错误：加载失败原因与 `SaveResult` 别名。
 * @ingroup tkw_save
 */

#ifndef INCLUDE_TKW_SAVE_ERROR_HPP
#define INCLUDE_TKW_SAVE_ERROR_HPP

#include <cstdint>
#include <string>

#include "util/types.hpp"

namespace tkw
{
    namespace save
    {
        /** @brief 存档加载失败原因。 */
        enum class SaveErrorKind : std::uint8_t
        {
            ParseError,       /**< JSON 非法 */
            VersionMismatch,  /**< format/version 不符 */
            DeckMismatch,     /**< 牌表指纹不符 */
            StructureError,   /**< 字段缺失/类型不符/结构非法 */
            RngError,         /**< 随机源状态无法恢复 */
        };

        /** @brief 带定位的存档错误。 */
        struct SaveError
        {
            SaveErrorKind kind = SaveErrorKind::StructureError; /**< 失败原因分类。 */
            std::string detail; /**< 人类可读的失败定位文本。 */
        };

        /**
         * @brief  存档操作结果别名：成功为 `T`，失败带 `SaveError`。
         * @tparam T 成功值类型；`void` 表示无返回值的操作。
         */
        template <typename T>
        using SaveResult = Result<T, SaveError>;
    }
}

#endif  // INCLUDE_TKW_SAVE_ERROR_HPP
