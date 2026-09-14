/**
 * @file   file.hpp
 * @brief  文本文件读写（平台无关、Result 风格、不抛异常）。
 * @details 仅负责字节进出：不解析内容（JSON 归 config），不管理路径语义
 *          （资源命名归 config）。存档与数据加载共用本层。
 * @ingroup tkw_io
 */

#ifndef INCLUDE_TKW_IO_FILE_HPP
#define INCLUDE_TKW_IO_FILE_HPP

#include <filesystem>
#include <string>
#include <string_view>

#include <pjh_platform/error.hpp>
#include <pjh_platform/fs.hpp>

#include "io/error.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace io
    {
        namespace plat = pjh::platform;

        /**
         * @brief  将平台错误码收敛为 `IoError`。
         * @param[in] code 平台层错误码。
         * @return 对应的模块级错误；未识别的码归为 `IoError::IoFailed`。
         */
        inline IoError map_error(plat::ErrorCode code)
        {
            switch (code)
            {
            case plat::ErrorCode::NotFound:
                return IoError::NotExist;
            case plat::ErrorCode::PermissionDenied:
                return IoError::Permission;
            default:
                return IoError::IoFailed;
            }
        }

        /**
         * @brief  路径是否存在（文件/目录/断链均按底层 stat 判定）。
         * @param[in] path 待检查路径。
         * @return `true` 表示路径存在。
         */
        inline bool exists(const std::filesystem::path &path) noexcept
        {
            return plat::Fs::exists(path);
        }

        /**
         * @brief  读取整个文件为字符串。空文件返回 `Ok("")`。
         * @param[in] path 目标文件路径。
         * @return 读到的内容；`Err` 携带失败原因。
         * @retval Ok 文件内容；空文件为空串。
         * @retval Err(IoError::NotExist)   文件不存在。
         * @retval Err(IoError::NotAFile)   路径存在但不是常规文件。
         * @retval Err(IoError::Permission) 无读权限。
         * @retval Err(IoError::IoFailed)   其他失败。
         */
        inline IOResult<std::string> read_text(const std::filesystem::path &path)
        {
            if (!plat::Fs::exists(path))
                return IOResult<std::string>::Err(IoError::NotExist);
            if (!plat::Fs::is_regular_file(path))
                return IOResult<std::string>::Err(IoError::NotAFile);
            auto r = plat::Fs::read_file(path);
            if (r.is_ok())
                return IOResult<std::string>::Ok(std::move(r).unwrap());
            return IOResult<std::string>::Err(map_error(r.unwrap_err()));
        }

        /**
         * @brief  写入字符串，覆盖已有内容；文件不存在则创建。
         * @param[in] path    目标文件路径。
         * @param[in] content 待写入内容。
         * @return 成功为空 `Ok`；`Err` 携带失败原因。
         * @retval Ok 写入完成。
         * @retval Err(IoError::NotExist)   父目录不存在。
         * @retval Err(IoError::Permission) 无写权限。
         * @retval Err(IoError::IoFailed)   其他失败。
         * @note   父目录必须已存在（本模块不建目录）。
         */
        inline IOResult<void> write_text(
            const std::filesystem::path &path, std::string_view content)
        {
            auto r = plat::Fs::write_file(path, content);
            if (r.is_ok())
                return IOResult<void>::Ok();
            return IOResult<void>::Err(map_error(r.unwrap_err()));
        }

        /**
         * @brief  原子写：委托平台以同目录唯一临时名写入后原子替换目标。
         * @param[in] path    目标文件路径。
         * @param[in] content 待写入内容。
         * @return 成功为空 `Ok`；`Err` 携带失败原因。
         * @retval Ok 写入并替换完成。
         * @retval Err(IoError::NotExist)   父目录不存在。
         * @retval Err(IoError::Permission) 无写权限。
         * @retval Err(IoError::IoFailed)   其他失败。
         * @note   父目录必须已存在（本模块不建目录）；失败时清理临时文件，
         *         不破坏已有目标文件，并发写不会互相覆盖临时文件。
         */
        inline IOResult<void> write_text_atomic(
            const std::filesystem::path &path, std::string_view content)
        {
            auto r = plat::Fs::write_file_atomic(path, content);
            if (r.is_ok())
                return IOResult<void>::Ok();
            return IOResult<void>::Err(map_error(r.unwrap_err()));
        }
    }
}

#endif  // INCLUDE_TKW_IO_FILE_HPP
