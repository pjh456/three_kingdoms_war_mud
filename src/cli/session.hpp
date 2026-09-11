/**
 * @file session.hpp
 * @brief CLI 会话与参数类型：AI 难度档、启动选项、跨命令持有的对局会话。
 * @note 纯声明，不含命令解析与渲染逻辑；对局统计复用存档层的可持久化结构，
 *       使会话与存档共用同一组字段。
 */
#ifndef INCLUDE_TKW_CLI_SESSION_HPP
#define INCLUDE_TKW_CLI_SESSION_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "game/core/rules.hpp"
#include "game/flow/loop.hpp"
#include "game/flow/table.hpp"
#include "save/session_meta.hpp"

namespace tkw
{
    namespace cli
    {
        /** 对局统计聚合：复用存档层的可持久化结构，会话与存档共用同一字段。 */
        using BattleStats = tkw::save::BattleStats;

        /** AI 难度档（CLI 值域；引擎侧实现为 SimpleDecider / AggressiveDecider）。 */
        enum class AiLevel : std::uint8_t
        {
            Simple,     /**< 贪心档（默认） */
            Aggressive, /**< 攻击优先档（伤害/多目标先行） */
        };

        /** 命令行/REPL 解析出的对局参数。 */
        struct Options
        {
            std::filesystem::path deck = "resources";
            int players = 4;
            int hand = tkw::game::RulesConfig{}.initial_hand;
            std::uint32_t seed = 42;
            bool verbose = false;
            std::filesystem::path autosave = "tkw-autosave.json";
            std::filesystem::path history; /**< REPL 命令历史文件；空 = 仅内存（opt-in） */
            std::vector<std::string> humans; /**< 真人座位 id（可重复选项累积） */
            AiLevel ai = AiLevel::Simple;    /**< AI 难度档（默认 simple，零行为变化） */
        };

        /** 跨命令持有的对局会话（new/step/run/save/load 共享）。 */
        struct Session
        {
            std::unique_ptr<tkw::game::Game> game;
            tkw::game::GameSession state;
            std::vector<std::string> humans; /**< 本会话的真人座位 id */
            AiLevel ai = AiLevel::Simple;    /**< 本会话 AI 难度（new/load 写入，step/run 消费） */
            bool verbose = false;            /**< 本会话是否打印事件日志 */
            Options base;                    /**< REPL 启动选项（供行内命令继承） */
            bool active = false;
            BattleStats stats; /**< 本会话累计的对局统计（new 时重置、load 时从存档恢复；对局结束时打印） */
        };
    }
}

#endif  // INCLUDE_TKW_CLI_SESSION_HPP
