/**
 * @file   error.hpp
 * @brief  实体域错误码与结果别名。
 * @ingroup tkw_entity
 */

#ifndef INCLUDE_TKW_ENTITY_ERROR_HPP
#define INCLUDE_TKW_ENTITY_ERROR_HPP

#include <cstdint>

#include "util/types.hpp"

namespace tkw
{
    namespace entity
    {
        /** @brief 实体域错误。 */
        enum class EntityError : std::uint8_t
        {
            DuplicateId, /**< 层内已存在相同 id 的实体。 */
        };

        /**
         * @brief 实体域结果别名。
         * @tparam T 成功时承载的值类型。
         */
        template <typename T>
        using EntityResult = Result<T, EntityError>;
    }
}

#endif  // INCLUDE_TKW_ENTITY_ERROR_HPP
