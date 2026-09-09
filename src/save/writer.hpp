/**
 * @file writer.hpp
 * @brief 对局状态 → 存档 JSON 文本。
 * @note 手工拼装 JSON（格式完全受控，字符串统一转义）；读取走 pjh_json。
 */

#ifndef INCLUDE_TKW_SAVE_WRITER_HPP
#define INCLUDE_TKW_SAVE_WRITER_HPP

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "card/card.hpp"
#include "card/catalog.hpp"
#include "card/def.hpp"
#include "entity/manager.hpp"
#include "game/flow/loop.hpp"
#include "game/core/rules.hpp"
#include "game/flow/table.hpp"
#include "save/format.hpp"

namespace tkw
{
    namespace save
    {
        namespace detail
        {
            inline void hash_bytes(std::uint64_t &h, std::string_view s)
            {
                for (unsigned char c : s)
                {
                    h ^= c;
                    h *= 1099511628211ULL;
                }
            }

            inline void hash_int(std::uint64_t &h, std::int64_t v)
            {
                hash_bytes(h, std::to_string(v));
                hash_bytes(h, "|");
            }
        }

        /** @brief 牌表指纹：对目录语义字段做 FNV-1a（读档校验一致性）。 */
        inline std::uint64_t deck_hash(const card::CardDefCatalog &catalog)
        {
            std::uint64_t h = 1469598103934665603ULL;
            for (const auto &def : catalog)
            {
                detail::hash_bytes(h, def.id);
                detail::hash_bytes(h, "|");
                detail::hash_int(h, static_cast<int>(def.type));
                detail::hash_bytes(h, def.subtype);
                detail::hash_bytes(h, "|");
                detail::hash_int(h, def.rescue ? 1 : 0);
                detail::hash_int(h, def.counter ? 1 : 0);
                for (const auto &c : def.copies)
                {
                    detail::hash_int(h, static_cast<int>(c.suit));
                    detail::hash_int(h, c.number);
                }
                if (def.effect.is_some())
                {
                    const auto &e = def.effect.unwrap();
                    detail::hash_int(h, static_cast<int>(e.kind));
                    detail::hash_int(h, e.amount);
                    detail::hash_int(h, e.count);
                    detail::hash_int(
                        h, e.scope.is_some() ? static_cast<int>(e.scope.unwrap()) : -1);
                    detail::hash_int(
                        h, e.response.is_some()
                               ? static_cast<int>(e.response.unwrap())
                               : -1);
                    detail::hash_int(h, e.range);
                }
                if (def.equip.is_some())
                {
                    const auto &e = def.equip.unwrap();
                    detail::hash_int(h, static_cast<int>(e.slot));
                    detail::hash_int(h, e.range);
                }
                if (def.judge.is_some())
                {
                    const auto &j = def.judge.unwrap();
                    detail::hash_int(h, static_cast<int>(j.trigger));
                    detail::hash_int(h, static_cast<int>(j.success));
                    detail::hash_int(h, static_cast<int>(j.failure));
                    detail::hash_int(h, j.amount);
                    detail::hash_int(
                        h, j.scope.is_some() ? static_cast<int>(j.scope.unwrap()) : -1);
                }
                for (auto a : def.abilities)
                    detail::hash_int(h, static_cast<int>(a));
            }
            return h;
        }

        /** @brief JSON 字符串转义。 */
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

        inline std::string jstr(std::string_view s)
        {
            return "\"" + escape_json(s) + "\"";
        }

        inline std::string card_json(const card::Card &c)
        {
            std::ostringstream os;
            os << "{\"iid\":" << jstr(c.instance_id) << ",\"def\":" << jstr(c.def_id)
               << ",\"suit\":" << jstr(suit_name(c.suit))
               << ",\"number\":" << c.number << "}";
            return os.str();
        }

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

        /** @brief 序列化整局（含会话进度）到 JSON 文本。 */
        inline std::string write(
            const game::Game &g, const game::GameSession &session,
            const std::string &deck_name)
        {
            const auto &r = g.rules;
            const auto snap = g.cards.snapshot();

            std::ostringstream os;
            os << "{\"format\":" << jstr(kFormat) << ",\"version\":" << kVersion;
            os << ",\"deck\":{\"name\":" << jstr(deck_name)
               << ",\"hash\":" << deck_hash(g.catalog) << "}";
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
               << ",\"started\":" << (session.started ? "true" : "false") << "}";
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
            const auto ents = g.entities.snapshot();
            for (std::size_t i = 0; i < ents.size(); ++i)
            {
                if (i)
                    os << ",";
                os << "{\"id\":" << jstr(ents[i].id) << ",\"seat\":" << ents[i].seat
                    << ",\"hp\":" << ents[i].hp << ",\"max_hp\":" << ents[i].max_hp
                    << ",\"gender\":" << jstr(gender_name(ents[i].gender))
                    << "}";
            }
            os << "]}";
            return os.str();
        }
    }
}

#endif  // INCLUDE_TKW_SAVE_WRITER_HPP
