/**
 * @file   controller.hpp
 * @brief  TUI 会话控制器：命令驱动、后台引擎线程、日志订阅重绑与退出自动存档。
 * @details 无 FTXUI、无终端：可脱离 TTY 做单元测试。跨线程只传不可变值——worker
 *          构造值快照与日志拷贝，经注入的 `Post` 回送主线程；`Post` 闭包只捕获
 *          `shared_ptr<UiModel>`，不捕获 `this`，避免退出后触碰已析构对象。
 *          同一时刻 `session_`/`log_` 只被一个线程访问：Idle 归主线程，Running
 *          归 worker；`cancel_` 为原子取消位，worker 在回合边界检查后退出。
 * @warning 成员声明序即析构保证：`worker_` 最后声明、最先析构（自动 join），
 *          随后 stats/log 句柄退订，最后 `session_` 析构（含 Game/总线）。
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
         *        主线程不读 `session_`/`log_`，只读 `UiModel`。
         * @warning 不可拷贝：持后台 worker 与订阅句柄；析构不显式 join，成员声明序
         *          保证 `worker_` 最先析构并自动 join。
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
                    post_ = std::move(post);
                else
                    post_ = [](std::function<void()> fn) { fn(); };
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
                base_ = std::move(options);
            }

            /**
             * @brief 建默认对局并绑定日志/统计，随后刷新模型。
             * @note  建局失败只写日志提示，不抛异常；初始摸牌事件先于开局发牌订阅。
             */
            void bootstrap()
            {
                session_.base = base_;
                start_game(base_);
                if (!session_.active)
                    append_line("输入命令：new / deal / step / run / status / "
                                "save / load / quit（help 查看用法）");
            }

            /**
             * @brief  解析并执行一行命令；反馈一律写入日志。
             * @param[in] line 用户输入行；空白行与解析失败只提示不改状态。
             * @note   主线程调用；new/load/save 在 worker 运行中被拒绝，step/run
             *         在运行中同样拒绝（worker 内另有一道 running 门）。
             */
            void execute_line(std::string_view line)
            {
                auto parsed = parse_command(line, base_);
                if (parsed.is_err())
                {
                    append_line(parsed.unwrap_err());
                    return;
                }
                const auto &cmd = parsed.unwrap();
                switch (cmd.kind)
                {
                case CommandKind::New:
                    if (require_idle())
                        start_game(cmd.options);
                    break;
                case CommandKind::Deal:
                    if (!require_idle())
                        break;
                    start_game(cmd.options);
                    if (session_.active && session_.game)
                        start_job(cmd.run_to_end);
                    break;
                case CommandKind::Step:
                    if (require_idle())
                        do_step(false);
                    break;
                case CommandKind::Run:
                    if (require_idle())
                        do_step(true);
                    break;
                case CommandKind::Status:
                    do_status();
                    break;
                case CommandKind::Save:
                    if (require_idle())
                        do_save(cmd.file);
                    break;
                case CommandKind::Load:
                    if (require_idle())
                        do_load(cmd.file);
                    break;
                case CommandKind::Quit:
                    request_quit();
                    break;
                case CommandKind::Help:
                    do_help(cmd);
                    break;
                case CommandKind::Cards:
                    if (require_idle())
                        do_cards(cmd);
                    break;
                case CommandKind::Rules:
                    if (require_idle())
                        do_rules(cmd);
                    break;
                case CommandKind::Audit:
                    if (require_idle())
                        do_audit(cmd);
                    break;
                case CommandKind::Decks:
                    if (require_idle())
                        do_decks(cmd);
                    break;
                case CommandKind::Heroes:
                    if (require_idle())
                        do_heroes(cmd);
                    break;
                case CommandKind::Simulate:
                    if (require_idle())
                        start_simulate_job(cmd);
                    break;
                }
            }

            /**
             * @brief 取消 worker、自动存档、退订句柄，最后触发退出回调。
             * @note 关停顺序不可颠倒：join 后引擎静默，才能读会话写存档与退订；
             *       幂等，重复调用直接返回。
             */
            void request_quit()
            {
                if (quit_requested_)
                    return;
                quit_requested_ = true;
                // 先唤醒可能阻塞在真人待决的 worker，再置取消位并 join，避免互等。
                if (decision_)
                    decision_->cancel();
                cancel_ = true;
                join_worker();
                autosave();
                log_.unbind();
                stats_handles_.clear();
                if (on_quit_)
                    on_quit_();
            }

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
                return decision_ && decision_->fetch_new(out);
            }

            /** @brief 是否存在未被取走的真人待决。
             * @return 有待决且已绑定真人决策源时为 true。 */
            bool has_pending_decision() const
            {
                return decision_ && decision_->has_pending();
            }

            /**
             * @brief  提交真人待决的选择；非法选择被忽略且保持待决。
             * @param[in] selected 选中候选的 0 基下标；Discard 为多选。
             * @param[in] pass     是否放弃。
             * @return 被接受返回 true；无待决/选择非法返回 false。
             */
            bool submit_decision(std::vector<std::size_t> selected, bool pass)
            {
                return decision_ &&
                       decision_->submit(std::move(selected), pass);
            }

            /** @brief 当前四面板值快照（主线程渲染读）。
             * @return 常引用，指向共享模型内的快照，生命周期同本对象。 */
            const UiSnapshot &snapshot() const noexcept
            {
                return model_->snapshot;
            }

            /** @brief 当前日志行值拷贝（主线程渲染读）。
             * @return 常引用，指向共享模型内的日志拷贝，生命周期同本对象。 */
            const std::vector<std::string> &log_lines() const noexcept
            {
                return model_->log_lines;
            }

            /** @brief worker 是否在推进引擎。
             * @return `true` 表示后台作业进行中。 */
            bool running() const noexcept { return model_->running.load(); }

            /** @brief 退出时写入的自动存档结果文案。
             * @return 常引用；未触发自动存档时为空串。 */
            const std::string &exit_message() const noexcept
            {
                return exit_message_;
            }

        private:
            std::shared_ptr<UiModel> model_ = std::make_shared<UiModel>();
            tkw::cli::Options base_;
            std::string viewer_ = "P0";
            Post post_;
            std::function<void()> on_quit_;
            bool quit_requested_ = false;

            // 声明序 = 析构保证：session_ 先、log_/stats 后、决策源与 worker_ 最后。
            // adapter_ 只持 decision_ 的裸指针，声明在 decision_ 之后，析构先于它。
            tkw::cli::Session session_;
            LogBuffer log_;
            std::vector<tkw::EventBus::Handle> stats_handles_;
            std::atomic<bool> cancel_{false};
            std::string exit_message_;
            std::shared_ptr<TuiDecisionSource> decision_;
            std::unique_ptr<tkw::game::ai::RequestDecisionSource> adapter_;
            std::jthread worker_;

            /** @brief 若 worker 可 join 则等待结束；非阻塞于运行中的选择由调用方门控。 */
            void join_worker()
            {
                if (worker_.joinable())
                    worker_.join();
            }

            /** @brief 主线程 Idle 时由会话重建模型（快照 + 日志拷贝）。 */
            void refresh_model()
            {
                model_->snapshot = make_snapshot(session_, viewer_);
                model_->log_lines.assign(log_.lines().begin(), log_.lines().end());
            }

            /**
             * @brief  追加一行用户反馈。
             * @param[in] line 反馈文本。
             * @note   引擎运行中只写模型日志（`log_` 归 worker）；Idle 时写 `log_`
             *         并整体刷新模型（状态提示可能与状态变化同批出现）。
             */
            void append_line(std::string line)
            {
                if (model_->running.load())
                {
                    auto &lines = model_->log_lines;
                    lines.push_back(std::move(line));
                    const std::size_t cap = log_.capacity();
                    if (lines.size() > cap)
                        lines.erase(lines.begin(),
                                    lines.begin() +
                                        static_cast<std::ptrdiff_t>(lines.size() -
                                                                    cap));
                    return;
                }
                log_.push(std::move(line));
                refresh_model();
            }

            /** @brief Idle 门：运行中拒绝并提示；否则 join 已结束的 worker。
             * @return 可执行返回 true；运行中返回 false 并已写提示。 */
            bool require_idle()
            {
                if (model_->running.load())
                {
                    append_line("引擎运行中，请等待当前命令完成");
                    return false;
                }
                join_worker();
                return true;
            }

            /** @brief 全 AI 决策源：按档取 Simple/Aggressive。
             * @param[in] ai AI 难度档。
             * @return 对应档位的决策源实例。 */
            static std::unique_ptr<tkw::game::DecisionSource> make_ai(
                tkw::cli::AiLevel ai)
            {
                if (ai == tkw::cli::AiLevel::Aggressive)
                    return std::make_unique<tkw::game::AggressiveAI>();
                return std::make_unique<tkw::game::SimpleAI>();
            }

            /** @brief 真人局非真人座位的回落决策器：按档取 Simple/Aggressive。
             * @param[in] ai AI 难度档。
             * @return 对应档位的回落决策器实例。 */
            static std::unique_ptr<tkw::game::ai::Decider> make_fallback_decider(
                tkw::cli::AiLevel ai)
            {
                if (ai == tkw::cli::AiLevel::Aggressive)
                    return std::make_unique<tkw::game::ai::AggressiveDecider>();
                return std::make_unique<tkw::game::ai::SimpleDecider>();
            }

            /**
             * @brief  按真人座位重建决策源与适配器（仅在 Idle 调用）。
             * @param[in] humans 本会话真人座位；空表示全 AI，清空决策源。
             * @param[in] ai     非真人座位的回落难度档。
             * @note   先析构适配器再析构决策源；唤醒回调投递空事件触发重绘，即将
             *         阻塞回调在真人决策阻塞前回送最新快照，使刚摸的牌即时可见。
             */
            void rebuild_decision_source(const std::vector<std::string> &humans,
                                         tkw::cli::AiLevel ai)
            {
                adapter_.reset();
                decision_.reset();
                if (humans.empty())
                    return;
                auto source = std::make_shared<TuiDecisionSource>(
                    humans, make_fallback_decider(ai));
                source->set_notify([this] { post_([] {}); });
                source->set_on_wait([this] { post_snapshot(); });
                decision_ = std::move(source);
                adapter_ = std::make_unique<tkw::game::ai::RequestDecisionSource>(
                    *decision_);
            }

            /**
             * @brief  建局并开局：先退订旧句柄、再绑新局日志/统计、最后落会话。
             * @param[in] opt 启动选项（牌表/人数/种子/AI/真人座位/英雄）。
             * @note   失败路径不改动旧会话；新局建好后旧 game 才被替换，替换前已
             *         退订旧总线句柄。
             */
            void start_game(const tkw::cli::Options &opt)
            {
                // --hero 解析失败（格式/座位/重复）先于任何会话改动返回，旧局不受影响。
                auto bo =
                    tkw::cli::detail::build_options_with_heroes(opt, opt.mode);
                if (bo.is_err())
                {
                    append_line(bo.unwrap_err());
                    return;
                }
                auto built = tkw::game::build_game(bo.unwrap());
                if (built.is_err())
                {
                    append_line(
                        tkw::cli::detail::format_build_error(built.unwrap_err()));
                    return;
                }
                auto game = std::move(built).unwrap();

                // 未实现卡/技能警告只提示不阻断，与 CLI new 同口径。
                for (const auto &line :
                     tkw::cli::detail::unsupported_cards_warning_lines(
                         game->catalog))
                    append_line(line);
                for (const auto &line :
                     tkw::cli::detail::unsupported_hero_skills_warning_lines(
                         *game))
                    append_line(line);

                const std::string verr =
                    tkw::cli::detail::validate_humans(*game, opt.humans);
                if (!verr.empty())
                {
                    append_line(verr);
                    return;
                }

                // 旧局仍在：先退订旧总线句柄，避免替换 Game 后向已释放总线退订。
                log_.unbind();
                stats_handles_.clear();
                session_.stats = tkw::cli::BattleStats{};

                // 日志订阅先于开局发牌，初始摸牌事件才会落入日志面板。
                log_.bind(*game, opt.humans);
                stats_handles_ =
                    tkw::cli::detail::subscribe_stats(*game, session_.stats);

                tkw::game::GameSession state;
                auto ctx = game->context();
                if (tkw::game::start_session(ctx, state, "P0", opt.hand).is_err())
                {
                    log_.unbind();
                    stats_handles_.clear();
                    append_line("开局失败：场上没有玩家");
                    return;
                }

                session_.game = std::move(game);
                session_.state = std::move(state);
                session_.humans = opt.humans;
                session_.ai = opt.ai;
                session_.deck = opt.deck;
                session_.active = true;
                viewer_ = opt.humans.empty() ? "P0" : opt.humans.front();
                rebuild_decision_source(opt.humans, opt.ai);
                refresh_model();
                append_line("新对局已开始");
            }

            /** @brief 满足前置则执行一个回合（to_end=false）或跑到底（true）。
             * @param[in] to_end 是否跑到底。 */
            void do_step(bool to_end)
            {
                if (!session_.active || !session_.game)
                {
                    append_line("没有进行中的对局（先运行 new 开局；help 查看用法）");
                    return;
                }
                auto ctx = session_.game->context();
                if (tkw::game::session_over(ctx))
                {
                    append_line("对局已结束，胜者: " +
                                model_->snapshot.winner_label);
                    return;
                }
                start_job(to_end);
            }

            /** @brief 启动 worker 作业；运行中拒绝，运行前 join 上一个 worker。
             * @param[in] to_end 是否跑到底。 */
            void start_job(bool to_end)
            {
                if (model_->running.load())
                {
                    append_line("引擎运行中，请等待当前命令完成");
                    return;
                }
                if (!session_.active || !session_.game)
                {
                    append_line("没有进行中的对局（先运行 new 开局；help 查看用法）");
                    return;
                }
                join_worker();
                cancel_ = false;
                model_->running = true;
                worker_ = std::jthread([this, to_end] { run_job(to_end); });
            }

            /**
             * @brief  worker 作业体：可选跑到底，每回合经 `Post` 回送一帧。
             * @param[in] to_end 是否跑到底。
             * @note   只在该线程读 `session_`/`log_`；结算错误与平局写入日志后退出。
             */
            void run_job(bool to_end)
            {
                auto ctx = session_.game->context();
                // 真人局走适配后的接入源，全 AI 局走既有单一决策源；两者只取其一。
                std::unique_ptr<tkw::game::DecisionSource> ai;
                tkw::game::DecisionSource *source = nullptr;
                if (adapter_)
                {
                    source = adapter_.get();
                }
                else
                {
                    ai = make_ai(session_.ai);
                    source = ai.get();
                }

                post_snapshot();
                // MaxRounds 分支已自行输出平局与统计块；置位后不再落入下方正常终局块。
                // 该分支只在未终局且越上限（仍有多名存活者）时到达，此局面 session_over
                // 为假、正常终局块本不会执行，置位作为防御，避免终局判定变化时重复写统计。
                bool drew = false;
                while (!cancel_.load() && !tkw::game::session_over(ctx))
                {
                    log_.push(tkw::cli::detail::turn_header_text(session_.state));
                    const std::string actor =
                        session_.state.current;  // 失败会推进，须先捕获
                    tkw::game::TurnError root =
                        tkw::game::TurnError::PlayRejected;
                    auto r = tkw::game::step_session(ctx, *source, session_.state,
                                                     &root);
                    if (r.is_err())
                    {
                        if (r.unwrap_err() == tkw::game::LoopError::MaxRounds)
                        {
                            log_.push("平局（达到最大回合数）");
                            append_battle_stats({});
                            drew = true;
                        }
                        else
                            log_.push(tkw::cli::detail::format_turn_failure(
                                r.unwrap_err(), root, actor));
                        post_snapshot();
                        break;
                    }
                    post_snapshot();
                    if (!to_end)
                        break;
                }
                // 正常终局补结束行与统计块（取消/失败/回合上限路径各自已有提示）。
                if (!drew && !cancel_.load() && tkw::game::session_over(ctx))
                {
                    log_.push("对局结束，胜者: " +
                              make_snapshot(session_, viewer_).winner_label);
                    append_battle_stats(
                        tkw::cli::detail::game_stats_label(ctx));
                    post_snapshot();
                }
                post_done();
            }

            /**
             * @brief  把 CLI 同口径统计块逐行写入日志（终局用）。
             * @param[in] winner 统计块「胜者」字段：乱斗原始 id（空 = 「无」）、
             *                   身份局阵营标签。
             * @note   只在 worker 内、终局后调用；纯文本构造自
             *         `cli::detail::battle_stats_lines`，与 CLI 统计块逐字一致。
             *         取消路径不调用。
             */
            void append_battle_stats(const std::string &winner)
            {
                for (auto &line : tkw::cli::detail::battle_stats_lines(
                         session_.stats, *session_.game, winner,
                         session_.state.turns))
                    log_.push(std::move(line));
            }

            /** @brief worker 内构造值快照与日志拷贝，经 Post 交给主线程写模型。 */
            void post_snapshot()
            {
                UiSnapshot snap = make_snapshot(session_, viewer_);
                std::vector<std::string> lines(log_.lines().begin(),
                                               log_.lines().end());
                auto model = model_;
                post_([model = std::move(model), snap = std::move(snap),
                       lines = std::move(lines)]() mutable
                      {
                          model->snapshot = std::move(snap);
                          model->log_lines = std::move(lines);
                      });
            }

            /** @brief 经 Post 清除运行标志；闭包只捕获模型 shared_ptr。 */
            void post_done()
            {
                auto model = model_;
                post_([model = std::move(model)] { model->running = false; });
            }

            /**
             * @brief  启动批量模拟 worker；不要求活动会话，运行中拒绝。
             * @param[in] cmd 解析后的 simulate 命令（局数 + 选项）。
             * @note   simulate 不读写 `session_`，只经合成选项自建独立对局，故单
             *         会话状态机零冲突；运行期间 q 可取消（`simulate_lines` 在局
             *         边界检查 `cancel_`），最坏 join 延迟 = 单局时长。
             */
            void start_simulate_job(const Command &cmd)
            {
                if (model_->running.load())
                {
                    append_line("引擎运行中，请等待当前命令完成");
                    return;
                }
                join_worker();
                cancel_ = false;
                model_->running = true;
                worker_ = std::jthread([this, cmd] { simulate_job(cmd); });
            }

            /**
             * @brief  worker 作业体：批量模拟 N 局全 AI，聚合结果逐行写入日志。
             * @param[in] cmd 解析后的 simulate 命令。
             * @note   选项合成复用 `query_options`（行内 `--deck` > 活动会话 >
             *         启动），再补非 deck 的行内覆盖；humans 已清空。
             *         `simulate_lines` 每局自建 `Game`/`SeededRng`，不读写
             *         `session_`/活动会话 rng，亦不订阅事件。
             */
            void simulate_job(const Command &cmd)
            {
                tkw::cli::Options opt = query_options(cmd);
                opt.players = cmd.options.players;
                opt.seed = cmd.options.seed;
                opt.ai = cmd.options.ai;
                opt.mode = cmd.options.mode;
                opt.hand = cmd.options.hand;

                log_.push("开始模拟 " + std::to_string(cmd.games) + " 局（" +
                          std::to_string(opt.players) + " 人，种子 " +
                          std::to_string(opt.seed) + ".." +
                          std::to_string(opt.seed +
                                          static_cast<std::uint32_t>(
                                              cmd.games) -
                                          1) +
                          "，ai=" + tkw::cli::detail::ai_level_name(opt.ai) +
                          "），请稍候…（q 可取消）");
                post_snapshot();

                std::vector<std::string> warnings;
                auto lines = tkw::cli::detail::simulate_lines(
                    opt, cmd.games, &warnings,
                    [this] { return cancel_.load(); });
                for (auto &line : warnings)
                    log_.push(std::move(line));
                if (lines.is_err())
                    log_.push(lines.unwrap_err());
                else
                    for (auto &line : lines.unwrap())
                        log_.push(std::move(line));

                post_snapshot();
                post_done();
            }

            /** @brief 追加会话状态摘要；只读模型，不触引擎，运行中亦可用。 */
            void do_status()
            {
                const UiSnapshot &snap = model_->snapshot;
                std::string text;
                if (!snap.active)
                    text = "没有进行中的对局";
                else if (snap.over)
                    text = "会话: 已结束  胜者: " + snap.winner_label +
                           "，存活: " + std::to_string(snap.alive);
                else
                    text = "第 " + std::to_string(snap.turns) +
                           " 回合  下一回合: " + snap.current + "  存活: " +
                           std::to_string(snap.alive) + "  AI: " +
                           detail::ai_level_name(snap.ai) + "  摸牌堆 " +
                           std::to_string(snap.draw_size) + "  弃牌堆 " +
                           std::to_string(snap.discard_size);
                append_line("状态: " + text);
            }

            /**
             * @brief  只读牌表查询的选项合并：行内覆盖优先，否则活动会话优先。
             * @param[in] cmd 本行命令；`deck_provided` 为真时以其 deck 压过其它来源。
             * @return 以 `base_` 为基准的选项副本：活动会话存在时 deck 取会话牌表，
             *         再被行内 `--deck` 覆盖；humans 一律清空。
             * @note   只读命令不运行对局，继承启动 `--human` 会被 reject_humans 误拒，
             *         故构造时清空；deck 优先序为 行内 > 活动会话 > 启动 base，与
             *         CLI 只读命令的 `options_from_for_query` 逐条一致；查询不写回
             *         `session_.deck`，不影响 load 的存档指纹匹配口径。
             */
            tkw::cli::Options query_options(const Command &cmd) const
            {
                tkw::cli::Options opt = base_;
                opt.humans.clear();
                if (session_.active && session_.game)
                    opt.deck = session_.deck;
                if (cmd.deck_provided)
                    opt.deck = cmd.options.deck;
                return opt;
            }

            /** @brief 把只读查询行结果写入日志；Err 作为单行错误提示。
             * @param[in] lines 查询结果或错误。 */
            void append_query_lines(const tkw::cli::detail::QueryLines &lines)
            {
                if (lines.is_err())
                {
                    append_line(lines.unwrap_err());
                    return;
                }
                for (const auto &line : lines.unwrap())
                    append_line(line);
            }

            /** @brief cards 结果就地写日志（Idle 主线程，不触引擎）。
             * @param[in] cmd 解析后的 cards 命令。 */
            void do_cards(const Command &cmd)
            {
                append_query_lines(tkw::cli::detail::cards_lines(
                    query_options(cmd), cmd.with_text));
            }

            /** @brief rules 结果就地写日志（keyword 空 = 全部）。
             * @param[in] cmd 解析后的 rules 命令。 */
            void do_rules(const Command &cmd)
            {
                append_query_lines(tkw::cli::detail::rules_lines(
                    query_options(cmd), cmd.keyword));
            }

            /** @brief audit 结果就地写日志。
             * @param[in] cmd 解析后的 audit 命令。 */
            void do_audit(const Command &cmd)
            {
                append_query_lines(
                    tkw::cli::detail::audit_lines(query_options(cmd)));
            }

            /** @brief decks 结果就地写日志（扫描根取查询牌表目录）。
             * @param[in] cmd 解析后的 decks 命令。 */
            void do_decks(const Command &cmd)
            {
                append_query_lines(
                    tkw::cli::detail::decks_lines(query_options(cmd).deck));
            }

            /** @brief heroes 结果就地写日志（武将数据根取查询牌表目录）。
             * @param[in] cmd 解析后的 heroes 命令。 */
            void do_heroes(const Command &cmd)
            {
                append_query_lines(
                    tkw::cli::detail::heroes_lines(query_options(cmd).deck));
            }

            /** @brief 显式存档：写 AI 档与统计元数据，失败给中文根因。
             * @param[in] file 目标存档路径。 */
            void do_save(const std::string &file)
            {
                if (!session_.active || !session_.game)
                {
                    append_line("没有进行中的对局（先运行 new 开局；help 查看用法）");
                    return;
                }
                tkw::save::SessionMeta meta;
                meta.ai = detail::ai_level_name(session_.ai);
                meta.stats = session_.stats;
                const std::string text =
                    tkw::save::write(*session_.game, session_.state, "deck", meta);
                const auto write = tkw::io::write_text_atomic(file, text);
                if (write.is_err())
                {
                    append_line(tkw::cli::render_write_error_zh(
                        file, write.unwrap_err()));
                    return;
                }
                append_line("已保存: " + file);
            }

            /**
             * @brief  读档并落会话：占位建局固定 Brawl，模式/角色由存档恢复；
             *         AI 档优先取存档、未知文本回落 base。
             * @param[in] file 源存档路径。
             * @note   全部前置校验通过后才替换会话，失败不污染当前对局。
             */
            void do_load(const std::string &file)
            {
                auto text = tkw::io::read_text(file);
                if (text.is_err())
                {
                    append_line(tkw::cli::render_read_error_zh(
                        file, text.unwrap_err()));
                    return;
                }
                auto built = tkw::game::build_game(tkw::game::BuildOptions{
                    base_.deck, base_.players, base_.seed,
                    tkw::game::GameMode::Brawl});
                if (built.is_err())
                {
                    append_line(
                        tkw::cli::detail::format_build_error(built.unwrap_err()));
                    return;
                }
                auto game = std::move(built).unwrap();

                tkw::game::GameSession state;
                tkw::save::SessionMeta meta;
                auto r = tkw::save::read(text.unwrap(), *game, state, &meta);
                if (r.is_err())
                {
                    append_line(tkw::cli::render_save_error_zh(
                        r.unwrap_err()));
                    return;
                }

                // 未实现卡警告只提示不阻断，与 CLI load 同口径。
                for (const auto &line :
                     tkw::cli::detail::unsupported_cards_warning_lines(
                         game->catalog))
                    append_line(line);

                const std::string verr =
                    tkw::cli::detail::validate_humans(*game, base_.humans);
                if (!verr.empty())
                {
                    append_line(verr);
                    return;
                }

                tkw::cli::AiLevel ai = base_.ai;
                tkw::cli::AiLevel saved = tkw::cli::AiLevel::Simple;
                if (!meta.ai.empty() && detail::ai_from(meta.ai, saved))
                    ai = saved;

                log_.unbind();
                stats_handles_.clear();
                session_.stats = std::move(meta.stats);
                log_.bind(*game, base_.humans);
                stats_handles_ =
                    tkw::cli::detail::subscribe_stats(*game, session_.stats);
                session_.game = std::move(game);
                session_.state = std::move(state);
                session_.humans = base_.humans;
                session_.ai = ai;
                session_.deck = base_.deck;
                session_.active = true;
                viewer_ = base_.humans.empty() ? "P0" : base_.humans.front();
                rebuild_decision_source(base_.humans, ai);
                refresh_model();
                append_line("已加载: " + file);
            }

            /**
             * @brief  追加命令表（help/? [关键词]）；与启动 --help 同源。
             * @param[in] cmd 解析后的 Help 命令；keyword 空 = 全量表，非空 = 过滤。
             */
            void do_help(const Command &cmd)
            {
                for (const auto &line : detail::query_help_lines(cmd.keyword))
                    append_line(line);
            }

            /**
             * @brief 退出自动存档：对齐 REPL 语义（有活动会话且路径非空）。
             * @note 结果写入 exit_message_，由入口在事件循环结束后写 stderr，
             *       避免退出后面板不可见导致失败静默。
             */
            void autosave()
            {
                if (!session_.active || !session_.game || base_.autosave.empty())
                    return;
                tkw::save::SessionMeta meta;
                meta.ai = detail::ai_level_name(session_.ai);
                meta.stats = session_.stats;
                const std::string text =
                    tkw::save::write(*session_.game, session_.state, "deck", meta);
                const std::string path = base_.autosave.string();
                if (tkw::io::write_text_atomic(base_.autosave, text).is_ok())
                    exit_message_ = "已自动存档: " + path;
                else
                    exit_message_ = "自动存档失败: " + path;
                append_line(exit_message_);
            }
        };
    }  // namespace tui
}  // namespace tkw

#endif  // INCLUDE_TKW_TUI_CONTROLLER_HPP
