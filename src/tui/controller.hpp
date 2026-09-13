/**
 * @file controller.hpp
 * @brief TUI 会话控制器：命令驱动、后台引擎线程、日志订阅重绑与退出自动存档。
 * @note 无 FTXUI、无终端：可脱离 TTY 做单元测试。跨线程只传不可变值——worker
 *       构造值快照与日志拷贝，经注入的 Post 回送主线程；Post 闭包只捕获
 *       shared_ptr<UiModel>，不捕获 this，避免退出后触碰已析构对象。
 *       同一时刻 session_/log_ 只被一个线程访问：Idle 归主线程，Running 归
 *       worker；cancel_ 为原子取消位，worker 在回合边界检查后退出。
 *       成员声明序即析构保证：worker_ 最后声明、最先析构（自动 join），随后
 *       stats/log 句柄退订，最后 session_ 析构（含 Game/总线）。
 */
#ifndef INCLUDE_TKW_TUI_CONTROLLER_HPP
#define INCLUDE_TKW_TUI_CONTROLLER_HPP

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "cli/error_zh.hpp"
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
#include "tui/log_lines.hpp"
#include "tui/snapshot.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace tui
    {
        /** 主线程渲染读取的共享模型：值快照 + 日志值拷贝 + 运行标志。 */
        struct UiModel
        {
            UiSnapshot snapshot;                /**< 四面板值快照 */
            std::vector<std::string> log_lines; /**< 日志值拷贝（主线程渲染读） */
            std::atomic<bool> running{false};   /**< worker 是否在推进引擎 */
        };

        /**
         * @class Controller
         * @brief 驱动一局会话的命令控制器：解析命令、调度 worker、回送快照、退出存档。
         * @note 线程契约：new/load/save/status 只在 Idle 的主线程执行；step/run 在
         *       worker 执行。worker 期间主线程不读 session_/log_，只读 UiModel。
         *       析构不显式 join：成员声明序保证 worker_ 最先析构并自动 join。
         */
        class Controller
        {
        public:
            /** Post 接缝：把回调投递到 UI 主循环（FTXUI 为 screen.Post）。 */
            using Post = std::function<void(std::function<void()>)>;

            /**
             * @brief 构造控制器。
             * @param post 回送接缝；空时使用同步就地执行的默认实现（测试/无 UI）。
             */
            explicit Controller(Post post = {}) { set_post(std::move(post)); }

            Controller(const Controller &) = delete;
            Controller &operator=(const Controller &) = delete;

            /**
             * @brief 替换回送接缝；空 Post 回落同步就地执行。
             * @param post 新的 Post；不得在 worker 运行中切换。
             */
            void set_post(Post post)
            {
                if (post)
                    post_ = std::move(post);
                else
                    post_ = [](std::function<void()> fn) { fn(); };
            }

            /** @brief 设置退出回调（UI 侧退出事件循环）；在 request_quit 末尾调用。 */
            void set_on_quit(std::function<void()> on_quit)
            {
                on_quit_ = std::move(on_quit);
            }

            /** @brief 设置启动选项基准（deck/players/seed/ai/autosave 等）。 */
            void set_base_options(tkw::cli::Options options)
            {
                base_ = std::move(options);
            }

            /**
             * @brief 建默认对局并绑定日志/统计，随后刷新模型。
             * @note 建局失败只写日志提示，不抛异常；初始摸牌事件先于开局发牌订阅。
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
             * @brief 解析并执行一行命令；反馈一律写入日志。
             * @param line 用户输入行；空白行与解析失败只提示不改状态。
             * @note 主线程调用；new/load/save 在 worker 运行中被拒绝，step/run 在
             *       运行中同样拒绝（worker 内另有一道 running 门）。
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
                    do_help();
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

            const UiSnapshot &snapshot() const noexcept
            {
                return model_->snapshot;
            }

            const std::vector<std::string> &log_lines() const noexcept
            {
                return model_->log_lines;
            }

            bool running() const noexcept { return model_->running.load(); }

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

            // 声明序 = 析构保证：session_ 先、log_/stats 后、worker_ 最后。
            tkw::cli::Session session_;
            LogBuffer log_;
            std::vector<tkw::EventBus::Handle> stats_handles_;
            std::atomic<bool> cancel_{false};
            std::string exit_message_;
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
             * @brief 追加一行用户反馈。
             * @note 引擎运行中只写模型日志（log_ 归 worker）；Idle 时写 log_ 并
             *       整体刷新模型（状态提示可能与状态变化同批出现）。
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

            /** @brief Idle 门：运行中拒绝并提示；否则 join 已结束的 worker。 */
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

            /** @brief 建决策源：M1 全 AI，按档取 Simple/Aggressive。 */
            static std::unique_ptr<tkw::game::DecisionSource> make_ai(
                tkw::cli::AiLevel ai)
            {
                if (ai == tkw::cli::AiLevel::Aggressive)
                    return std::make_unique<tkw::game::AggressiveAI>();
                return std::make_unique<tkw::game::SimpleAI>();
            }

            /**
             * @brief 建局并开局：先退订旧句柄、再绑新局日志/统计、最后落会话。
             * @note 失败路径不改动旧会话；新局建好后旧 game 才被替换，替换前已
             *       退订旧总线句柄。
             */
            void start_game(const tkw::cli::Options &opt)
            {
                auto built = tkw::game::build_game(tkw::game::BuildOptions{
                    opt.deck, opt.players, opt.seed, opt.mode});
                if (built.is_err())
                {
                    append_line(
                        tkw::cli::detail::format_build_error(built.unwrap_err()));
                    return;
                }
                auto game = std::move(built).unwrap();

                if (!opt.humans.empty())
                {
                    append_line(detail::human_rejected_error());
                    return;
                }

                // 旧局仍在：先退订旧总线句柄，避免替换 Game 后向已释放总线退订。
                log_.unbind();
                stats_handles_.clear();
                session_.stats = tkw::cli::BattleStats{};

                // 日志订阅先于开局发牌，初始摸牌事件才会落入日志面板。
                log_.bind(*game);
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
                refresh_model();
                append_line("新对局已开始");
            }

            /** @brief 满足前置则执行一个回合（to_end=false）或跑到底（true）。 */
            void do_step(bool to_end)
            {
                if (!session_.active || !session_.game)
                {
                    append_line("没有进行中的对局（先运行 new 开局）");
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

            /** @brief 启动 worker 作业；运行中拒绝，运行前 join 上一个 worker。 */
            void start_job(bool to_end)
            {
                if (model_->running.load())
                {
                    append_line("引擎运行中，请等待当前命令完成");
                    return;
                }
                if (!session_.active || !session_.game)
                {
                    append_line("没有进行中的对局（先运行 new 开局）");
                    return;
                }
                join_worker();
                cancel_ = false;
                model_->running = true;
                worker_ = std::jthread([this, to_end] { run_job(to_end); });
            }

            /**
             * @brief worker 作业体：可选跑到底，每回合经 Post 回送一帧。
             * @note 只在该线程读 session_/log_；结算错误与平局写入日志后退出。
             */
            void run_job(bool to_end)
            {
                auto ctx = session_.game->context();
                auto ai = make_ai(session_.ai);

                post_snapshot();
                while (!cancel_.load() && !tkw::game::session_over(ctx))
                {
                    const std::string actor =
                        session_.state.current;  // 失败会推进，须先捕获
                    tkw::game::TurnError root =
                        tkw::game::TurnError::PlayRejected;
                    auto r =
                        tkw::game::step_session(ctx, *ai, session_.state, &root);
                    if (r.is_err())
                    {
                        if (r.unwrap_err() == tkw::game::LoopError::MaxRounds)
                            log_.push("平局（达到最大回合数）");
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
                post_done();
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

            /** @brief 追加会话状态摘要；只读模型，不触引擎，运行中亦可用。 */
            void do_status()
            {
                const UiSnapshot &snap = model_->snapshot;
                std::string text;
                if (!snap.active)
                    text = "没有进行中的对局";
                else if (snap.over)
                    text = "会话: 已结束  胜者: " + snap.winner_label;
                else
                    text = "第 " + std::to_string(snap.turns) +
                           " 回合  下一回合: " + snap.current + "  AI: " +
                           detail::ai_level_name(snap.ai) + "  摸牌堆 " +
                           std::to_string(snap.draw_size) + "  弃牌堆 " +
                           std::to_string(snap.discard_size);
                append_line("状态: " + text);
            }

            /** @brief 显式存档：写 AI 档与统计元数据，失败给中文根因。 */
            void do_save(const std::string &file)
            {
                if (!session_.active || !session_.game)
                {
                    append_line("没有进行中的对局（先运行 new 开局）");
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
             * @brief 读档并落会话：占位建局固定 Brawl，模式/角色由存档恢复；
             *        AI 档优先取存档、未知文本回落 base。
             * @note 全部前置校验通过后才替换会话，失败不污染当前对局。
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
                if (!base_.humans.empty())
                {
                    append_line(detail::human_rejected_error());
                    return;
                }

                tkw::cli::AiLevel ai = base_.ai;
                tkw::cli::AiLevel saved = tkw::cli::AiLevel::Simple;
                if (!meta.ai.empty() && detail::ai_from(meta.ai, saved))
                    ai = saved;

                log_.unbind();
                stats_handles_.clear();
                session_.stats = std::move(meta.stats);
                log_.bind(*game);
                stats_handles_ =
                    tkw::cli::detail::subscribe_stats(*game, session_.stats);
                session_.game = std::move(game);
                session_.state = std::move(state);
                session_.humans = base_.humans;
                session_.ai = ai;
                session_.deck = base_.deck;
                session_.active = true;
                refresh_model();
                append_line("已加载: " + file);
            }

            /** @brief 追加命令表（help/?）。 */
            void do_help()
            {
                append_line("命令: new [--players N] [--seed S] [--mode "
                            "brawl|identity] [--ai simple|aggressive] [--deck P] "
                            "[--hand N]");
                append_line("      deal <players> <seed>；step；run/r；status/st；"
                            "save <file>；load <file>；quit/q；help/?");
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
