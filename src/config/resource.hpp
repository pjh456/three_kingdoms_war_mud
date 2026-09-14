/**
 * @file   resource.hpp
 * @brief  命名资源寻址与加载：`name` → `<root>/<name>.json` → 解析后的 Document。
 * @details 只负责「文件变 Document」：不做内容语义校验（schema 归域模块），
 *          不缓存（调用方持有 Document 并自行解析成值类型后可安全丢弃）。
 * @ingroup tkw_config
 */

#ifndef INCLUDE_TKW_CONFIG_RESOURCE_HPP
#define INCLUDE_TKW_CONFIG_RESOURCE_HPP

#include <filesystem>
#include <string>
#include <string_view>

#include <pjh_json/document.hpp>
#include <pjh_json/error.hpp>

#include "config/error.hpp"
#include "io/file.hpp"

namespace tkw
{
    namespace config
    {
        namespace json = pjh::json;

        /**
         * @class  ResourceStore
         * @brief  资源目录句柄。root 在构造时固定（测试/调平衡注入不同目录）。
         */
        class ResourceStore
        {
        public:
            /**
             * @brief  以资源根目录构造。
             * @param[in] root 资源根目录；内部持有其副本。
             */
            explicit ResourceStore(std::filesystem::path root) : m_root(std::move(root))
            {
            }

            /**
             * @brief  返回资源根目录。
             * @return 构造时固定的根目录引用。
             */
            const std::filesystem::path &root() const noexcept { return m_root; }

            /**
             * @brief  加载资源 `<root>/<name>.json`。
             * @param[in] name 资源名（可含 `/` 作为逻辑段分隔，不含 `.json` 后缀）。
             * @return 解析成功的 JSON Document；失败时 `Err` 携带原因。
             * @retval Ok 解析完成的 Document。
             * @retval Err(ConfigErrorKind::FileNotFound) 文件不存在，detail 为完整路径。
             * @retval Err(ConfigErrorKind::IoFailed)     目录、无权限等，detail 为完整路径。
             * @retval Err(ConfigErrorKind::ParseError)   非法 JSON，detail 为 `路径 @ 字节offset`。
             * @note   不做 schema 校验；返回的 Document 生命周期由调用方持有。
             */
            ConfigResult<json::Document> load(std::string_view name) const;

        private:
            static ConfigError to_config_error(
                io::IoError e, const std::filesystem::path &file);

            std::filesystem::path m_root;
        };
    }
}

#endif  // INCLUDE_TKW_CONFIG_RESOURCE_HPP
