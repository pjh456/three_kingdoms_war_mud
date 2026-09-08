/**
 * @file file.hpp
 * @brief 文本文件读写（平台无关、Result 风格、不抛异常）。
 * @note 仅负责字节进出：不解析内容（JSON 归 config），不管理路径语义
 *       （资源命名归 config）。存档与数据加载共用本层。
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
         * @brief 路径是否存在（文件/目录/断链均按底层 stat 判定）。
         */
        inline bool exists(const std::filesystem::path &path) noexcept
        {
            return plat::Fs::exists(path);
        }

        /**
         * @brief 读取整个文件为字符串。空文件返回 Ok("")。
         * @return NotExist（文件不存在）/ NotAFile（是目录等非常规文件）/
         *         Permission（无读权限）/ IoFailed（其他失败）。
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
         * @brief 写入字符串，覆盖已有内容；文件不存在则创建。
         * @note 父目录必须已存在（本模块不建目录）。
         * @return NotExist（父目录不存在）/ Permission（无写权限）/ IoFailed（其他失败）。
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
         * @brief 原子写：先写 `<path>.tmp`，再 rename 覆盖目标。
         * @note 失败时清理临时文件，不破坏已有目标文件。
         */
        inline IOResult<void> write_text_atomic(
            const std::filesystem::path &path, std::string_view content)
        {
            auto tmp = path;
            tmp += ".tmp";
            auto w = write_text(tmp, content);
            if (w.is_err())
                return w;
            auto r = plat::Fs::rename(tmp, path, /*overwrite=*/true);
            if (r.is_err())
            {
                (void)plat::Fs::remove_all(tmp);
                return IOResult<void>::Err(map_error(r.unwrap_err()));
            }
            return IOResult<void>::Ok();
        }
    }
}

#endif  // INCLUDE_TKW_IO_FILE_HPP
