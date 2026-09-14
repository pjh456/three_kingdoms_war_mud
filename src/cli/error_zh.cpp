/**
 * @file   error_zh.cpp
 * @brief  CLI 错误中文渲染的定义。
 * @ingroup tkw_cli
 */

#include "cli/error_zh.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tkw
{
    namespace cli
    {
        namespace detail
        {
            std::string join_items(
                const std::vector<std::string> &items, std::string_view sep)
            {
                std::string out;
                for (std::size_t i = 0; i < items.size(); ++i)
                {
                    if (i > 0)
                        out += sep;
                    out += items[i];
                }
                return out;
            }

            std::string expected_type_zh(std::string_view expected_type)
            {
                if (expected_type == "integer")
                    return "整数";
                if (expected_type == "float")
                    return "浮点数";
                if (expected_type.starts_with("bool"))
                    return "布尔值";
                return std::string(expected_type);
            }

            std::string format_load_error(const config::ConfigError &e)
            {
                return "加载牌堆失败（" +
                       std::string(config_error_kind_zh(e.kind)) + "）: " + e.detail;
            }

            std::string format_loop_error(game::LoopError code)
            {
                return loop_error_zh(code);
            }

            std::string format_turn_failure(
                game::LoopError code, game::TurnError root,
                const std::string &actor)
            {
                if (code == game::LoopError::NoPlayers)
                    return "回合执行失败（角色 " + actor + "，角色不存在）";
                std::string text =
                    "回合执行失败（角色 " + actor + "，" +
                    std::string(turn_error_label_zh(root));
                if (code == game::LoopError::TurnFailed)
                    text += "；本回合已终止并跳过（已部分结算），可继续推进";
                text += "）";
                return text;
            }

            std::string format_build_error(const game::BuildError &e)
            {
                if (e.kind == game::BuildError::Kind::CreatePlayer)
                    return "创建玩家失败: P" + std::to_string(e.player_index);
                if (e.kind == game::BuildError::Kind::UnknownHero)
                    return "武将不存在: " + e.hero + "（座位 P" +
                           std::to_string(e.player_index) +
                           "；用 tkw heroes 查看可用武将）";
                if (e.kind == game::BuildError::Kind::IdentityPlayerCount)
                {
                    // 身份局配比只覆盖 4–8 人（roles_for_count）；人数越界时按越界
                    // 方向给出最近的合法值，使「下一步」具体可复制。
                    const int n = e.player_index;
                    const int suggested = n < 4 ? 4 : 8;
                    return "身份模式人数须为 4–8 人（当前 " + std::to_string(n) +
                           "）；请用 --players " + std::to_string(suggested);
                }
                return format_load_error(e.config);
            }

        }  // namespace detail
    }  // namespace cli
}  // namespace tkw

namespace tkw
{
    namespace cli
    {
        std::string render_parse_error_zh(const pjh::cli::CliError &err)
        {
            const pjh::cli::ErrorInfo &info = err.info();
            switch (err.tag())
            {
            case pjh::cli::ErrorTag::RawMessage:
                return std::get<pjh::cli::RawMessageError>(info).message;
            case pjh::cli::ErrorTag::Parse:
            {
                const auto &e = std::get<pjh::cli::ParseError>(info);
                return "参数解析失败: '" + e.raw_input + "'（第 " +
                       std::to_string(e.position) + " 个参数）";
            }
            case pjh::cli::ErrorTag::UnknownOption:
            {
                const auto &e = std::get<pjh::cli::UnknownOptionError>(info);
                if (e.suggestions.empty())
                    return "未知选项: '" + e.option_display + "'";
                return "未知选项: '" + e.option_display + "'；您是否要找: " +
                       detail::join_items(e.suggestions, "、");
            }
            case pjh::cli::ErrorTag::MissingValue:
                return "选项 '" +
                       std::get<pjh::cli::MissingValueError>(info).option_display +
                       "' 需要一个值";
            case pjh::cli::ErrorTag::MissingRequiredOption:
                return "缺少必需选项: '" +
                       std::get<pjh::cli::MissingRequiredOptionError>(info).option_name +
                       "'";
            case pjh::cli::ErrorTag::MissingRequiredArg:
                return "缺少必需参数: '" +
                       std::get<pjh::cli::MissingRequiredArgError>(info).arg_name + "'";
            case pjh::cli::ErrorTag::TypeConversion:
            {
                const auto &e = std::get<pjh::cli::TypeConversionError>(info);
                return "选项 '" + e.option_display + "' 的值 '" + e.raw_value +
                       "' 无效: 期望 " + detail::expected_type_zh(e.expected_type);
            }
            case pjh::cli::ErrorTag::AmbiguousCommand:
            {
                const auto &e = std::get<pjh::cli::AmbiguousCommandError>(info);
                return "命令 '" + e.input + "' 有歧义，候选: " +
                       detail::join_items(e.candidates, "、");
            }
            case pjh::cli::ErrorTag::UnknownCommand:
            {
                const auto &e = std::get<pjh::cli::UnknownCommandError>(info);
                if (e.suggestions.empty())
                    return "未知命令: '" + e.input + "'";
                return "未知命令: '" + e.input + "'；您是否要找: " +
                       detail::join_items(e.suggestions, "、");
            }
            case pjh::cli::ErrorTag::ValueOutOfRange:
            {
                const auto &e = std::get<pjh::cli::ValueOutOfRangeError>(info);
                return "选项 '" + e.option_display + "' 的值 '" + e.raw_value +
                       "' 超出范围 [" + e.min + ", " + e.max + "]";
            }
            case pjh::cli::ErrorTag::EnumValue:
            {
                const auto &e = std::get<pjh::cli::EnumValueError>(info);
                return "选项 '" + e.option_display + "' 的值 '" + e.raw_value +
                       "' 无效: 期望以下之一: " +
                       detail::join_items(e.valid_choices, "、");
            }
            case pjh::cli::ErrorTag::CommandDisabled:
                return "命令 '" +
                       std::get<pjh::cli::CommandDisabledError>(info).command_name +
                       "' 当前不可用";
            case pjh::cli::ErrorTag::ConflictingOptions:
                return "选项冲突: " +
                       detail::join_items(
                           std::get<pjh::cli::ConflictingOptionsError>(info).option_names,
                           "、") +
                       " 不能同时使用";
            case pjh::cli::ErrorTag::RequiredOptionGroup:
            {
                const auto &e = std::get<pjh::cli::RequiredOptionGroupError>(info);
                return std::string(e.exactly_one ? "必须提供" : "至少提供") + " " +
                       detail::join_items(e.option_names, "、") + " 之一";
            }
            case pjh::cli::ErrorTag::OptionDoesNotAcceptValue:
                return "选项 '" +
                       std::get<pjh::cli::OptionDoesNotAcceptValueError>(info)
                           .option_display +
                       "' 不接受值";
            case pjh::cli::ErrorTag::NoCommandMatched:
                return "没有匹配的命令";
            case pjh::cli::ErrorTag::Runtime:
                // Runtime 走 what()（项目自身消息本已中文），非 Runtime 不会到此。
                return err.what();
            }
            // 上游新增 ErrorTag：中文兜底，避免英文静默泄漏。
            return "参数解析失败";
        }

        std::string render_error_zh(const pjh::cli::CliError &err)
        {
            if (err.kind() == pjh::cli::ErrorKind::Runtime)
                return err.what();
            return std::string(kParseErrorPrefix) + render_parse_error_zh(err);
        }

        std::string_view save_error_kind_zh(save::SaveErrorKind kind)
        {
            switch (kind)
            {
            case save::SaveErrorKind::ParseError:
                return "JSON 非法";
            case save::SaveErrorKind::VersionMismatch:
                return "存档版本不符";
            case save::SaveErrorKind::DeckMismatch:
                return "牌表不符";
            case save::SaveErrorKind::StructureError:
                return "结构错误";
            case save::SaveErrorKind::RngError:
                return "随机源错误";
            }
            return "未知错误";
        }

        std::string_view io_error_zh(io::IoError e, bool writing)
        {
            switch (e)
            {
            case io::IoError::NotExist:
                return writing ? "父目录不存在" : "文件不存在";
            case io::IoError::NotAFile:
                return "不是常规文件";
            case io::IoError::Permission:
                return "无访问权限";
            case io::IoError::IoFailed:
                return "读写失败";
            }
            return "未知错误";
        }

        std::string render_save_error_zh(const save::SaveError &e)
        {
            return "存档加载失败（" + std::string(save_error_kind_zh(e.kind)) +
                   "）: " + e.detail;
        }

        std::string render_read_error_zh(
            const std::filesystem::path &file, io::IoError e)
        {
            return "读取存档失败（" + std::string(io_error_zh(e, false)) +
                   "）: " + file.string();
        }

        std::string render_write_error_zh(
            const std::filesystem::path &file, io::IoError e)
        {
            return "写入存档失败（" + std::string(io_error_zh(e, true)) +
                   "）: " + file.string();
        }

        std::string_view config_error_kind_zh(config::ConfigErrorKind kind)
        {
            switch (kind)
            {
            case config::ConfigErrorKind::FileNotFound:
                return "文件不存在";
            case config::ConfigErrorKind::IoFailed:
                return "文件读写失败";
            case config::ConfigErrorKind::ParseError:
                return "JSON 非法";
            case config::ConfigErrorKind::MissingField:
                return "缺少字段";
            case config::ConfigErrorKind::TypeMismatch:
                return "字段类型不符";
            case config::ConfigErrorKind::InvalidValue:
                return "字段值非法";
            }
            return "未知错误";
        }

        std::string_view loop_error_label_zh(game::LoopError code)
        {
            switch (code)
            {
            case game::LoopError::NoPlayers:
                return "无可用玩家";
            case game::LoopError::TurnFailed:
                return "回合流程失败";
            case game::LoopError::MaxRounds:
                return "达到最大回合数";
            }
            return "未知错误";
        }

        std::string_view turn_error_label_zh(game::TurnError e)
        {
            switch (e)
            {
            case game::TurnError::UnknownPlayer:
                return "角色不存在";
            case game::TurnError::UnknownCard:
                return "卡牌定义缺失";
            case game::TurnError::CardNotInHand:
                return "手牌中没有该牌";
            case game::TurnError::InvalidTarget:
                return "目标非法";
            case game::TurnError::ShaLimitExceeded:
                return "本回合杀已达上限";
            case game::TurnError::AnalepticLimitExceeded:
                return "本回合已使用过酒";
            case game::TurnError::NotEquipment:
                return "该牌不是装备";
            case game::TurnError::DelayedDuplicate:
                return "判定区已有同名延时锦囊";
            case game::TurnError::PlayRejected:
                return "出牌被拒绝";
            case game::TurnError::DiscardInsufficient:
                return "弃牌数量不足";
            case game::TurnError::JudgeEmptyDeck:
                return "判定时牌堆已空";
            }
            return "未知错误";
        }

        std::string loop_error_zh(game::LoopError code)
        {
            return "对局失败（" + std::string(loop_error_label_zh(code)) + "）";
        }

    }  // namespace cli
}  // namespace tkw
