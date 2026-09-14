/**
 * @file   writer.hpp
 * @brief  对局状态 → 存档 JSON 文本。
 * @details 手工拼装 JSON（格式完全受控，字符串统一转义）；读取走 pjh_json。
 * @ingroup tkw_save
 */

#ifndef INCLUDE_TKW_SAVE_WRITER_HPP
#define INCLUDE_TKW_SAVE_WRITER_HPP

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "card/card.hpp"
#include "card/catalog.hpp"
#include "card/def.hpp"
#include "entity/manager.hpp"
#include "game/core/roles.hpp"
#include "game/core/rules.hpp"
#include "game/flow/loop.hpp"
#include "game/flow/table.hpp"
#include "save/deck_hash.hpp"
#include "save/format.hpp"
#include "save/session_meta.hpp"

namespace tkw
{
    namespace save
    {

        /**
         * @brief  JSON 字符串转义。
         * @param[in] s 原始字符串。
         * @return 转义 `"`、`\\` 与控制字符后的字符串。
         */
        inline std::string escape_json(std::string_view s)
        {
            std::string out;
            out.reserve(s.size() + 2);
            for (char ch : s)
            {
                const auto c = static_cast<unsigned char>(ch);
                switch (ch)
                {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (c < 0x20)
                    {
                        char buf[7];
                        std::snprintf(buf, sizeof buf, "\\u%04x", c);
                        out += buf;
                    }
                    else
                        out += ch;
                }
            }
            return out;
        }

        /**
         * @brief  将字符串序列化为带引号的 JSON 字符串字面量。
         * @param[in] s 原始字符串。
         * @return `"..."`，内部已转义。
         */
        inline std::string jstr(std::string_view s)
        {
            return "\"" + escape_json(s) + "\"";
        }

        /**
         * @brief  将单张牌序列化为 JSON 对象。
         * @param[in] c 待序列化的牌。
         * @return 含 `iid`/`def`/`suit`/`number` 的 JSON 对象文本。
         */
        inline std::string card_json(const card::Card &c)
        {
            std::ostringstream os;
            os << "{\"iid\":" << jstr(c.instance_id) << ",\"def\":" << jstr(c.def_id)
               << ",\"suit\":" << jstr(suit_name(c.suit))
               << ",\"number\":" << c.number << "}";
            return os.str();
        }

        /**
         * @brief  将牌列表序列化为 JSON 数组。
         * @param[in] cards 待序列化的牌列表；空列表输出 `[]`。
         * @return JSON 数组文本，元素顺序与 `cards` 一致。
         */
        inline std::string cards_json(const std::vector<card::Card> &cards)
        {
            std::string out = "[";
            for (std::size_t i = 0; i < cards.size(); ++i)
            {
                if (i)
                    out += ",";
                out += card_json(cards[i]);
            }
            out += "]";
            return out;
        }

        namespace detail
        {
            /**
             * @brief  序列化 string→int 映射。
             * @param[in] m 待序列化的映射；`std::map` 迭代有序，输出确定。
             * @return JSON 对象文本。
             */
            inline std::string int_map_json(const std::map<std::string, int> &m)
            {
                std::string out = "{";
                bool first = true;
                for (const auto &[k, v] : m)
                {
                    if (!first)
                        out += ",";
                    first = false;
                    out += jstr(k) + ":" + std::to_string(v);
                }
                out += "}";
                return out;
            }

            /**
             * @brief  序列化 string→string 映射。
             * @param[in] m 待序列化的映射；`std::map` 迭代有序，输出确定。
             * @return JSON 对象文本。
             */
            inline std::string str_map_json(
                const std::map<std::string, std::string> &m)
            {
                std::string out = "{";
                bool first = true;
                for (const auto &[k, v] : m)
                {
                    if (!first)
                        out += ",";
                    first = false;
                    out += jstr(k) + ":" + jstr(v);
                }
                out += "}";
                return out;
            }

            /**
             * @brief  序列化 id→`Role` 映射。
             * @param[in] m 待序列化的角色表；`std::map` 迭代有序，输出确定。
             * @return JSON 对象文本，值为角色文本。
             */
            inline std::string role_map_json(const game::RoleTable &m)
            {
                std::string out = "{";
                bool first = true;
                for (const auto &[k, v] : m)
                {
                    if (!first)
                        out += ",";
                    first = false;
                    out += jstr(k) + ":" + jstr(role_name(v));
                }
                out += "}";
                return out;
            }

            /**
             * @brief  序列化 string 集合为 JSON 数组。
             * @param[in] s 待序列化的集合；`std::set` 迭代有序，输出确定。
             * @return JSON 数组文本。
             */
            inline std::string str_set_json(const std::set<std::string> &s)
            {
                std::string out = "[";
                bool first = true;
                for (const auto &v : s)
                {
                    if (!first)
                        out += ",";
                    first = false;
                    out += jstr(v);
                }
                out += "]";
                return out;
            }

            /**
             * @brief  统计是否有任一非空字段。
             * @param[in] stats 待检查的统计聚合。
             * @return 是否有字段非空；全空表示未写，序列化时省略以保持规范往返。
             */
            inline bool has_any_stats(const BattleStats &stats)
            {
                return !stats.damage_dealt.empty() || !stats.healing.empty() ||
                       !stats.kills.empty() || !stats.last_hit_source.empty() ||
                       !stats.died.empty();
            }
        }

        /**
         * @brief  序列化整局（含会话进度与可选元数据）到 JSON 文本。
         * @details 按实际用到的最高格式特性动态写出版本号：有武将写
         *          `kVersionHeroes`，仅有连环写 `kVersionChained`，否则写
         *          `kVersion`。牌表指纹按 int64 位型写出，高位指纹落成负十进制，
         *          读取端逐位还原。
         * @param[in] g         对局运行时；只读，本接口不修改。
         * @param[in] session   会话进度。
         * @param[in] deck_name 牌表名称，写入 `deck.name`。
         * @param[in] meta      会话元数据；ai 空、stats 全空时不写对应字段，保证
         *                      默认元数据的规范化往返结果不因新增字段而变化。
         * @return 完整、自包含的存档 JSON 文本。
         * @post  本接口不改变任何状态；落盘原子性由调用方负责。
         * @note  身份局才写出 `mode` 与 `roles`；乱斗不写新键，保持旧档逐字节不变。
         * @see   save::read
         */
        inline std::string write(
            const game::Game &g, const game::GameSession &session,
            const std::string &deck_name, const SessionMeta &meta = {})
        {
            const auto &r = g.rules;
            const auto snap = g.cards.snapshot();
            const auto ents = g.entities.snapshot();

            // 动态版本：取实际用到的最高格式特性——有武将写 v3，仅有连环写 v2，
            // 否则保持 v1 旧格式
            const bool any_chained =
                std::any_of(ents.begin(), ents.end(),
                            [](const EntitySnapshot &e) { return e.chained; });
            const bool any_hero =
                std::any_of(ents.begin(), ents.end(),
                            [](const EntitySnapshot &e) { return !e.hero.empty(); });
            const int version = any_hero    ? kVersionHeroes
                                : any_chained ? kVersionChained
                                              : kVersion;

            std::ostringstream os;
            os << "{\"format\":" << jstr(kFormat)
               << ",\"version\":" << version;
            // 指纹按 int64 位型写出：JSON 数值只有 int64 精确域，高位指纹
            // （≥2^63）须落成负十进制，读取端再逐位还原为 uint64。
            os << ",\"deck\":{\"name\":" << jstr(deck_name)
               << ",\"hash\":" << static_cast<std::int64_t>(deck_hash(g.catalog))
               << "}";
            os << ",\"rules\":{"
               << "\"draw_per_turn\":" << r.draw_per_turn
               << ",\"sha_limit\":" << r.sha_limit
               << ",\"kill_reward\":" << r.kill_reward
               << ",\"initial_hand\":" << r.initial_hand
               << ",\"max_turns\":" << r.max_turns
               << ",\"dying_rounds\":" << r.dying_rounds
               << ",\"wuxie_rounds\":" << r.wuxie_rounds
               << ",\"duel_rounds\":" << r.duel_rounds
               << ",\"base_hp\":" << r.base_hp
               << ",\"min_players\":" << r.min_players
               << ",\"max_players\":" << r.max_players << "}";
            os << ",\"rng\":{\"data\":" << jstr(g.rng ? g.rng->save_state().data : "")
               << "}";
            os << ",\"session\":{\"current\":" << jstr(session.current)
               << ",\"turns\":" << session.turns
               << ",\"started\":" << (session.started ? "true" : "false");
            if (!meta.ai.empty())
                os << ",\"ai\":" << jstr(meta.ai);
            if (detail::has_any_stats(meta.stats))
                os << ",\"stats\":{"
                   << "\"damage_dealt\":"
                   << detail::int_map_json(meta.stats.damage_dealt)
                   << ",\"healing\":" << detail::int_map_json(meta.stats.healing)
                   << ",\"kills\":" << detail::int_map_json(meta.stats.kills)
                   << ",\"last_hit_source\":"
                   << detail::str_map_json(meta.stats.last_hit_source)
                   << ",\"died\":" << detail::str_set_json(meta.stats.died) << "}";
            os << "}";
            // 身份局才写出模式与角色；乱斗不写新键，保持旧档逐字节不变
            if (g.mode == game::GameMode::Identity)
            {
                os << ",\"mode\":" << jstr(mode_name(g.mode));
                os << ",\"roles\":" << detail::role_map_json(g.roles);
            }
            os << ",\"cards\":{\"instance_seq\":" << snap.instance_seq
               << ",\"draw\":" << cards_json(snap.draw)
               << ",\"discard\":" << cards_json(snap.discard);

            auto zone_obj = [&](const char *name,
                                const std::vector<std::pair<
                                    std::string, std::vector<card::Card>>> &zones)
            {
                os << ",\"" << name << "\":{";
                for (std::size_t i = 0; i < zones.size(); ++i)
                {
                    if (i)
                        os << ",";
                    os << jstr(zones[i].first) << ":" << cards_json(zones[i].second);
                }
                os << "}";
            };
            zone_obj("hand", snap.hand);
            zone_obj("equip", snap.equip);
            zone_obj("judge", snap.judge);
            os << "}";  // cards

            os << ",\"entities\":[";
            for (std::size_t i = 0; i < ents.size(); ++i)
            {
                if (i)
                    os << ",";
                os << "{\"id\":" << jstr(ents[i].id) << ",\"seat\":" << ents[i].seat
                    << ",\"hp\":" << ents[i].hp << ",\"max_hp\":" << ents[i].max_hp
                    << ",\"gender\":" << jstr(gender_name(ents[i].gender));
                // 可选字段：仅为真/非空时写出，未横置且无武将时标准档逐字节不变
                if (ents[i].chained)
                    os << ",\"chained\":true";
                if (!ents[i].hero.empty())
                    os << ",\"hero\":" << jstr(ents[i].hero);
                os << "}";
            }
            os << "]}";
            return os.str();
        }
    }
}

#endif  // INCLUDE_TKW_SAVE_WRITER_HPP
