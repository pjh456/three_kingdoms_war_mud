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
        UiSnapshot make_snapshot(const tkw::cli::Session &s,
                                 const std::string &viewer);
    }  // namespace tui
}  // namespace tkw

#endif  // INCLUDE_TKW_TUI_SNAPSHOT_HPP
