/**
 * @file   resource.cpp
 * @brief  `ResourceStore` 的定义：资源文件寻址、读取与错误收敛。
 * @ingroup tkw_config
 */

#include "config/resource.hpp"

#include <string>
#include <utility>

namespace tkw
{
    namespace config
    {
        ConfigResult<json::Document> ResourceStore::load(std::string_view name) const
        {
            auto file = m_root / (std::string(name) + ".json");
            // name 可含 '/' 作为逻辑段分隔，统一成原生分隔符，避免混用。
            file.make_preferred();

            auto text = io::read_text(file);
            if (text.is_err())
                return ConfigResult<json::Document>::Err(
                    to_config_error(text.unwrap_err(), file));

            auto parsed = json::parse_copy_result(text.unwrap());
            if (parsed.is_err())
            {
                const auto &e = parsed.unwrap_err();
                return ConfigResult<json::Document>::Err(
                    ConfigError{
                        ConfigErrorKind::ParseError,
                        file.string() + " @ " + std::to_string(e.offset())});
            }
            return ConfigResult<json::Document>::Ok(std::move(parsed).unwrap());
        }

        ConfigError ResourceStore::to_config_error(
            io::IoError e, const std::filesystem::path &file)
        {
            if (e == io::IoError::NotExist)
                return {ConfigErrorKind::FileNotFound, file.string()};
            return {ConfigErrorKind::IoFailed, file.string()};
        }
    }
}
