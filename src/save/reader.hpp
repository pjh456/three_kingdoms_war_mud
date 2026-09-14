/**
 * @file   reader.hpp
 * @brief  存档 JSON 文本 → 对局状态。
 * @details 校验 format/version/牌表指纹与结构；全部通过后才写入目标对象，
 *          任一失败立即早退，目标状态不被污染。
 * @ingroup tkw_save
 */

#ifndef INCLUDE_TKW_SAVE_READER_HPP
#define INCLUDE_TKW_SAVE_READER_HPP

#include <cstdint>
#include <limits>
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
#include "game/core/roles.hpp"
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

            SaveResult<void> fail(SaveErrorKind kind, std::string detail);

            /**
             * @brief  int64 → int：收窄前检查值域，不静默截断。
             * @param[in]  v   待收窄的整数。
             * @param[out] out 收窄结果；仅在返回 `true` 时写入。
             * @return 收窄是否成功。
             * @retval true  `v` 在 int 值域内，`out` 已写入。
             * @retval false `v` 越界，`out` 不被修改。
             */
            bool narrow_to_int(std::int64_t v, int &out) noexcept;

            bool read_int(const json::Object &o, std::string_view key, int &out);

            bool read_bool(const json::Object &o, std::string_view key, bool &out);

            bool read_str(
                const json::Object &o, std::string_view key, std::string &out);

            bool read_card(const json::Json &j, card::Card &out);

            bool read_cards(const json::Json &j, std::vector<card::Card> &out);

            bool read_zones(
                const json::Json &j,
                std::vector<std::pair<std::string, std::vector<card::Card>>> &out);

            bool read_int_map(
                const json::Json &j, std::map<std::string, int> &out);

            bool read_str_map(
                const json::Json &j, std::map<std::string, std::string> &out);

            bool read_str_set(const json::Json &j, std::set<std::string> &out);
        }

        /**
         * @brief  从存档文本恢复：填充 `g` 的 cards/entities/rules/rng 与 session。
         * @details 解析并逐项校验后一次性落子；出参 `meta` 与目标状态同批赋值。
         * @param[in]     text    存档 JSON 文本。
         * @param[in,out] g       已加载好 catalog 的对局运行时；成功时写入 cards/
         *                        entities/rules/rng/mode/roles，catalog 不参与序列化。
         * @param[out]    session 接收会话进度。
         * @param[out]    meta    非空时接收可选会话元数据；失败时不写入。
         * @return 成功为 `Ok`，失败为 `Err(SaveError)`。
         * @retval Ok                   全部校验通过并已落子。
         * @retval Err(ParseError)      JSON 文本无法解析。
         * @retval Err(VersionMismatch) `format` 或 `version` 不符。
         * @retval Err(DeckMismatch)    牌表指纹与当前目录及历史降级值均不符。
         * @retval Err(StructureError)  字段缺失、类型不符或结构非法。
         * @retval Err(RngError)        随机源状态无法恢复。
         * @pre   `g` 已加载好 catalog；`g.rng` 可空，为空时跳过随机源恢复。
         * @post  全部校验通过后才写入 `g`/`session`/`meta`；任一失败立即返回，
         *        目标对象不被修改。
         * @note  版本策略：接受基础 1、含连环状态的 2 与含武将选择的 3；旧档
         *        缺失 ai/stats 字段回落默认，缺 mode/roles 回落乱斗 + 空角色表，
         *        缺 chained 回落 false、缺 hero 回落空。身份局档要求角色表覆盖
         *        全部存活实体且恰含一名主公，允许保留已阵亡玩家的角色条目；
         *        非空 hero 必须命中本局目录，否则以 `StructureError` 拒绝。
         * @note  牌表指纹不符时以 `DeckMismatch` 拒绝；同时接受「判定属性尚未
         *        数据化」时期写出的历史标准档值。
         * @see   save::write, deck_hash
         */
        SaveResult<void> read(
            std::string_view text, game::Game &g, game::GameSession &session,
            SessionMeta *meta = nullptr);
    }
}

#endif  // INCLUDE_TKW_SAVE_READER_HPP
