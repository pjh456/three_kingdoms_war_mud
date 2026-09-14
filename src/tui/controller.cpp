/**
 * @file   controller.cpp
 * @brief  TUI 会话控制器实现：命令驱动、后台引擎线程、日志订阅重绑与退出自动存档。
 * @ingroup tkw_tui
 */
#include "tui/controller.hpp"

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

namespace tkw
{
    namespace tui
    {
        void Controller::bootstrap()
        {
            m_session.base = m_base;
            start_game(m_base);
            if (!m_session.active)
                append_line("输入命令：new / deal / step / run / status / "
                            "save / load / quit（help 查看用法）");
        }

        void Controller::execute_line(std::string_view line)
        {
            auto parsed = parse_command(line, m_base);
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
                if (m_session.active && m_session.game)
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

        void Controller::request_quit()
        {
            if (m_quit_requested)
                return;
            m_quit_requested = true;
            // 先唤醒可能阻塞在真人待决的 worker，再置取消位并 join，避免互等。
            if (m_decision)
                m_decision->cancel();
            m_cancel = true;
            join_worker();
            autosave();
            m_log.unbind();
            m_stats_handles.clear();
            if (m_on_quit)
                m_on_quit();
        }

        void Controller::append_line(std::string line)
        {
            if (m_model->running.load())
            {
                auto &lines = m_model->log_lines;
                lines.push_back(std::move(line));
                const std::size_t cap = m_log.capacity();
                if (lines.size() > cap)
                    lines.erase(lines.begin(),
                                lines.begin() +
                                    static_cast<std::ptrdiff_t>(lines.size() -
                                                                cap));
                return;
            }
            m_log.push(std::move(line));
            refresh_model();
        }

        bool Controller::require_idle()
        {
            if (m_model->running.load())
            {
                append_line("引擎运行中，请等待当前命令完成");
                return false;
            }
            join_worker();
            return true;
        }

        std::unique_ptr<tkw::game::DecisionSource> Controller::make_ai(
            tkw::cli::AiLevel ai)
        {
            if (ai == tkw::cli::AiLevel::Aggressive)
                return std::make_unique<tkw::game::AggressiveAI>();
            return std::make_unique<tkw::game::SimpleAI>();
        }

        std::unique_ptr<tkw::game::ai::Decider>
        Controller::make_fallback_decider(tkw::cli::AiLevel ai)
        {
            if (ai == tkw::cli::AiLevel::Aggressive)
                return std::make_unique<tkw::game::ai::AggressiveDecider>();
            return std::make_unique<tkw::game::ai::SimpleDecider>();
        }

        void Controller::rebuild_decision_source(
            const std::vector<std::string> &humans, tkw::cli::AiLevel ai)
        {
            m_adapter.reset();
            m_decision.reset();
            if (humans.empty())
                return;
            auto source = std::make_shared<TuiDecisionSource>(
                humans, make_fallback_decider(ai));
            source->set_notify([this] { m_post([] {}); });
            source->set_on_wait([this] { post_snapshot(); });
            m_decision = std::move(source);
            m_adapter = std::make_unique<tkw::game::ai::RequestDecisionSource>(
                *m_decision);
        }

        void Controller::start_game(const tkw::cli::Options &opt)
        {
            // --hero 解析失败（格式/座位/重复）先于任何会话改动返回，旧局不受影响。
            auto bo =
                tkw::cli::detail::build_options_with_heroes(opt, opt.mode);
            if (bo.is_err())
            {
                append_line(bo.unwrap_err());
                return;
            }
            auto built = tkw::game::GameFactory::build(bo.unwrap());
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
            m_log.unbind();
            m_stats_handles.clear();
            m_session.stats = tkw::cli::BattleStats{};

            // 日志订阅先于开局发牌，初始摸牌事件才会落入日志面板。
            m_log.bind(*game, opt.humans);
            m_stats_handles =
                tkw::cli::detail::subscribe_stats(*game, m_session.stats);

            tkw::game::GameSession state;
            auto ctx = game->context();
            if (tkw::game::GameSetup(ctx)
                    .start_session(state, "P0", opt.hand)
                    .is_err())
            {
                m_log.unbind();
                m_stats_handles.clear();
                append_line("开局失败：场上没有玩家");
                return;
            }

            m_session.game = std::move(game);
            m_session.state = std::move(state);
            m_session.humans = opt.humans;
            m_session.ai = opt.ai;
            m_session.deck = opt.deck;
            m_session.active = true;
            m_viewer = opt.humans.empty() ? "P0" : opt.humans.front();
            rebuild_decision_source(opt.humans, opt.ai);
            refresh_model();
            append_line("新对局已开始");
        }

        void Controller::do_step(bool to_end)
        {
            if (!m_session.active || !m_session.game)
            {
                append_line("没有进行中的对局（先运行 new 开局；help 查看用法）");
                return;
            }
            auto ctx = m_session.game->context();
            if (tkw::game::SessionQuery::session_over(ctx))
            {
                append_line("对局已结束，胜者: " +
                            m_model->snapshot.winner_label);
                return;
            }
            start_job(to_end);
        }

        void Controller::start_job(bool to_end)
        {
            if (m_model->running.load())
            {
                append_line("引擎运行中，请等待当前命令完成");
                return;
            }
            if (!m_session.active || !m_session.game)
            {
                append_line("没有进行中的对局（先运行 new 开局；help 查看用法）");
                return;
            }
            join_worker();
            m_cancel = false;
            m_model->running = true;
            m_worker = std::jthread([this, to_end] { run_job(to_end); });
        }

        void Controller::run_job(bool to_end)
        {
            auto ctx = m_session.game->context();
            // 真人局走适配后的接入源，全 AI 局走既有单一决策源；两者只取其一。
            std::unique_ptr<tkw::game::DecisionSource> ai;
            tkw::game::DecisionSource *source = nullptr;
            if (m_adapter)
            {
                source = m_adapter.get();
            }
            else
            {
                ai = make_ai(m_session.ai);
                source = ai.get();
            }

            post_snapshot();
            // MaxRounds 分支已自行输出平局与统计块；置位后不再落入下方正常终局块。
            // 该分支只在未终局且越上限（仍有多名存活者）时到达，此局面 session_over
            // 为假、正常终局块本不会执行，置位作为防御，避免终局判定变化时重复写统计。
            bool drew = false;
            while (!m_cancel.load() && !tkw::game::SessionQuery::session_over(ctx))
            {
                m_log.push(tkw::cli::detail::turn_header_text(m_session.state));
                const std::string actor =
                    m_session.state.current;  // 失败会推进，须先捕获
                tkw::game::TurnError root =
                    tkw::game::TurnError::PlayRejected;
                auto r = tkw::game::GameLoop(ctx, *source).step_session(
                    m_session.state, &root);
                if (r.is_err())
                {
                    if (r.unwrap_err() == tkw::game::LoopError::MaxRounds)
                    {
                        m_log.push("平局（达到最大回合数）");
                        append_battle_stats({});
                        drew = true;
                    }
                    else
                        m_log.push(tkw::cli::detail::format_turn_failure(
                            r.unwrap_err(), root, actor));
                    post_snapshot();
                    break;
                }
                post_snapshot();
                if (!to_end)
                    break;
            }
            // 正常终局补结束行与统计块（取消/失败/回合上限路径各自已有提示）。
            if (!drew && !m_cancel.load() &&
                tkw::game::SessionQuery::session_over(ctx))
            {
                m_log.push("对局结束，胜者: " +
                          make_snapshot(m_session, m_viewer).winner_label);
                append_battle_stats(
                    tkw::cli::detail::game_stats_label(ctx));
                post_snapshot();
            }
            post_done();
        }

        void Controller::append_battle_stats(const std::string &winner)
        {
            for (auto &line : tkw::cli::detail::battle_stats_lines(
                     m_session.stats, *m_session.game, winner,
                     m_session.state.turns))
                m_log.push(std::move(line));
        }

        void Controller::post_snapshot()
        {
            UiSnapshot snap = make_snapshot(m_session, m_viewer);
            std::vector<std::string> lines(m_log.lines().begin(),
                                           m_log.lines().end());
            auto model = m_model;
            m_post([model = std::move(model), snap = std::move(snap),
                   lines = std::move(lines)]() mutable
                  {
                      model->snapshot = std::move(snap);
                      model->log_lines = std::move(lines);
                  });
        }

        void Controller::post_done()
        {
            auto model = m_model;
            m_post([model = std::move(model)] { model->running = false; });
        }

        void Controller::start_simulate_job(const Command &cmd)
        {
            if (m_model->running.load())
            {
                append_line("引擎运行中，请等待当前命令完成");
                return;
            }
            join_worker();
            m_cancel = false;
            m_model->running = true;
            m_worker = std::jthread([this, cmd] { simulate_job(cmd); });
        }

        void Controller::simulate_job(const Command &cmd)
        {
            tkw::cli::Options opt = query_options(cmd);
            opt.players = cmd.options.players;
            opt.seed = cmd.options.seed;
            opt.ai = cmd.options.ai;
            opt.mode = cmd.options.mode;
            opt.hand = cmd.options.hand;

            m_log.push("开始模拟 " + std::to_string(cmd.games) + " 局（" +
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
                [this] { return m_cancel.load(); });
            for (auto &line : warnings)
                m_log.push(std::move(line));
            if (lines.is_err())
                m_log.push(lines.unwrap_err());
            else
                for (auto &line : lines.unwrap())
                    m_log.push(std::move(line));

            post_snapshot();
            post_done();
        }

        void Controller::do_status()
        {
            const UiSnapshot &snap = m_model->snapshot;
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

        tkw::cli::Options Controller::query_options(const Command &cmd) const
        {
            tkw::cli::Options opt = m_base;
            opt.humans.clear();
            if (m_session.active && m_session.game)
                opt.deck = m_session.deck;
            if (cmd.deck_provided)
                opt.deck = cmd.options.deck;
            return opt;
        }

        void Controller::append_query_lines(
            const tkw::cli::detail::QueryLines &lines)
        {
            if (lines.is_err())
            {
                append_line(lines.unwrap_err());
                return;
            }
            for (const auto &line : lines.unwrap())
                append_line(line);
        }

        void Controller::do_cards(const Command &cmd)
        {
            append_query_lines(tkw::cli::detail::cards_lines(
                query_options(cmd), cmd.with_text));
        }

        void Controller::do_rules(const Command &cmd)
        {
            append_query_lines(tkw::cli::detail::rules_lines(
                query_options(cmd), cmd.keyword));
        }

        void Controller::do_audit(const Command &cmd)
        {
            append_query_lines(
                tkw::cli::detail::audit_lines(query_options(cmd)));
        }

        void Controller::do_decks(const Command &cmd)
        {
            append_query_lines(
                tkw::cli::detail::decks_lines(query_options(cmd).deck));
        }

        void Controller::do_heroes(const Command &cmd)
        {
            append_query_lines(
                tkw::cli::detail::heroes_lines(query_options(cmd).deck));
        }

        void Controller::do_save(const std::string &file)
        {
            if (!m_session.active || !m_session.game)
            {
                append_line("没有进行中的对局（先运行 new 开局；help 查看用法）");
                return;
            }
            tkw::save::SessionMeta meta;
            meta.ai = detail::ai_level_name(m_session.ai);
            meta.stats = m_session.stats;
            const std::string text =
                tkw::save::write(*m_session.game, m_session.state, "deck", meta);
            const auto write = tkw::io::write_text_atomic(file, text);
            if (write.is_err())
            {
                append_line(tkw::cli::render_write_error_zh(
                    file, write.unwrap_err()));
                return;
            }
            append_line("已保存: " + file);
        }

        void Controller::do_load(const std::string &file)
        {
            auto text = tkw::io::read_text(file);
            if (text.is_err())
            {
                append_line(tkw::cli::render_read_error_zh(
                    file, text.unwrap_err()));
                return;
            }
            auto built = tkw::game::GameFactory::build(tkw::game::BuildOptions{
                m_base.deck, m_base.players, m_base.seed,
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
                tkw::cli::detail::validate_humans(*game, m_base.humans);
            if (!verr.empty())
            {
                append_line(verr);
                return;
            }

            tkw::cli::AiLevel ai = m_base.ai;
            tkw::cli::AiLevel saved = tkw::cli::AiLevel::Simple;
            if (!meta.ai.empty() && detail::ai_from(meta.ai, saved))
                ai = saved;

            m_log.unbind();
            m_stats_handles.clear();
            m_session.stats = std::move(meta.stats);
            m_log.bind(*game, m_base.humans);
            m_stats_handles =
                tkw::cli::detail::subscribe_stats(*game, m_session.stats);
            m_session.game = std::move(game);
            m_session.state = std::move(state);
            m_session.humans = m_base.humans;
            m_session.ai = ai;
            m_session.deck = m_base.deck;
            m_session.active = true;
            m_viewer = m_base.humans.empty() ? "P0" : m_base.humans.front();
            rebuild_decision_source(m_base.humans, ai);
            refresh_model();
            append_line("已加载: " + file);
        }

        void Controller::do_help(const Command &cmd)
        {
            for (const auto &line : detail::query_help_lines(cmd.keyword))
                append_line(line);
        }

        void Controller::autosave()
        {
            if (!m_session.active || !m_session.game || m_base.autosave.empty())
                return;
            tkw::save::SessionMeta meta;
            meta.ai = detail::ai_level_name(m_session.ai);
            meta.stats = m_session.stats;
            const std::string text =
                tkw::save::write(*m_session.game, m_session.state, "deck", meta);
            const std::string path = m_base.autosave.string();
            if (tkw::io::write_text_atomic(m_base.autosave, text).is_ok())
                m_exit_message = "已自动存档: " + path;
            else
                m_exit_message = "自动存档失败: " + path;
            append_line(m_exit_message);
        }
    }  // namespace tui
}  // namespace tkw
