/**
 * @file reader.hpp
 * @brief 存档 JSON 文本 → 对局状态。
 * @note 校验 format/version/牌表指纹；失败不修改目标。
 */

#ifndef INCLUDE_TKW_SAVE_READER_HPP
#define INCLUDE_TKW_SAVE_READER_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pjh_json/document.hpp>
#include <pjh_json/json.hpp>

#include "card/card.hpp"
#include "card/manager.hpp"
#include "entity/manager.hpp"
#include "game/core/rules.hpp"
#include "game/flow/loop.hpp"
#include "game/flow/table.hpp"
#include "save/deck_hash.hpp"
#include "save/error.hpp"
#include "save/format.hpp"
#include "save/session_meta.hpp"
#include "util/rng.hpp"

namespace tkw
{
    namespace save
    {
        namespace detail
        {
            namespace json = pjh::json;

            inline SaveResult<void> fail(SaveErrorKind kind, std::string detail)
            {
                return SaveResult<void>::Err(SaveError{kind, std::move(detail)});
            }

            inline bool read_int(const json::Object &o, std::string_view key, int &out)
            {
                if (!o.contains(key))
                    return false;
                auto v = o[key].try_as_int();
                if (!v)
                    return false;
                out = static_cast<int>(*v);
                return true;
            }

            inline bool read_bool(
                const json::Object &o, std::string_view key, bool &out)
            {
                if (!o.contains(key))
                    return false;
                auto v = o[key].try_as_boolean();
                if (!v)
                    return false;
                out = *v;
                return true;
            }

            inline bool read_str(
                const json::Object &o, std::string_view key, std::string &out)
            {
                if (!o.contains(key))
                    return false;
                auto v = o[key].try_as_string();
                if (!v)
                    return false;
                out = std::string(*v);
                return true;
            }

            inline bool read_card(const json::Json &j, card::Card &out)
            {
                const auto *o = j.try_as_object();
                if (!o)
                    return false;
                std::string iid;
                std::string def;
                std::string suit;
                int number = 0;
                if (!read_str(*o, "iid", iid) || !read_str(*o, "def", def) ||
                    !read_str(*o, "suit", suit) || !read_int(*o, "number", number))
                    return false;
                card::Suit s{};
                if (!suit_from(suit, s) || number < 1 || number > 13)
                    return false;
                out = card::Card{std::move(iid), std::move(def), s, number};
                return true;
            }

            inline bool read_cards(
                const json::Json &j, std::vector<card::Card> &out)
            {
                const auto *arr = j.try_as_array();
                if (!arr)
                    return false;
                for (std::size_t i = 0; i < arr->size(); ++i)
                {
                    card::Card c;
                    if (!read_card((*arr)[i], c))
                        return false;
                    out.push_back(std::move(c));
                }
                return true;
            }

            inline bool read_zones(
                const json::Json &j,
                std::vector<std::pair<std::string, std::vector<card::Card>>> &out)
            {
                const auto *o = j.try_as_object();
                if (!o)
                    return false;
                for (std::string_view k : o->keys())
                {
                    std::vector<card::Card> cards;
                    if (!read_cards((*o)[k], cards))
                        return false;
                    out.emplace_back(std::string(k), std::move(cards));
                }
                return true;
            }

            inline bool read_int_map(
                const json::Json &j, std::map<std::string, int> &out)
            {
                const auto *o = j.try_as_object();
                if (!o)
                    return false;
                for (std::string_view k : o->keys())
                {
                    auto v = (*o)[k].try_as_int();
                    if (!v)
                        return false;
                    out[std::string(k)] = static_cast<int>(*v);
                }
                return true;
            }

            inline bool read_str_map(
                const json::Json &j, std::map<std::string, std::string> &out)
            {
                const auto *o = j.try_as_object();
                if (!o)
                    return false;
                for (std::string_view k : o->keys())
                {
                    auto v = (*o)[k].try_as_string();
                    if (!v)
                        return false;
                    out[std::string(k)] = std::string(*v);
                }
                return true;
            }

            inline bool read_str_set(const json::Json &j, std::set<std::string> &out)
            {
                const auto *arr = j.try_as_array();
                if (!arr)
                    return false;
                for (std::size_t i = 0; i < arr->size(); ++i)
                {
                    auto v = (*arr)[i].try_as_string();
                    if (!v)
                        return false;
                    out.insert(std::string(*v));
                }
                return true;
            }
        }

        /**
         * @brief 从存档文本恢复：填充 g 的 cards/entities/rules/rng 与 session。
         * @param g        已加载好 catalog 的对局运行时（catalog 不参与序列化）。
         * @param session  接收会话进度。
         * @param meta     非空时接收可选会话元数据；解析失败不写入。
         * @return Ok 或 Err(SaveError)；牌表指纹不符时拒绝。
         * @note 旧档缺失 ai/stats 字段时回落默认，不拒绝；全部校验通过后才落子，
         *       出参 meta 与目标状态同批赋值，早退不污染。
         */
        inline SaveResult<void> read(
            std::string_view text, game::Game &g, game::GameSession &session,
            SessionMeta *meta = nullptr)
        {
            namespace json = pjh::json;

            auto doc_r = json::parse_copy_result(text);
            if (doc_r.is_err())
                return detail::fail(SaveErrorKind::ParseError, "JSON 解析失败");
            json::Document doc = std::move(doc_r).unwrap();
            const json::Json &root = doc.root();
            const auto *obj = root.try_as_object();
            if (!obj)
                return detail::fail(SaveErrorKind::StructureError, "root");

            // format / version
            auto fmt = obj->contains("format") ? (*obj)["format"].try_as_string()
                                               : std::optional<std::string_view>{};
            if (!fmt || *fmt != kFormat)
                return detail::fail(SaveErrorKind::VersionMismatch, "format");
            int version = 0;
            if (!detail::read_int(*obj, "version", version) || version != kVersion)
                return detail::fail(SaveErrorKind::VersionMismatch, "version");

            // deck hash：写出侧按 int64 位型承载，高位指纹在此逐位还原为 uint64
            if (!obj->contains("deck") || !(*obj)["deck"].try_as_object())
                return detail::fail(SaveErrorKind::StructureError, "deck");
            const auto &deck = (*obj)["deck"].as_object();
            auto hash = deck.contains("hash") ? deck["hash"].try_as_int()
                                              : std::optional<std::int64_t>{};
            if (!hash)
                return detail::fail(SaveErrorKind::StructureError, "deck.hash");
            if (static_cast<std::uint64_t>(*hash) != deck_hash(g.catalog))
                return detail::fail(SaveErrorKind::DeckMismatch, "deck.hash");

            // rules
            if (!obj->contains("rules") || !(*obj)["rules"].try_as_object())
                return detail::fail(SaveErrorKind::StructureError, "rules");
            const auto &rules = (*obj)["rules"].as_object();
            game::RulesConfig rc;
            if (!detail::read_int(rules, "draw_per_turn", rc.draw_per_turn) ||
                !detail::read_int(rules, "sha_limit", rc.sha_limit) ||
                !detail::read_int(rules, "kill_reward", rc.kill_reward) ||
                !detail::read_int(rules, "initial_hand", rc.initial_hand) ||
                !detail::read_int(rules, "max_turns", rc.max_turns) ||
                !detail::read_int(rules, "dying_rounds", rc.dying_rounds) ||
                !detail::read_int(rules, "wuxie_rounds", rc.wuxie_rounds) ||
                !detail::read_int(rules, "duel_rounds", rc.duel_rounds) ||
                !detail::read_int(rules, "base_hp", rc.base_hp) ||
                !detail::read_int(rules, "min_players", rc.min_players) ||
                !detail::read_int(rules, "max_players", rc.max_players))
                return detail::fail(SaveErrorKind::StructureError, "rules");

            // rng
            if (!obj->contains("rng") || !(*obj)["rng"].try_as_object())
                return detail::fail(SaveErrorKind::StructureError, "rng");
            std::string rng_data;
            if (!detail::read_str((*obj)["rng"].as_object(), "data", rng_data))
                return detail::fail(SaveErrorKind::StructureError, "rng.data");

            // session（进度 + 可选元数据：ai / stats）
            if (!obj->contains("session") || !(*obj)["session"].try_as_object())
                return detail::fail(SaveErrorKind::StructureError, "session");
            const auto &sess = (*obj)["session"].as_object();
            game::GameSession s;
            if (!detail::read_str(sess, "current", s.current) ||
                !detail::read_int(sess, "turns", s.turns) ||
                !detail::read_bool(sess, "started", s.started))
                return detail::fail(SaveErrorKind::StructureError, "session");
            SessionMeta parsed;
            if (sess.contains("ai"))
            {
                auto ai = sess["ai"].try_as_string();
                if (!ai)
                    return detail::fail(SaveErrorKind::StructureError, "session.ai");
                parsed.ai = std::string(*ai);
            }
            if (sess.contains("stats"))
            {
                const auto *st = sess["stats"].try_as_object();
                if (!st)
                    return detail::fail(
                        SaveErrorKind::StructureError, "session.stats");
                if ((st->contains("damage_dealt") &&
                     !detail::read_int_map(
                         (*st)["damage_dealt"], parsed.stats.damage_dealt)) ||
                    (st->contains("healing") &&
                     !detail::read_int_map((*st)["healing"], parsed.stats.healing)) ||
                    (st->contains("kills") &&
                     !detail::read_int_map((*st)["kills"], parsed.stats.kills)) ||
                    (st->contains("last_hit_source") &&
                     !detail::read_str_map((*st)["last_hit_source"],
                                           parsed.stats.last_hit_source)) ||
                    (st->contains("died") &&
                     !detail::read_str_set((*st)["died"], parsed.stats.died)))
                    return detail::fail(
                        SaveErrorKind::StructureError, "session.stats");
            }

            // cards
            if (!obj->contains("cards") || !(*obj)["cards"].try_as_object())
                return detail::fail(SaveErrorKind::StructureError, "cards");
            const auto &cards = (*obj)["cards"].as_object();
            card::CardManagerSnapshot snap;
            {
                auto seq = cards.contains("instance_seq")
                               ? cards["instance_seq"].try_as_int()
                               : std::optional<std::int64_t>{};
                if (!seq)
                    return detail::fail(
                        SaveErrorKind::StructureError, "cards.instance_seq");
                snap.instance_seq = static_cast<std::uint64_t>(*seq);
            }
            if (!cards.contains("draw") || !cards.contains("discard") ||
                !cards.contains("hand") || !cards.contains("equip") ||
                !cards.contains("judge"))
                return detail::fail(SaveErrorKind::StructureError, "cards");
            if (!detail::read_cards(cards["draw"], snap.draw) ||
                !detail::read_cards(cards["discard"], snap.discard) ||
                !detail::read_zones(cards["hand"], snap.hand) ||
                !detail::read_zones(cards["equip"], snap.equip) ||
                !detail::read_zones(cards["judge"], snap.judge))
                return detail::fail(SaveErrorKind::StructureError, "cards");

            // entities
            if (!obj->contains("entities") || !(*obj)["entities"].try_as_array())
                return detail::fail(SaveErrorKind::StructureError, "entities");
            const auto &arr = (*obj)["entities"].as_array();
            std::vector<EntitySnapshot> ents;
            for (std::size_t i = 0; i < arr.size(); ++i)
            {
                const auto *eo = arr[i].try_as_object();
                if (!eo)
                    return detail::fail(SaveErrorKind::StructureError, "entities");
                EntitySnapshot e;
                if (!detail::read_str(*eo, "id", e.id) ||
                    !detail::read_int(*eo, "seat", e.seat) ||
                    !detail::read_int(*eo, "hp", e.hp) ||
                    !detail::read_int(*eo, "max_hp", e.max_hp))
                    return detail::fail(SaveErrorKind::StructureError, "entities");
                // 旧档无 gender 字段：回落 Male，不拒绝旧档
                if (eo->contains("gender"))
                {
                    auto gv = (*eo)["gender"].try_as_string();
                    if (!gv || !gender_from(*gv, e.gender))
                        return detail::fail(
                            SaveErrorKind::StructureError, "entities.gender");
                }
                ents.push_back(std::move(e));
            }

            // ── 全部校验通过后再落子；rng 恢复是唯一可能失败的阶段，先于其余赋值 ──
            if (g.rng && !g.rng->load_state(RngState{rng_data}))
                return detail::fail(SaveErrorKind::RngError, "rng.data");
            g.rules = rc;
            session = std::move(s);
            g.cards.restore(snap);
            g.entities.restore(ents);
            if (meta)
                *meta = std::move(parsed);
            return SaveResult<void>::Ok();
        }
    }
}

#endif  // INCLUDE_TKW_SAVE_READER_HPP
