/**
 * @file   command.hpp
 * @brief  TUI 命令栏纯解析：单行文本 → 结构化的会话命令。
 * @details 纯函数、无 FTXUI、无输出副作用：解析失败以 `CommandParseError`
 *          返回（判别类别 + 面向用户的中文提示），由控制器写入日志面板。行内
 *          选项以调用方传入的启动选项为基准继承（未显式给出的项沿用 `base`），
 *          与 REPL 行内命令继承启动选项同语义。
 * @ingroup tkw_tui
 */
#ifndef INCLUDE_TKW_TUI_COMMAND_HPP
#define INCLUDE_TKW_TUI_COMMAND_HPP

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pjh_cli/command/matcher.hpp>

#include "cli/session.hpp"
#include "game/core/rules.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace tui
    {
        /** @brief 命令栏支持的命令类别。 */
        enum class CommandKind : std::uint8_t
        {
            New,    /**< 开新对局（不立即推进）。 */
            Deal,   /**< 按位置参数开新局并直接跑到底。 */
            Step,   /**< 执行一个回合。 */
            Run,    /**< 跑到对局结束。 */
            Status, /**< 追加会话状态摘要（不触引擎）。 */
            Save,   /**< 显式存档到文件。 */
            Load,   /**< 从存档恢复会话。 */
            Quit,   /**< 退出 TUI。 */
            Help,   /**< 追加命令表。 */
            Cards,  /**< 列出牌表（结果写日志面板）。 */
            Rules,  /**< 查询卡牌说明（结果写日志面板）。 */
            Audit,  /**< 审计牌堆（结果写日志面板）。 */
            Decks,  /**< 列出可用牌表（结果写日志面板）。 */
            Heroes, /**< 列出可用武将（结果写日志面板）。 */
            Simulate, /**< 批量模拟全 AI 对局（结果写日志面板，worker 执行）。 */
        };

        /** @brief 解析后的命令值：类别 + new/deal 选项 + save/load 路径 + 查询参数。 */
        struct Command
        {
            CommandKind kind = CommandKind::Step;   /**< 命令类别。 */
            tkw::cli::Options options;              /**< new/deal：base 继承 + 行内覆盖。 */
            std::string file;                       /**< save/load 存档路径。 */
            bool run_to_end = false;                /**< deal/run 建局或起跑后跑到底。 */
            std::string keyword;                    /**< rules：过滤关键词；空 = 全部。 */
            bool with_text = false;                 /**< cards：附 CardDef.text 效果文案。 */
            bool deck_provided = false;             /**< 行内 --deck 是否显式给出（查询类）。 */
            int games = 0;                          /**< simulate：局数（≥1）。 */
        };

        /**
         * @brief 命令栏解析失败：判别类别 + 面向用户的中文提示。
         * @note  `detail` 逐字沿用解析器既有中文文案，供日志面板直接展示；`kind`
         *        供调用方/测试按类别判定，不解析文案。
         */
        struct CommandParseError
        {
            /** @brief 解析失败类别。 */
            enum class Kind : std::uint8_t
            {
                EmptyCommand,       /**< 空命令。 */
                UnknownCommand,     /**< 命令名不被识别（`detail` 可含建议）。 */
                UnknownOption,      /**< 未知选项。 */
                MissingValue,       /**< 选项缺少取值。 */
                InvalidValue,       /**< 选项/位置参数取值格式或值域非法。 */
                PlayerOutOfRange,   /**< 玩家数越界（值与合法范围在 `detail`）。 */
                MissingArgument,    /**< 缺少必需的位置/文件/局数参数。 */
                TooManyArguments,   /**< 位置/文件/关键词参数超过允许个数。 */
                UnexpectedArgument, /**< 命令不接受该参数（含多余位置参数）。 */
            };

            Kind kind = Kind::EmptyCommand; /**< 失败类别。 */
            std::string detail;             /**< 面向用户的中文提示文案。 */

            /**
             * @brief  比较两个错误值是否等价。
             * @param[in] other 另一错误值。
             * @return `true` 表示 `kind` 与 `detail` 均相等。
             */
            bool operator==(const CommandParseError &other) const = default;
        };

        /** @brief 解析结果：`Ok(Command)` 或 `Err(CommandParseError)`。 */
        using CommandParseResult = tkw::Result<Command, CommandParseError>;

        namespace detail
        {
            /**
             * @brief  去除首尾空白（空格与制表符）；全空白返回空串。
             * @param[in] text 待处理文本。
             * @return 去掉首尾空白后的子串视图，指向 `text` 底层缓冲。
             */
            std::string_view trim(std::string_view text);

            /**
             * @brief  按空白切分为 token 列表；输入假定已 trim。
             * @param[in] text 输入文本。
             * @return 按空白分隔的非空 token 列表。
             */
            std::vector<std::string> split_ws(std::string_view text);

            /**
             * @brief  严格解析十进制 `int`（整串消费、无前后缀）。
             * @param[in]  text 待解析文本。
             * @param[out] out  解析成功时写入结果。
             * @return 解析成功返回 true。
             * @retval true  整串均为合法十进制且可放入 `int`。
             * @retval false 空串、含非法字符或溢出；`out` 不被修改。
             */
            bool parse_i32(std::string_view text, int &out);

            /**
             * @brief  严格解析十进制 `uint32`（拒绝负号与溢出）。
             * @param[in]  text 待解析文本。
             * @param[out] out  解析成功时写入结果。
             * @return 解析成功返回 true。
             * @retval true  整串均为合法非负十进制且不超过 `uint32` 上限。
             * @retval false 空串、负号、含非法字符或溢出；`out` 不被修改。
             */
            bool parse_u32(std::string_view text, std::uint32_t &out);

            /**
             * @brief  选项值域字符串 → 对局模式。
             * @param[in]  name 选项值（`brawl`/`identity`）。
             * @param[out] out  命中时写入模式。
             * @return 命中返回 true；未命中返回 false 且不写 `out`。
             * @note  与 `src/cli/commands.cpp` 的 `--mode` 映射同为输入文本契约，
             *        两处须保持一致；与存档文本（`save::mode_name`/`mode_from`）
             *        分属独立契约，不得合并。
             */
            bool mode_from(std::string_view name, tkw::game::GameMode &out);

            /**
             * @brief  玩家数越界提示：值与合法范围同 REPL 文案口径。
             * @param[in] value 越界的玩家数。
             * @return 中文提示文本。
             */
            std::string player_range_error(int value);

            /**
             * @brief  可选值缺失提示。
             * @param[in] option 缺少取值的选项名。
             * @return 中文提示文本。
             */
            std::string missing_value_error(const std::string &option);

            /**
             * @brief  未知选项提示。
             * @param[in] option 未知选项名。
             * @return 中文提示文本。
             */
            std::string unknown_option_error(const std::string &option);

            /**
             * @brief  只读查询命令识别并消费行内 `--deck`。
             * @param[in]     tokens 全 token 列表。
             * @param[in,out] i      token 下标引用；命中时前移到值 token 并消费。
             * @param[in,out] cmd    目标命令；命中时写入 `options.deck` 并置
             *                       `deck_provided`。
             * @return 非 `--deck` token 返回 `Ok(false)`；命中且取值成功返回
             *         `Ok(true)`；命中但缺值返回 `Err`（「需要一个值」，与
             *         new/CLI 同文案）。
             * @retval Ok(false) 当前 token 不是 `--deck`。
             * @retval Ok(true)  已消费 `--deck` 及其值。
             * @retval Err       `--deck` 后缺值（`MissingValue`）；`i` 与 `cmd`
             *                   不被修改。
             */
            tkw::Result<bool, CommandParseError> take_query_deck(
                const std::vector<std::string> &tokens, std::size_t &i,
                Command &cmd);

            /**
             * @brief  解析 new 的行内选项到 `cmd.options`；失败返回中文文案。
             * @param[in] tokens 全 token 列表（`tokens[0] == "new"`）。
             * @param[in] base   启动选项；未显式覆盖的项沿用此基准。
             * @return 成功返回 `Ok(Command)`；未知选项/缺值/类型或越界返回 `Err`。
             */
            CommandParseResult parse_new(
                const std::vector<std::string> &tokens,
                const tkw::cli::Options &base);

            /**
             * @brief  解析 deal 的两个位置参数 `<players>` 与 `<seed>`。
             * @param[in] tokens 全 token 列表（`tokens[0] == "deal"`）。
             * @param[in] base   启动选项；两位置参数覆盖 players/seed。
             * @return 成功返回 `Ok(Command{Deal, run_to_end=true})`；参数个数、
             *         类型或范围非法返回中文 `Err`。
             */
            CommandParseResult parse_deal(
                const std::vector<std::string> &tokens,
                const tkw::cli::Options &base);

            /**
             * @brief  解析 cards 的可选 `--text` 与行内 `--deck`；其余 token 报错。
             * @param[in] tokens 全 token 列表（`tokens[0] == "cards"`）。
             * @return 成功返回 `Ok(Command{Cards})`；未知 token/缺值返回中文 `Err`。
             */
            CommandParseResult parse_cards(
                const std::vector<std::string> &tokens);

            /**
             * @brief  解析 rules 的可选关键词（至多一个）与行内 `--deck`。
             * @param[in] tokens 全 token 列表（`tokens[0] == "rules"`）。
             * @return 成功返回 `Ok(Command{Rules, keyword})`；重复关键词/未知选项/
             *         缺值返回中文 `Err`。
             * @note   选项与关键词可任意顺序混写。
             */
            CommandParseResult parse_rules(
                const std::vector<std::string> &tokens);

            /**
             * @brief  解析 audit 的可选行内 `--deck`；其余 token 报错。
             * @param[in] tokens 全 token 列表（`tokens[0] == "audit"`）。
             * @return 成功返回 `Ok(Command{Audit})`；未知 token/缺值返回中文 `Err`。
             */
            CommandParseResult parse_audit(
                const std::vector<std::string> &tokens);

            /**
             * @brief  解析 decks 的可选行内 `--deck`（扫描根目录）；其余 token 报错。
             * @param[in] tokens 全 token 列表（`tokens[0] == "decks"`）。
             * @return 成功返回 `Ok(Command{Decks})`；未知 token/缺值返回中文 `Err`。
             * @note   扫描根缺省与 cards/rules/audit 同源（行内 `--deck` > 活动
             *         会话 > 启动），故 decks 无参数即列出当前牌表所在目录的内置
             *         牌表。
             */
            CommandParseResult parse_decks(
                const std::vector<std::string> &tokens);

            /**
             * @brief  解析 heroes 的可选行内 `--deck`（武将数据根目录）；其余
             *         token 报错。
             * @param[in] tokens 全 token 列表（`tokens[0] == "heroes"`）。
             * @return 成功返回 `Ok(Command{Heroes})`；未知 token/缺值返回中文 `Err`。
             * @note   武将数据与牌表同根，缺省目录与 cards/rules/audit/decks 同源
             *         （行内 `--deck` > 活动会话 > 启动），故 heroes 无参数即列出
             *         当前牌表目录的 `heroes.json`。
             */
            CommandParseResult parse_heroes(
                const std::vector<std::string> &tokens);

            /**
             * @brief  解析 `simulate <局数> [玩家数]` 与行内
             *         `--seed`/`--ai`/`--mode`/`--hand`/`--deck`。
             * @param[in] tokens 全 token 列表（`tokens[0] == "simulate"`）。
             * @param[in] base   启动选项；未显式给出的项沿用 `base`，但 seed 基值
             *                   固定为 1。
             * @return `Ok(Command{Simulate, games, options})`；缺局数/局数非正/
             *         玩家数越界或非法/多余位置参数/未知选项返回中文 `Err`。
             * @note   基种子缺省 1（局种子 1..N）对齐 CLI simulate，与
             *         `Options.seed` 缺省 42 不同；行内 `--deck` 置
             *         `deck_provided`，控制器据此覆盖活动会话/启动牌表。
             */
            CommandParseResult parse_simulate(
                const std::vector<std::string> &tokens,
                const tkw::cli::Options &base);

            /**
             * @brief  解析 `help`/`?` 的可选关键词（至多一个）。
             * @param[in] name   实际输入的命令名（`help` 或 `?`），用于参数错误文案。
             * @param[in] tokens 全 token 列表（`tokens[0]` 为命令名）。
             * @return `Ok(Command{Help, keyword})`；多于一个参数返回 `Err`。
             * @note   关键词经 `Command::keyword` 承载，由 `query_help_lines` 过滤；
             *         空 = 全量表。
             */
            CommandParseResult parse_help(
                const std::string &name,
                const std::vector<std::string> &tokens);

            /**
             * @brief  解析单文件参数命令（save/load）。
             * @param[in] kind   目标命令类别。
             * @param[in] name   命令名，用于错误文案。
             * @param[in] tokens 全 token 列表（`tokens[0]` 为命令名）。
             * @return `Ok(Command{kind, file})`；缺文件或多于一个文件参数返回 `Err`。
             */
            CommandParseResult parse_file_command(
                CommandKind kind, const std::string &name,
                const std::vector<std::string> &tokens);

            /**
             * @brief  解析不接受参数的命令（step/run/status/quit/help）。
             * @tparam T 成功时的结果值类型。
             * @param[in] name   命令名，用于错误文案。
             * @param[in] tokens 全 token 列表（`tokens[0]` 为命令名）。
             * @param[in] value  无多余参数时返回的值。
             * @return `Ok(value)`；存在多余参数返回 `Err`
             *         （`UnexpectedArgument`）。
             */
            template <typename T>
            inline tkw::Result<T, CommandParseError> no_args(
                const std::string &name, const std::vector<std::string> &tokens,
                T value)
            {
                if (tokens.size() > 1)
                    return tkw::Result<T, CommandParseError>::Err(
                        CommandParseError{
                            CommandParseError::Kind::UnexpectedArgument,
                            name + " 不接受参数"});
                return tkw::Result<T, CommandParseError>::Ok(std::move(value));
            }

            /**
             * @brief 命令栏可识别的命令名与别名表。
             * @return 规范名与别名的有序列表（命令表序），供 did-you-mean 建议使用。
             * @note 与 `parse_command` 的分派面保持同步；simulate 作为可识别名字
             *       参与纠错，避免手误时无候选。
             */
            const std::vector<std::string> &command_names();

            /**
             * @brief  对未知命令名生成 did-you-mean 候选。
             * @param[in] token 用户输入的未知命令名（已 trim）。
             * @return 编辑距离不超过阈值的最近 1–3 个候选（距离升序、同距按命令
             *         表序）；无候选返回空。
             * @note   阈值 2 覆盖插入/删除/替换等常见手误；建议只用于提示，不参与
             *         命令分派。长度差即编辑距离下界，先据此跳过不可能命中的候选。
             */
            std::vector<std::string> suggest_commands(std::string_view token);

            /**
             * @brief  ASCII 大小写不敏感的子串查找。
             * @param[in] haystack 被查找文本（含中文时按字节原样比较）。
             * @param[in] needle   关键词；空串视为命中。
             * @return 命中返回 true。
             */
            bool contains_ci(std::string_view haystack, std::string_view needle);

            /**
             * @brief TUI 命令表/用法纯文本：命令+别名、只读查询、默认值、启动选项、键位。
             * @return 逐行文本（不含换行）；`Controller::do_help` 与启动 `--help/-h`
             *         共用同一来源，保证两入口逐行一致。
             * @note 默认值取自 Options/RulesConfig 声明，不写魔法数；事件日志恒开
             *       且无 --verbose 开关，在此明示避免与 CLI 混淆。
             */
            std::vector<std::string> help_lines();

            /**
             * @brief  按关键词过滤命令表。
             * @param[in] keyword 关键词；去空白后为空时返回未过滤的全量表（逐字节
             *                    与 `help_lines()` 一致）。
             * @return 命中行（ASCII 大小写不敏感子串匹配，中文按字节匹配）；
             *         无命中返回单行中文提示。
             * @note   过滤只挑选既有行、不重写文案，全量表断言不因本函数漂移；
             *         命令表仍由 `help_lines()` 单一维护。
             */
            std::vector<std::string> query_help_lines(std::string_view keyword);
        }  // namespace detail

        /**
         * @brief  解析一行命令文本。
         * @param[in] line 用户输入的单行文本（首尾空白忽略）。
         * @param[in] base 启动选项；new/deal 未显式给出的项沿用此基准。
         * @return `Ok(Command)`；`Err(CommandParseError)`（未知命令/缺参/类型
         *         或越界/未知选项）。
         * @note   命令名与别名：run/r、status/st、save/w、load/l、quit/q、help/?。
         *         save/load 只取一个文件位置参数；new 只接受行内长选项与
         *         `--human`/`--no-human`/`--hero`（座位=武将，可重复，行内覆盖
         *         启动值），不接受位置参数；cards 接受可选 `--text` 与
         *         `--deck <路径>`，rules 接受至多一个关键词与 `--deck <路径>`，
         *         audit 接受 `--deck <路径>` 且无其它参数，decks/heroes 只接受
         *         `--deck <路径>`；help/? 接受至多一个关键词用于过滤命令表；
         *         simulate 接受 `<局数> [玩家数]` 与行内
         *         `--seed`/`--ai`/`--mode`/`--hand`/`--deck`，基种子缺省 1。
         *         未知命令名附邻近拼写建议。
         */
        CommandParseResult parse_command(std::string_view line,
                                         const tkw::cli::Options &base);
    }  // namespace tui
}  // namespace tkw

#endif  // INCLUDE_TKW_TUI_COMMAND_HPP
