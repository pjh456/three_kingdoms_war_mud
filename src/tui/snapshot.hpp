/**
 * @file   snapshot.hpp
 * @brief  TUI 视图快照：由 CLI 会话抽取四面板纯数据值。
 * @details 全值拷贝、不含目录/对局裸指针：脱离 `Game` 生命周期可安全渲染，供
 *          后台线程经值传递回送主线程。字符串标签复用 `cli/render.hpp` 的纯函数。
 * @ingroup tkw_tui
 */
#ifndef INCLUDE_TKW_TUI_SNAPSHOT_HPP
#define INCLUDE_TKW_TUI_SNAPSHOT_HPP

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "cli/render.hpp"
#include "cli/session.hpp"
#include "entity/base.hpp"
#include "entity/manager.hpp"
#include "game/core/context.hpp"
#include "game/core/roles.hpp"
#include "game/flow/loop.hpp"
#include "game/flow/table.hpp"
#include "game/query/distance.hpp"
#include "hero/catalog.hpp"
#include "tui/visibility.hpp"

namespace tkw
{
    namespace tui
    {
        /**
         * @brief  一名玩家的棋盘行（手牌遵守可见性红线）。
         * @note   `hand` 的展开由 `viewer` 与座位关系决定；其余字段均为公开信息。
         */
        struct PlayerRow
        {
            std::string id;  /**< 玩家 id。 */
            int seat = 0;    /**< 座位号。 */
            int hp = 0;      /**< 当前体力。 */
            int max_hp = 0;  /**< 体力上限。 */
            ZoneView hand;   /**< 仅 viewer == self 展开；他人只 `count`。 */
            ZoneView equip;  /**< 装备区，明置。 */
            ZoneView judge;  /**< 判定区，明置。 */
            int distance = 0; /**< viewer → 该玩家（调整后距离）。 */
            bool in_attack_range = false; /**< viewer 能否用杀够到。 */
            tkw::game::Role role = tkw::game::Role::None; /**< 身份局；乱斗 `None`。 */
            bool chained = false; /**< 横置（连环）状态；公开信息。 */
            std::string hero;     /**< 武将展示名；空 = 无（公开信息）。 */

            bool operator==(const PlayerRow &) const = default; /**< 逐字段相等；@return 全等。 */
        };

        /**
         * @brief  四面板快照（值拷贝，脱离 Game 生命周期可安全渲染）。
         * @note   全字段均为值或纯文本，不持目录/`Game` 裸指针。
         */
        struct UiSnapshot
        {
            bool active = false;      /**< 是否有进行中的对局。 */
            bool over = false;        /**< `session_over` 结果。 */
            bool at_cap = false;      /**< `!over && turns > max_turns`，仅提示。 */
            tkw::game::GameMode mode = tkw::game::GameMode::Brawl; /**< 对局模式。 */
            std::string current;      /**< 下一回合 id。 */
            int turns = 0;            /**< 已完成回合数。 */
            std::size_t alive = 0;    /**< 存活实体数。 */
            std::string winner;       /**< 原始胜者/阵营代表 id（空串 = 无/平局）。 */
            std::string winner_label; /**< 终局展示标签（乱斗/身份局口径分流）。 */
            tkw::cli::AiLevel ai = tkw::cli::AiLevel::Simple; /**< AI 难度档。 */
            std::filesystem::path deck;       /**< 活动牌表路径。 */
            std::vector<std::string> humans;  /**< 真人座位 id。 */
            std::string viewer;               /**< 本视图视角座位。 */
            std::vector<PlayerRow> players;   /**< 座位序（`ordered_ids`）。 */
            std::size_t draw_size = 0;        /**< 摸牌堆张数。 */
            std::size_t discard_size = 0;     /**< 弃牌堆张数。 */

            bool operator==(const UiSnapshot &) const = default; /**< 逐字段相等；@return 全等。 */
        };

        /**
         * @brief  由会话构造四面板快照；`viewer` 决定手牌可见性。
         * @param[in] s      当前 CLI 会话。
         * @param[in] viewer 视角玩家 id。
         * @return `active` 为假或 `game` 为空时返回 `active=false` 的空快照，不触
         *         引擎容器；否则经 `Game::context()` 只读取数，`players` 按座位序。
         * @post  本函数不改变会话与对局状态。
         * @note  值语义：所有字段均为值拷贝，不持 `catalog`/`Game` 指针。
         */
        inline UiSnapshot make_snapshot(
            const tkw::cli::Session &s, const std::string &viewer)
        {
            UiSnapshot snap;
            snap.viewer = viewer;
            if (!s.active || !s.game)
                return snap;

            auto ctx = s.game->context();
            const tkw::game::ReadOnlyContext ro = ctx;

            snap.active = true;
            snap.over = tkw::game::SessionQuery::session_over(ctx);
            snap.at_cap =
                !snap.over && s.state.turns > tkw::game::rules_of(ctx).max_turns;
            snap.mode = tkw::game::mode_of(ctx);
            snap.current = s.state.current;
            snap.turns = s.state.turns;
            snap.alive = ctx.entities->size();
            snap.winner = tkw::game::SessionQuery::session_winner(ctx);
            snap.winner_label =
                snap.mode == tkw::game::GameMode::Brawl
                    ? tkw::cli::detail::winner_label(snap.winner)
                    : tkw::cli::detail::identity_result_label(
                          tkw::game::SessionQuery::session_camp(ctx), snap.winner);
            snap.ai = s.ai;
            snap.deck = s.deck;
            snap.humans = s.humans;

            snap.draw_size = ctx.cards->draw_size();
            snap.discard_size = ctx.cards->discard_size();

            for (const auto &id : ro.entities->ordered_ids())
            {
                const auto found = ro.entities->find(id);
                if (found.is_none())
                    continue;
                const auto *entity = found.unwrap();

                PlayerRow row;
                row.id = id;
                row.seat = entity->get_seat();
                row.hp = entity->get_hp();
                row.max_hp = entity->get_hp_bar().get_max();
                row.hand = visible_hand(ro, viewer, id);
                row.equip = public_zone(ro, ro.cards->equip(id));
                row.judge = public_zone(ro, ro.cards->judge(id));
                row.distance =
                    tkw::game::DistanceQuery::distance_between(ro, viewer, id);
                row.in_attack_range =
                    tkw::game::DistanceQuery::in_attack_range(ro, viewer, id);
                const tkw::game::Role role = tkw::game::role_of(ro.roles, id);
                row.role = role_visible(s.humans, snap.over, id, role)
                               ? role
                               : tkw::game::Role::None;
                row.chained = entity->get_chained();
                row.hero = tkw::hero::display_hero_name(
                    ro.heroes, entity->get_hero());
                snap.players.push_back(std::move(row));
            }

            return snap;
        }
    }  // namespace tui
}  // namespace tkw

#endif  // INCLUDE_TKW_TUI_SNAPSHOT_HPP
