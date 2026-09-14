/**
 * @file   controller.hpp
 * @brief  TUI 会话控制器：命令驱动、后台引擎线程、日志订阅重绑与退出自动存档。
 * @details 无 FTXUI、无终端：可脱离 TTY 做单元测试。跨线程只传不可变值——worker
 *          构造值快照与日志拷贝，经注入的 `Post` 回送主线程；`Post` 闭包只捕获
 *          `shared_ptr<UiModel>`，不捕获 `this`，避免退出后触碰已析构对象。
 *          同一时刻 `m_session`/`m_log` 只被一个线程访问：Idle 归主线程，Running
 *          归 worker；`m_cancel` 为原子取消位，worker 在回合边界检查后退出。
 * @warning 成员声明序即析构保证：`m_worker` 最后声明、最先析构（自动 join），
 *          随后 stats/log 句柄退订，最后 `m_session` 析构（含 Game/总线）。
 * @ingroup tkw_tui
 */
#ifndef INCLUDE_TKW_TUI_CONTROLLER_HPP
#define INCLUDE_TKW_TUI_CONTROLLER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "cli/commands.hpp"
#include "cli/error_zh.hpp"
#include "cli/query_lines.hpp"
#include "cli/render.hpp"
#include "cli/session.hpp"
#include "event/event_bus.hpp"
#include "game/ai/aggressive.hpp"
#include "game/ai/simple.hpp"
#include "game/core/decision.hpp"
#include "game/flow/factory.hpp"
#include "game/flow/loop.hpp"
#include "game/flow/table.hpp"
#include "io/file.hpp"
#include "save/reader.hpp"
#include "save/session_meta.hpp"
#include "save/writer.hpp"
#include "tui/command.hpp"
#include "tui/decision_source.hpp"
#include "tui/log_lines.hpp"
#include "tui/snapshot.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace tui
    {
        /**
         * @brief  主线程渲染读取的共享模型：值快照 + 日志值拷贝 + 运行标志。
         * @note   只承载不可变值与原子运行位；跨线程经 `shared_ptr` 共享。
         */
        struct UiModel
        {
            UiSnapshot snapshot;                /**< 四面板值快照。 */
            std::vector<std::string> log_lines; /**< 日志值拷贝（主线程渲染读）。 */
            std::atomic<bool> running{false};   /**< worker 是否在推进引擎。 */
        };

        /**
         * @class Controller
         * @brief 驱动一局会话的命令控制器：解析命令、调度 worker、回送快照、退出存档。
         * @note  线程契约：new/load/save/status/cards/rules/audit/decks/heroes 只在
         *        Idle 的主线程执行；step/run/simulate 在 worker 执行。worker 期间
         *        主线程不读 `m_session`/`m_log`，只读 `UiModel`。
         * @warning 不可拷贝：持后台 worker 与订阅句柄；析构不显式 join，成员声明序
         *          保证 `m_worker` 最先析构并自动 join。
         */
        class Controller
        {
        public:
            /** @brief Post 接缝：把回调投递到 UI 主循环（FTXUI 为 `screen.Post`）。 */
            using Post = std::function<void(std::function<void()>)>;

            /**
             * @brief 构造控制器。
             * @param[in] post 回送接缝；空时使用同步就地执行的默认实现（测试/无 UI）。
             */
            explicit Controller(Post post = {}) { set_post(std::move(post)); }

            Controller(const Controller &) = delete;            /**< 不可拷贝。 */
            Controller &operator=(const Controller &) = delete; /**< 不可拷贝赋值。 */

            /**
             * @brief  替换回送接缝；空 `Post` 回落同步就地执行。
             * @param[in] post 新的 `Post`；不得在 worker 运行中切换。
             */
            void set_post(Post post)
            {
                if (post)
                    m_post = std::move(post);
                else
                    m_post = [](std::function<void()> fn) { fn(); };
            }

            /** @brief 设置退出回调（UI 侧退出事件循环）；在 request_quit 末尾调用。
             * @param[in] on_quit 退出回调。 */
            void set_on_quit(std::function<void()> on_quit)
            {
                on_quit_ = std::move(on_quit);
            }

            /** @brief 设置启动选项基准（deck/players/seed/ai/autosave 等）。
             * @param[in] options 启动选项。 */
            void set_base_options(tkw::cli::Options options)
            {
                m_base = std::move(options);
            }

            /**
             * @brief 建默认对局并绑定日志/统计，随后刷新模型。
             * @note  建局失败只写日志提示，不抛异常；初始摸牌事件先于开局发牌订阅。
             */
            void bootstrap();

            /**
             * @brief  解析并执行一行命令；反馈一律写入日志。
             * @param[in] line 用户输入行；空白行与解析失败只提示不改状态。
             * @note   主线程调用；new/load/save 在 worker 运行中被拒绝，step/run
             *         在运行中同样拒绝（worker 内另有一道 running 门）。
             */
            void execute_line(std::string_view line);

            /**
             * @brief 取消 worker、自动存档、退订句柄，最后触发退出回调。
             * @note 关停顺序不可颠倒：join 后引擎静默，才能读会话写存档与退订；
             *       幂等，重复调用直接返回。
             */
            void request_quit();

            /** @brief 阻塞等待当前 worker 结束（测试与关停用）。 */
            void wait_idle() { join_worker(); }

            /**
             * @brief  取走当前真人待决面板（每个待决仅返回真一次）。
             * @param[out] out 有待决且未被取走时写入面板值视图。
             * @return 取到返回 true；无待决/无真人决策源/已取走返回 false。
             * @note   主线程调用；返回后面板值可安全渲染，不依赖引擎生命周期。
             */
            bool fetch_new_decision(DecisionPanelView &out)
            {
                return m_decision && m_decision->fetch_new(out);
            }

            /** @brief 是否存在未被取走的真人待决。
             * @return 有待决且已绑定真人决策源时为 true。 */
            bool has_pending_decision() const
            {
                return m_decision && m_decision->has_pending();
            }

            /**
             * @brief  提交真人待决的选择；非法选择被忽略且保持待决。
             * @param[in] selected 选中候选的 0 基下标；Discard 为多选。
             * @param[in] pass     是否放弃。
             * @return 被接受返回 true；无待决/选择非法返回 false。
             */
            bool submit_decision(std::vector<std::size_t> selected, bool pass)
            {
                return m_decision &&
                       m_decision->submit(std::move(selected), pass);
            }

            /** @brief 当前四面板值快照（主线程渲染读）。
             * @return 常引用，指向共享模型内的快照，生命周期同本对象。 */
            const UiSnapshot &snapshot() const noexcept
            {
                return m_model->snapshot;
            }

            /** @brief 当前日志行值拷贝（主线程渲染读）。
             * @return 常引用，指向共享模型内的日志拷贝，生命周期同本对象。 */
            const std::vector<std::string> &log_lines() const noexcept
            {
                return m_model->log_lines;
            }

            /** @brief worker 是否在推进引擎。
             * @return `true` 表示后台作业进行中。 */
            bool running() const noexcept { return m_model->running.load(); }

            /** @brief 退出时写入的自动存档结果文案。
             * @return 常引用；未触发自动存档时为空串。 */
            const std::string &exit_message() const noexcept
            {
                return exit_message_;
            }

        private:
            std::shared_ptr<UiModel> m_model = std::make_shared<UiModel>();
            tkw::cli::Options m_base;
            std::string m_viewer = "P0";
            Post m_post;
            std::function<void()> on_quit_;
            bool quit_requested_ = false;

            // 声明序 = 析构保证：m_session 先、m_log/stats 后、决策源与 m_worker 最后。
            // m_adapter 只持 m_decision 的裸指针，声明在 m_decision 之后，析构先于它。
            tkw::cli::Session m_session;
            LogBuffer m_log;
            std::vector<tkw::EventBus::Handle> stats_handles_;
            std::atomic<bool> m_cancel{false};
            std::string exit_message_;
            std::shared_ptr<TuiDecisionSource> m_decision;
            std::unique_ptr<tkw::game::ai::RequestDecisionSource> m_adapter;
            std::jthread m_worker;

            /** @brief 若 worker 可 join 则等待结束；非阻塞于运行中的选择由调用方门控。 */
            void join_worker()
            {
                if (m_worker.joinable())
                    m_worker.join();
            }

            /** @brief 主线程 Idle 时由会话重建模型（快照 + 日志拷贝）。 */
            void refresh_model()
            {
                m_model->snapshot = make_snapshot(m_session, m_viewer);
                m_model->log_lines.assign(m_log.lines().begin(), m_log.lines().end());
            }

            /**
             * @brief  追加一行用户反馈。
             * @param[in] line 反馈文本。
             * @note   引擎运行中只写模型日志（`m_log` 归 worker）；Idle 时写 `m_log`
             *         并整体刷新模型（状态提示可能与状态变化同批出现）。
             */
            void append_line(std::string line);

            /** @brief Idle 门：运行中拒绝并提示；否则 join 已结束的 worker。
             * @return 可执行返回 true；运行中返回 false 并已写提示。 */
            bool require_idle();

            /** @brief 全 AI 决策源：按档取 Simple/Aggressive。
             * @param[in] ai AI 难度档。
             * @return 对应档位的决策源实例。 */
            static std::unique_ptr<tkw::game::DecisionSource> make_ai(
                tkw::cli::AiLevel ai);

            /** @brief 真人局非真人座位的回落决策器：按档取 Simple/Aggressive。
             * @param[in] ai AI 难度档。
             * @return 对应档位的回落决策器实例。 */
            static std::unique_ptr<tkw::game::ai::Decider> make_fallback_decider(
                tkw::cli::AiLevel ai);

            /**
             * @brief  按真人座位重建决策源与适配器（仅在 Idle 调用）。
             * @param[in] humans 本会话真人座位；空表示全 AI，清空决策源。
             * @param[in] ai     非真人座位的回落难度档。
             * @note   先析构适配器再析构决策源；唤醒回调投递空事件触发重绘，即将
             *         阻塞回调在真人决策阻塞前回送最新快照，使刚摸的牌即时可见。
             */
            void rebuild_decision_source(const std::vector<std::string> &humans,
                                         tkw::cli::AiLevel ai);

            /**
             * @brief  建局并开局：先退订旧句柄、再绑新局日志/统计、最后落会话。
             * @param[in] opt 启动选项（牌表/人数/种子/AI/真人座位/英雄）。
             * @note   失败路径不改动旧会话；新局建好后旧 game 才被替换，替换前已
             *         退订旧总线句柄。
             */
            void start_game(const tkw::cli::Options &opt);

            /** @brief 满足前置则执行一个回合（to_end=false）或跑到底（true）。
             * @param[in] to_end 是否跑到底。 */
            void do_step(bool to_end);

            /** @brief 启动 worker 作业；运行中拒绝，运行前 join 上一个 worker。
             * @param[in] to_end 是否跑到底。 */
            void start_job(bool to_end);

            /**
             * @brief  worker 作业体：可选跑到底，每回合经 `Post` 回送一帧。
             * @param[in] to_end 是否跑到底。
             * @note   只在该线程读 `m_session`/`m_log`；结算错误与平局写入日志后退出。
             */
            void run_job(bool to_end);

            /**
             * @brief  把 CLI 同口径统计块逐行写入日志（终局用）。
             * @param[in] winner 统计块「胜者」字段：乱斗原始 id（空 = 「无」）、
             *                   身份局阵营标签。
             * @note   只在 worker 内、终局后调用；纯文本构造自
             *         `cli::detail::battle_stats_lines`，与 CLI 统计块逐字一致。
             *         取消路径不调用。
             */
            void append_battle_stats(const std::string &winner);

            /** @brief worker 内构造值快照与日志拷贝，经 Post 交给主线程写模型。 */
            void post_snapshot();

            /** @brief 经 Post 清除运行标志；闭包只捕获模型 shared_ptr。 */
            void post_done();

            /**
             * @brief  启动批量模拟 worker；不要求活动会话，运行中拒绝。
             * @param[in] cmd 解析后的 simulate 命令（局数 + 选项）。
             * @note   simulate 不读写 `m_session`，只经合成选项自建独立对局，故单
             *         会话状态机零冲突；运行期间 q 可取消（`simulate_lines` 在局
             *         边界检查 `m_cancel`），最坏 join 延迟 = 单局时长。
             */
            void start_simulate_job(const Command &cmd);

            /**
             * @brief  worker 作业体：批量模拟 N 局全 AI，聚合结果逐行写入日志。
             * @param[in] cmd 解析后的 simulate 命令。
             * @note   选项合成复用 `query_options`（行内 `--deck` > 活动会话 >
             *         启动），再补非 deck 的行内覆盖；humans 已清空。
             *         `simulate_lines` 每局自建 `Game`/`SeededRng`，不读写
             *         `m_session`/活动会话 rng，亦不订阅事件。
             */
            void simulate_job(const Command &cmd);

            /** @brief 追加会话状态摘要；只读模型，不触引擎，运行中亦可用。 */
            void do_status();

            /**
             * @brief  只读牌表查询的选项合并：行内覆盖优先，否则活动会话优先。
             * @param[in] cmd 本行命令；`deck_provided` 为真时以其 deck 压过其它来源。
             * @return 以 `m_base` 为基准的选项副本：活动会话存在时 deck 取会话牌表，
             *         再被行内 `--deck` 覆盖；humans 一律清空。
             * @note   只读命令不运行对局，继承启动 `--human` 会被 reject_humans 误拒，
             *         故构造时清空；deck 优先序为 行内 > 活动会话 > 启动 base，与
             *         CLI 只读命令的 `options_from_for_query` 逐条一致；查询不写回
             *         `m_session.deck`，不影响 load 的存档指纹匹配口径。
             */
            tkw::cli::Options query_options(const Command &cmd) const;

            /** @brief 把只读查询行结果写入日志；Err 作为单行错误提示。
             * @param[in] lines 查询结果或错误。 */
            void append_query_lines(const tkw::cli::detail::QueryLines &lines);

            /** @brief cards 结果就地写日志（Idle 主线程，不触引擎）。
             * @param[in] cmd 解析后的 cards 命令。 */
            void do_cards(const Command &cmd);

            /** @brief rules 结果就地写日志（keyword 空 = 全部）。
             * @param[in] cmd 解析后的 rules 命令。 */
            void do_rules(const Command &cmd);

            /** @brief audit 结果就地写日志。
             * @param[in] cmd 解析后的 audit 命令。 */
            void do_audit(const Command &cmd);

            /** @brief decks 结果就地写日志（扫描根取查询牌表目录）。
             * @param[in] cmd 解析后的 decks 命令。 */
            void do_decks(const Command &cmd);

            /** @brief heroes 结果就地写日志（武将数据根取查询牌表目录）。
             * @param[in] cmd 解析后的 heroes 命令。 */
            void do_heroes(const Command &cmd);

            /** @brief 显式存档：写 AI 档与统计元数据，失败给中文根因。
             * @param[in] file 目标存档路径。 */
            void do_save(const std::string &file);

            /**
             * @brief  读档并落会话：占位建局固定 Brawl，模式/角色由存档恢复；
             *         AI 档优先取存档、未知文本回落 base。
             * @param[in] file 源存档路径。
             * @note   全部前置校验通过后才替换会话，失败不污染当前对局。
             */
            void do_load(const std::string &file);

            /**
             * @brief  追加命令表（help/? [关键词]）；与启动 --help 同源。
             * @param[in] cmd 解析后的 Help 命令；keyword 空 = 全量表，非空 = 过滤。
             */
            void do_help(const Command &cmd);

            /**
             * @brief 退出自动存档：对齐 REPL 语义（有活动会话且路径非空）。
             * @note 结果写入 exit_message_，由入口在事件循环结束后写 stderr，
             *       避免退出后面板不可见导致失败静默。
             */
            void autosave();
        };
    }  // namespace tui
}  // namespace tkw

#endif  // INCLUDE_TKW_TUI_CONTROLLER_HPP
