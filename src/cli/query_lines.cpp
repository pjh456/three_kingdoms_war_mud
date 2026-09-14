/**
 * @file   query_lines.cpp
 * @brief  只读牌表查询行构造的定义。
 * @ingroup tkw_cli
 */

#include "cli/query_lines.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace tkw
{
    namespace cli
    {
        namespace detail
        {
            QueryLines audit_lines(const Options &opt)
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

            QueryLines cards_lines(const Options &opt, bool show_text)
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

            QueryLines decks_lines(const std::filesystem::path &root)
            {
                if (!tkw::io::exists(root))
                    return QueryLines::Err(format_load_error(tkw::config::ConfigError{
                        tkw::config::ConfigErrorKind::FileNotFound,
                        (root / "deck.json").string()}));

                // 候选：根目录自身（根也是牌表时）+ 含 deck.json 的直接子目录，子目录按路径排序。
                std::vector<std::filesystem::path> candidates;
                if (tkw::io::exists(root / "deck.json"))
                    candidates.push_back(root);
                std::vector<std::filesystem::path> subs;
                std::error_code ec;
                for (std::filesystem::directory_iterator it(root, ec), end;
                     !ec && it != end; it.increment(ec))
                {
                    if (!it->is_directory(ec))
                        continue;
                    const std::filesystem::path &dir = it->path();
                    if (tkw::io::exists(dir / "deck.json"))
                        subs.push_back(dir);
                }
                std::sort(subs.begin(), subs.end());
                candidates.insert(candidates.end(), subs.begin(), subs.end());

                std::vector<std::string> lines;
                lines.push_back(
                    "可用牌表（tkw decks [目录] 扫描；用 --deck <路径> 选择）:");
                if (candidates.empty())
                {
                    lines.push_back("  未发现含 deck.json 的牌表目录");
                    return QueryLines::Ok(std::move(lines));
                }

                for (const auto &dir : candidates)
                {
                    tkw::config::ResourceStore store(dir);
                    auto catalog = tkw::card::CardDefCatalog::load(store, "deck");
                    if (catalog.is_err())
                    {
                        lines.push_back("  " + dir.string() + "  " +
                                        format_load_error(catalog.unwrap_err()));
                        continue;
                    }
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
                    if (deck_name.empty())
                        deck_name = dir.filename().string();

                    lines.push_back("  " + dir.string() + "  " + deck_name + " " +
                                    std::to_string(cat.size()) + " 种/" +
                                    std::to_string(cat.total_copies()) + " 张");
                }
                return QueryLines::Ok(std::move(lines));
            }

            QueryLines heroes_lines(const std::filesystem::path &root)
            {
                tkw::config::ResourceStore store(root);
                auto catalog = tkw::hero::HeroCatalog::load_optional(store, "heroes");
                if (catalog.is_err())
                    return QueryLines::Err(
                        format_load_error(catalog.unwrap_err()));
                const auto &cat = catalog.unwrap();

                std::vector<std::string> lines;
                lines.push_back(
                    "可用武将（tkw heroes [目录] 扫描；随 --deck 选择）:");
                if (cat.empty())
                {
                    lines.push_back("无武将数据（" + root.string() + "）");
                    return QueryLines::Ok(std::move(lines));
                }

                for (const auto &def : cat)
                {
                    std::string line = "  " + tkw::hero::display_hero_name(def) +
                                       "(" + def.id + ")";
                    if (def.hp > 0)
                        line += " " + std::to_string(def.hp) + "体力";
                    if (def.gender.is_some())
                        line += def.gender.unwrap() == tkw::entity::Gender::Male
                                    ? " 男"
                                    : " 女";
                    if (!def.skills.empty())
                    {
                        line += " 技能: ";
                        for (std::size_t i = 0; i < def.skills.size(); ++i)
                        {
                            if (i != 0)
                                line += "、";
                            line += tkw::hero::display_skill_name(def.skills[i]);
                            if (tkw::game::is_unimplemented_skill(def.skills[i]))
                                line += "（未实现）";
                        }
                    }
                    lines.push_back(std::move(line));
                }
                return QueryLines::Ok(std::move(lines));
            }

            QueryLines rules_lines(const Options &opt,
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
