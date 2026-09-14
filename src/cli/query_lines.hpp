/**
 * @file   query_lines.hpp
 * @brief  只读牌表查询的纯行构造。
 * @details `audit`/`cards`/`decks`/`heroes`/`rules` 的展示行，不含打印与 human
 *          策略；纯函数、无输出副作用，仅按 `opt.deck` 加载目录并构造行，不打印、
 *          不建局、不消耗随机源、不校验 humans（真人拒绝留在 CLI 命令包装层）。
 * @note   每行不含换行符，由消费方补 "\n"；CLI 与 TUI 共用同一行序，保证两处
 *         结果逐行一致。
 * @ingroup tkw_cli
 */
#ifndef INCLUDE_TKW_CLI_QUERY_LINES_HPP
#define INCLUDE_TKW_CLI_QUERY_LINES_HPP

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "card/catalog.hpp"
#include "cli/error_zh.hpp"
#include "cli/render.hpp"
#include "cli/session.hpp"
#include "config/resource.hpp"
#include "game/core/effect.hpp"
#include "game/resolve/audit.hpp"
#include "hero/catalog.hpp"
#include "io/file.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace cli
    {
        namespace detail
        {
            /** @brief 只读查询结果：`Ok` 为逐行文本（不含换行），`Err` 为中文加载错误文案。 */
            using QueryLines = tkw::Result<std::vector<std::string>, std::string>;

            /**
             * @brief 审计牌堆的纯展示行：牌表来源头 + 「牌堆全部可结算」或未实现卡清单。
             * @param[in] opt 对局选项；仅 deck 决定被审计的牌表目录。
             * @return Ok 为行序：`牌表: <dir>`，随后为「牌堆全部可结算」单行，或
             *         「未实现卡（N 张）:」+ 每卡「  <name>(<id>)」；Err 为
             *         format_load_error 拼出的中文加载错误。
             * @note 走容错扫描不建局：未知机制名逐卡列出而非整体拒载，与 CLI audit
             *       同口径。
             */
            QueryLines audit_lines(const Options &opt);

            /**
             * @brief 列出牌表的纯展示行：牌表来源头 + 牌堆名与种类/张数 + 逐卡行。
             * @param[in] opt       对局选项；仅 deck 决定被读取的牌表目录。
             * @param[in] show_text 为真时在每卡行末尾附 CardDef.text 效果文案。
             * @return Ok 为行序：`牌表: <dir>`、`牌堆[ <name>]（N 种 / M 张）`，再按
             *         deck 序每卡「  <name>(<id>) <类型> <张数>[: <text>]」；Err 为
             *         format_load_error 拼出的中文加载错误。
             * @note deck.json 的 name 缺失或类型不符时头行退化为无牌堆名，不阻断列出；
             *       只读查询不建局、不消耗随机源。
             */
            QueryLines cards_lines(const Options &opt, bool show_text);

            /**
             * @brief 可用牌表一览的纯展示行：扫描根目录自身与其直接子目录中含 deck.json 者。
             * @param[in] root 扫描根目录（通常为默认牌表 resources）。
             * @return Ok 为行序：`可用牌表（tkw decks [目录] 扫描；用 --deck <路径> 选择）:`，
             *         随后每副牌表一行「  <目录>  <牌堆名> <N> 种/<M> 张」，牌堆名缺失回落
             *         目录名；根目录不存在时 Err 为 format_load_error 的中文加载错误。
             * @note 只读取文件系统与牌表目录，不建局、不消耗随机源；直接子目录按路径字典序
             *       输出保证稳定。单个牌表加载失败只在该行渲染中文错误、不中断其余牌表列出。
             */
            QueryLines decks_lines(const std::filesystem::path &root);

            /**
             * @brief 可用武将一览的纯展示行：读取 <root>/heroes.json 与 heroes/<id>.json。
             * @param[in] root 牌表目录（武将数据与牌表同根，通常为 --deck）。
             * @return Ok 为行序：`可用武将（tkw heroes [目录] 扫描；随 --deck 选择）:`，
             *         随后每名武将一行「  <名>(<id>) <N>体力 <性别> 技能: <名…>」，
             *         未实现技能在其名后标「（未实现）」；无 heroes.json 回落
             *         「无武将数据（<dir>）」；Err 为坏 JSON/未知技能/未知性别等中文错误。
             * @note 只读加载不建局、不消耗随机源；缺文件不是错误，使无武将数据的
             *       自定义牌表照常列出。
             */
            QueryLines heroes_lines(const std::filesystem::path &root);

            /**
             * @brief 卡牌说明查询的纯展示行：牌表来源头 + 命中计数头 + 逐卡说明。
             * @param[in] opt     对局选项；仅 deck 决定被读取的牌表目录。
             * @param[in] keyword 过滤关键词；空串 = 列出全部。
             * @return Ok 为行序：`牌表: <dir>`、`卡牌说明（[匹配「keyword」的 ]N 种）:`，
             *         随后为「  没有匹配的卡牌说明。」或每卡
             *         「  <name>(<id>): <text>」；Err 为 format_load_error 拼出的
             *         中文加载错误。
             * @note 命中谓词为 name/id/text 三字段子串；文案缺失回落「（无说明）」占位
             *       且不跳过该卡，与 cards 列表面同口径。
             */
            QueryLines rules_lines(const Options &opt,
                                          const std::string &keyword);
        }  // namespace detail
    }  // namespace cli
}  // namespace tkw

#endif  // INCLUDE_TKW_CLI_QUERY_LINES_HPP
