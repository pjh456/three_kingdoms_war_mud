/**
 * @file   session.hpp
 * @brief  CLI 会话与参数类型。
 * @details AI 难度档、启动选项、跨命令持有的对局会话；纯声明，不含命令解析与
 *          渲染逻辑。
 * @note   对局统计复用存档层的可持久化结构，使会话与存档共用同一组字段。
 * @ingroup tkw_cli
 */
#ifndef INCLUDE_TKW_CLI_SESSION_HPP
#define INCLUDE_TKW_CLI_SESSION_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "game/core/roles.hpp"
#include "game/core/rules.hpp"
#include "game/flow/loop.hpp"
#include "game/flow/table.hpp"
#include "save/session_meta.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace cli
    {
        /** @brief 对局统计聚合；复用存档层的可持久化结构，使会话与存档共用同一字段。 */
        using BattleStats = tkw::save::BattleStats;

        /**
         * @brief AI 难度档。
         * @note  CLI 值域；引擎侧实现为 `SimpleDecider` / `AggressiveDecider`。
         */
        enum class AiLevel : std::uint8_t
        {
            Simple,     /**< 贪心档（默认）。 */
            Aggressive, /**< 攻击优先档（伤害/多目标先行）。 */
        };

        /** @brief AI 难度档与其稳定文本的一条映射。 */
        struct AiLevelText
        {
            const char *name; /**< 稳定值域文本；CLI/TUI/存档共用的文本契约。 */
            AiLevel level;    /**< 对应难度档。 */
        };

        /**
         * @brief AI 难度档 ↔ 文本的唯一事实源。
         * @note  表序即值域顺序：枚举候选、completer、存档文本与各前端显示均由此
         *        派生；新增/调整档位只改此表。
         */
        inline constexpr AiLevelText kAiLevelTexts[] = {
            {"simple", AiLevel::Simple},
            {"aggressive", AiLevel::Aggressive},
        };

        /**
         * @brief  AI 难度档 → 稳定文本。
         * @param[in] ai 难度档。
         * @return 稳定字面量（`"simple"`/`"aggressive"`）；表外档位回落首项。
         * @note  返回静态字面量，可直接参与 `std::string` 拼接。
         */
        const char *ai_level_name(AiLevel ai);

        /**
         * @brief  稳定文本 → AI 难度档。
         * @param[in] name 值域文本。
         * @return 匹配的难度档。
         * @retval Some `name` 命中 `kAiLevelTexts`。
         * @retval None 空串或未知文本；默认档由上层决定。
         */
        tkw::Option<AiLevel> ai_level_from(std::string_view name);

        /**
         * @brief 命令行/REPL 解析出的对局参数。
         * @note  REPL 每行命令独立解析，行内显式选项优先于启动选项。
         */
        struct Options
        {
            std::filesystem::path deck = "resources"; /**< 牌表目录（含 `deck.json` 与 `cards/`）。 */
            int players = 4;                          /**< 玩家数。 */
            int hand = tkw::game::RulesConfig{}.initial_hand; /**< 初始手牌数。 */
            std::uint32_t seed = 42;                  /**< 随机种子。 */
            bool verbose = false;                     /**< 是否打印事件日志。 */
            bool verbose_explicit = false; /**< verbose 是否来自显式选项（含启动 --no-verbose）。 */
            std::filesystem::path autosave = "tkw-autosave.json"; /**< REPL 退出时自动存档路径；空串关闭。 */
            std::filesystem::path history; /**< REPL 命令历史文件；空 = 仅内存（opt-in）。 */
            std::vector<std::string> humans; /**< 真人座位 id（可重复选项累积）。 */
            std::vector<std::string> heroes; /**< 武将选择原文（`--hero 座位=武将`，可重复）。 */
            AiLevel ai = AiLevel::Simple;    /**< AI 难度档（默认 simple，零行为变化）。 */
            tkw::game::GameMode mode =
                tkw::game::GameMode::Brawl; /**< 对局模式（默认乱斗，零行为变化）。 */
        };

        /**
         * @brief 跨命令持有的对局会话。
         * @note  `new`/`step`/`run`/`save`/`load` 共享同一会话。
         */
        struct Session
        {
            std::unique_ptr<tkw::game::Game> game; /**< 当前对局运行时；未开局为空。 */
            tkw::game::GameSession state;          /**< 当前会话进度。 */
            std::vector<std::string> humans; /**< 本会话的真人座位 id。 */
            AiLevel ai = AiLevel::Simple;    /**< 本会话 AI 难度（new/load 写入，step/run 消费）。 */
            bool verbose = false;            /**< 本会话是否打印事件日志。 */
            Options base;                    /**< REPL 启动选项（供行内命令继承）。 */
            bool active = false;             /**< 是否已建立可续玩的会话。 */
            BattleStats stats; /**< 本会话累计的对局统计（new 时重置、load 时从存档恢复；对局结束时打印）。 */
            std::filesystem::path deck = "resources"; /**< 本会话牌表目录（new/load 写入；status 展示来源）。 */
        };
    }
}

#endif  // INCLUDE_TKW_CLI_SESSION_HPP
