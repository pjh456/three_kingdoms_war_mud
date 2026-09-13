/**
 * @file query_lines.hpp
 * @brief 只读牌表查询的纯行构造：audit/cards/rules 的展示行（不含打印与 human 策略）。
 * @note 纯函数、无输出副作用：仅按 opt.deck 加载目录并构造行，不打印、不建局、
 *       不消耗随机源、不校验 humans（真人拒绝留在 CLI 命令包装层）。每行不含换行
 *       符，由消费方补 "\n"；CLI 与 TUI 共用同一行序，保证两处结果逐行一致。
 */
#ifndef INCLUDE_TKW_CLI_QUERY_LINES_HPP
#define INCLUDE_TKW_CLI_QUERY_LINES_HPP

#include <string>
#include <utility>
#include <vector>

#include "card/catalog.hpp"
#include "cli/error_zh.hpp"
#include "cli/render.hpp"
#include "cli/session.hpp"
#include "config/resource.hpp"
#include "game/resolve/audit.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace cli
    {
        namespace detail
        {
            /** 只读查询结果：Ok 为逐行文本（不含换行），Err 为中文加载错误文案。 */
            using QueryLines = tkw::Result<std::vector<std::string>, std::string>;

            /**
             * @brief 审计牌堆的纯展示行：牌表来源头 + 「牌堆全部可结算」或未实现卡清单。
             * @param opt 对局选项；仅 deck 决定被审计的牌表目录。
             * @return Ok 为行序：`牌表: <dir>`，随后为「牌堆全部可结算」单行，或
             *         「未实现卡（N 张）:」+ 每卡「  <name>(<id>)」；Err 为
             *         format_load_error 拼出的中文加载错误。
             * @note 走容错扫描不建局：未知机制名逐卡列出而非整体拒载，与 CLI audit
             *       同口径。
             */
            inline QueryLines audit_lines(const Options &opt)
            {
                tkw::config::ResourceStore store(opt.deck);
                auto unsupported = tkw::game::unsupported_cards(store, "deck");
                if (unsupported.is_err())
                    return QueryLines::Err(
                        format_load_error(unsupported.unwrap_err()));
                const auto &cards = unsupported.unwrap();

                std::vector<std::string> lines;
                lines.push_back("牌表: " + opt.deck.string());
                if (cards.empty())
                {
                    lines.push_back("牌堆全部可结算");
                    return QueryLines::Ok(std::move(lines));
                }
                lines.push_back("未实现卡（" + std::to_string(cards.size()) +
                                " 张）:");
                for (const auto &c : cards)
                    lines.push_back("  " + c.name + "(" + c.id + ")");
                return QueryLines::Ok(std::move(lines));
            }

            /**
             * @brief 列出牌表的纯展示行：牌表来源头 + 牌堆名与种类/张数 + 逐卡行。
             * @param opt       对局选项；仅 deck 决定被读取的牌表目录。
             * @param show_text 为真时在每卡行末尾附 CardDef.text 效果文案。
             * @return Ok 为行序：`牌表: <dir>`、`牌堆[ <name>]（N 种 / M 张）`，再按
             *         deck 序每卡「  <name>(<id>) <类型> <张数>[: <text>]」；Err 为
             *         format_load_error 拼出的中文加载错误。
             * @note deck.json 的 name 缺失或类型不符时头行退化为无牌堆名，不阻断列出；
             *       只读查询不建局、不消耗随机源。
             */
            inline QueryLines cards_lines(const Options &opt, bool show_text)
            {
                tkw::config::ResourceStore store(opt.deck);
                auto catalog = tkw::card::CardDefCatalog::load(store, "deck");
                if (catalog.is_err())
                    return QueryLines::Err(
                        format_load_error(catalog.unwrap_err()));
                const auto &cat = catalog.unwrap();

                std::string deck_name;
                const auto deck_doc = store.load("deck");
                if (deck_doc.is_ok())
                {
                    const auto nm = tkw::config::opt_string(
                        deck_doc.unwrap().root(), "name", "", "deck");
                    if (nm.is_ok())
                        deck_name = nm.unwrap();
                }

                std::vector<std::string> lines;
                lines.push_back("牌表: " + opt.deck.string());
                lines.push_back("牌堆" +
                                (deck_name.empty() ? "" : " " + deck_name) +
                                "（" + std::to_string(cat.size()) + " 种 / " +
                                std::to_string(cat.total_copies()) + " 张）");
                for (const auto &def : cat)
                {
                    std::string line = "  " + def.name + "(" + def.id + ") " +
                                       card_type_zh(def.type) + ' ' +
                                       std::to_string(def.copies.size());
                    if (show_text)
                        line += ": " + card_text_of(def);
                    lines.push_back(std::move(line));
                }
                return QueryLines::Ok(std::move(lines));
            }

            /**
             * @brief 卡牌说明查询的纯展示行：牌表来源头 + 命中计数头 + 逐卡说明。
             * @param opt     对局选项；仅 deck 决定被读取的牌表目录。
             * @param keyword 过滤关键词；空串 = 列出全部。
             * @return Ok 为行序：`牌表: <dir>`、`卡牌说明（[匹配「keyword」的 ]N 种）:`，
             *         随后为「  没有匹配的卡牌说明。」或每卡
             *         「  <name>(<id>): <text>」；Err 为 format_load_error 拼出的
             *         中文加载错误。
             * @note 命中谓词为 name/id/text 三字段子串；文案缺失回落「（无说明）」占位
             *       且不跳过该卡，与 cards 列表面同口径。
             */
            inline QueryLines rules_lines(const Options &opt,
                                          const std::string &keyword)
            {
                tkw::config::ResourceStore store(opt.deck);
                auto catalog = tkw::card::CardDefCatalog::load(store, "deck");
                if (catalog.is_err())
                    return QueryLines::Err(
                        format_load_error(catalog.unwrap_err()));
                const auto &cat = catalog.unwrap();

                std::vector<std::string> body;
                for (const auto &def : cat)
                {
                    if (!keyword.empty() &&
                        def.name.find(keyword) == std::string::npos &&
                        def.id.find(keyword) == std::string::npos &&
                        def.text.find(keyword) == std::string::npos)
                        continue;
                    body.push_back("  " + def.name + "(" + def.id + "): " +
                                   card_text_of(def));
                }

                std::vector<std::string> lines;
                lines.push_back("牌表: " + opt.deck.string());
                if (keyword.empty())
                    lines.push_back("卡牌说明（" + std::to_string(body.size()) +
                                    " 种）:");
                else
                    lines.push_back("卡牌说明（匹配「" + keyword + "」的 " +
                                    std::to_string(body.size()) + " 种）:");
                if (body.empty())
                    lines.push_back("  没有匹配的卡牌说明。");
                else
                    for (auto &line : body)
                        lines.push_back(std::move(line));
                return QueryLines::Ok(std::move(lines));
            }
        }  // namespace detail
    }  // namespace cli
}  // namespace tkw

#endif  // INCLUDE_TKW_CLI_QUERY_LINES_HPP
