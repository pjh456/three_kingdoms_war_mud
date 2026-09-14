/**
 * @file   file.cpp
 * @brief  文本文件读写的实现。
 * @ingroup tkw_io
 */

#include "io/file.hpp"

#include <utility>

#include <pjh_platform/fs.hpp>

namespace tkw
{
    namespace io
    {
        IoError map_error(plat::ErrorCode code)
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

        bool exists(const std::filesystem::path &path) noexcept
        {
            return plat::Fs::exists(path);
        }

        IOResult<std::string> read_text(const std::filesystem::path &path)
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

        IOResult<void> write_text(
            const std::filesystem::path &path, std::string_view content)
        {
            auto r = plat::Fs::write_file(path, content);
            if (r.is_ok())
                return IOResult<void>::Ok();
            return IOResult<void>::Err(map_error(r.unwrap_err()));
        }

        IOResult<void> write_text_atomic(
            const std::filesystem::path &path, std::string_view content)
        {
            auto r = plat::Fs::write_file_atomic(path, content);
            if (r.is_ok())
                return IOResult<void>::Ok();
            return IOResult<void>::Err(map_error(r.unwrap_err()));
        }
    }
}
