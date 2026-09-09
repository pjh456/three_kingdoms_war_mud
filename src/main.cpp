/**
 * @file main.cpp
 * @brief CLI 入口（pjh_cli）：根命令跑一局，audit 审计牌堆，deal 位置参数跑局，
 *        repl 进入交互模式。
 */

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <pjh_cli.hpp>
#include <pjh_cli/console/help_navigator.hpp>
#include <pjh_cli/console/query_result.hpp>

#include "card/catalog.hpp"
#include "config/error.hpp"
#include "config/resource.hpp"
#include "entity/base.hpp"
#include "entity/event.hpp"
#include "entity/hp.hpp"
#include "event/handler.hpp"
#include "game/ai/human.hpp"
#include "game/ai/simple.hpp"
#include "game/resolve/audit.hpp"
#include "game/core/card_event.hpp"
#include "game/flow/loop.hpp"
#include "game/flow/table.hpp"
#include "io/file.hpp"
#include "save/reader.hpp"
#include "save/writer.hpp"
#include "util/rng.hpp"

namespace
{
    using pjh::cli::App;
    using pjh::cli::CliError;
    using pjh::cli::CliFailure;
    using pjh::cli::CliResult;
    using pjh::cli::ExtraArgsPolicy;
    using pjh::cli::fixed_string;
    using pjh::cli::InteractiveConsole;
    using pjh::cli::ParseContext;
    using pjh::cli::Visibility;

    struct Options
    {
        std::filesystem::path deck = "resources";
        int players = 4;
        int hand = tkw::game::RulesConfig{}.initial_hand;
        std::uint32_t seed = 42;
        bool verbose = false;
        std::filesystem::path autosave = "tkw-autosave.json";
        std::vector<std::string> humans; /**< 真人座位 id（可重复选项累积） */
    };

    /**
     * @brief 从解析上下文读参数；未出现的选项回落 base。
     * @note REPL 每行命令独立解析，根选项不会自动继承启动命令行的取值，
     *       故 REPL 内建局以启动选项为 base 合并，行内显式选项优先。
     */
    Options options_from(ParseContext &ctx, const Options &base)
    {
        Options opt = base;
        opt.deck = ctx.get_or<std::filesystem::path, fixed_string("deck")>(base.deck);
        opt.players = ctx.get_or<int, fixed_string("players")>(base.players);
        opt.hand = ctx.get_or<int, fixed_string("hand")>(base.hand);
        opt.seed = static_cast<std::uint32_t>(
            ctx.get_or<int, fixed_string("seed")>(static_cast<int>(base.seed)));
        opt.verbose = ctx.get_or<bool, fixed_string("verbose")>(base.verbose);
        opt.autosave = ctx.get_or<std::filesystem::path, fixed_string("autosave")>(
            base.autosave);
        if (ctx.has<fixed_string("human")>())
            opt.humans = ctx.get_all<std::string, fixed_string("human")>();
        return opt;
    }

    /** 从解析上下文读参数（无 base：一次性命令使用选项默认值）。 */
    Options options_from(ParseContext &ctx) { return options_from(ctx, Options{}); }

    /**
     * @brief 在命令上声明标量公共选项（牌堆/人数/手牌/种子/日志/自动存档）。
     * @param cmd   目标命令：根命令或会读取这些选项的 leaf。
     * @param rules 玩家数上下限来源。
     * @note pjh_cli 的选项声明只查当前命令、值才沿父链查找，故根命令与各 leaf
     *       需各声明一份，子命令名之后的选项才可解析；未显式给的项仍由
     *       options_from 沿父链或会话启动选项回落，声明处一律不设默认值。
     *       标量「最近声明胜出」即期望语义，故 per-leaf 重声明无副作用。
     */
    void declare_common_options(
        pjh::cli::BaseCommand &cmd, const tkw::game::RulesConfig &rules)
    {
        cmd.option<fixed_string("deck")>(
               "--deck", 'd', "资源目录（含 deck.json 与 cards/）")
            .path();
        cmd.option<fixed_string("players")>("--players", 'p', "玩家数")
            .integer()
            .min(rules.min_players)
            .max(rules.max_players);
        cmd.option<fixed_string("hand")>("--hand", "初始手牌数")
            .integer()
            .min(0)
            .max(20);
        cmd.option<fixed_string("seed")>("--seed", 's', "随机种子").integer().min(0);
        cmd.option<fixed_string("verbose")>("--verbose", 'v', "打印卡牌/死亡事件日志")
            .boolean();
        cmd.option<fixed_string("autosave")>(
               "--autosave", "REPL 退出时自动存档路径（空串关闭）")
            .path();
    }

    /**
     * @brief 在根命令上声明可重复的真人座位选项。
     * @param cmd 目标命令；只应传根命令，使父/叶混写累积进同一上下文。
     * @note repeatable 选项的值按「最近声明」写入单一上下文，若根与 leaf 各声明
     *       一份，`--human P0 new --human P1` 会分落两处，而读取只取最近节点，
     *       导致 P0 静默丢弃；故仅根声明。leaf 处仍可解析（祖先链查找）。
     */
    void declare_human_option(pjh::cli::BaseCommand &cmd)
    {
        cmd.option<fixed_string("human")>(
               "--human",
               "真人座位（可重复：--human P0 --human P2；存档不保存，读档后需重新指定）")
            .str()
            .repeatable();
    }

    /** 跨命令持有的对局会话（new/step/run/save/load 共享）。 */
    struct Session
    {
        std::unique_ptr<tkw::game::Game> game;
        tkw::game::GameSession state;
        std::vector<std::string> humans; /**< 本会话的真人座位 id */
        bool verbose = false;            /**< 本会话是否打印事件日志 */
        Options base;                    /**< REPL 启动选项（供行内命令继承） */
        bool active = false;
    };

    /** 性别占位：无玩家数据源，按座位奇偶交替（P0 男 / P1 女 / …）。 */
    tkw::entity::Gender gender_for_seat(int seat)
    {
        return seat % 2 == 0 ? tkw::entity::Gender::Male
                             : tkw::entity::Gender::Female;
    }

    /** 按选项构建一局（加载牌堆 + 建玩家）；失败返回 nullptr 并填 err。 */
    std::unique_ptr<tkw::game::Game> build_game(const Options &opt, std::string &err)
    {
        tkw::config::ResourceStore store(opt.deck);
        auto catalog = tkw::card::CardDefCatalog::load(store, "deck");
        if (catalog.is_err())
        {
            const auto &e = catalog.unwrap_err();
            err = "加载牌堆失败 (kind=" + std::to_string(static_cast<int>(e.kind)) +
                  "): " + e.detail;
            return nullptr;
        }
        auto game = std::make_unique<tkw::game::Game>(
            std::move(catalog).unwrap(), std::make_unique<tkw::SeededRng>(opt.seed));
        for (int i = 0; i < opt.players; ++i)
        {
            auto r = game->add_player(
                "P" + std::to_string(i), i,
                tkw::entity::Hp::make(game->rules.base_hp),
                gender_for_seat(i));
            if (r.is_err())
            {
                err = "创建玩家失败: P" + std::to_string(i);
                return nullptr;
            }
        }
        return game;
    }

    /**
     * @brief 订阅本局事件日志：verbose 为真时打印摸牌/打牌/弃牌/阵亡。
     * @return 订阅句柄；verbose 为假时为空，句柄析构即退订。
     * @note 句柄只应活在需要日志的命令作用域内，不得存入 Session：会话被覆盖
     *       时会先析构旧 Game（含总线），遗留句柄将对已释放总线退订。
     */
    std::vector<tkw::EventBus::Handle> subscribe_event_log(
        tkw::game::Game &game, bool verbose)
    {
        std::vector<tkw::EventBus::Handle> handles;
        if (!verbose)
            return handles;
        handles.push_back(game.bus.subscribe(tkw::Handler<tkw::CardPlayedEvent>(
            [](tkw::HandlerContext<tkw::CardPlayedEvent> &c)
            { std::cout << "[打出] " << c.event.user << " " << c.event.def_id << "\n"; })));
        handles.push_back(
            game.bus.subscribe(tkw::Handler<tkw::CardDiscardedEvent>(
                [](tkw::HandlerContext<tkw::CardDiscardedEvent> &c) {
                    std::cout << "[弃置] " << c.event.entity << " " << c.event.def_id
                              << "\n";
                })));
        handles.push_back(game.bus.subscribe(tkw::Handler<tkw::CardDrawnEvent>(
            [](tkw::HandlerContext<tkw::CardDrawnEvent> &c)
            { std::cout << "[摸牌] " << c.event.entity << " " << c.event.def_id << "\n"; })));
        handles.push_back(game.bus.subscribe(tkw::Handler<tkw::EntityDiedEvent>(
            [](tkw::HandlerContext<tkw::EntityDiedEvent> &c)
            { std::cout << "[阵亡] " << c.event.entity_id << "\n"; })));
        return handles;
    }

    /** 校验真人座位：必须是对局中存在的实体且互不重复；空串表示通过。 */
    std::string validate_humans(
        tkw::game::Game &game, const std::vector<std::string> &humans)
    {
        auto ctx = game.context();
        for (std::size_t i = 0; i < humans.size(); ++i)
        {
            if (ctx.entities->find(humans[i]).is_none())
                return "真人座位不存在: " + humans[i];
            for (std::size_t j = i + 1; j < humans.size(); ++j)
                if (humans[i] == humans[j])
                    return "真人座位重复: " + humans[i];
        }
        return {};
    }

    /** 构造决策源：无真人走 SimpleAI，否则按 actor 路由到交互输入。 */
    std::unique_ptr<tkw::game::DecisionSource> make_decision_source(
        const std::vector<std::string> &humans)
    {
        if (humans.empty())
            return std::make_unique<tkw::game::SimpleAI>();
        return std::make_unique<tkw::game::ai::RoutedAI>(
            humans, std::cin, std::cout);
    }

    void print_status(const Session &s)
    {
        std::cout << "会话: " << (s.active ? "进行中" : "无") << "\n";
        if (!s.active || !s.game)
            return;
        auto ctx = s.game->context();
        std::cout << "  下一回合: " << s.state.current
                  << "，已执行回合: " << s.state.turns
                  << "，存活: " << ctx.entities->size() << "\n";
        std::cout << "  真人座位: ";
        if (s.humans.empty())
            std::cout << "无";
        else
            for (std::size_t i = 0; i < s.humans.size(); ++i)
                std::cout << (i == 0 ? "" : ",") << s.humans[i];
        std::cout << "\n";
        std::cout << "  局面:\n";
        for (const auto &e : *ctx.entities)
            std::cout << "    " << e->get_id() << " 体力 " << e->get_hp() << "/"
                      << e->get_hp_bar().get_max() << " 手牌 "
                      << ctx.cards->hand_size(e->get_id()) << " 装备 "
                      << ctx.cards->equip_size(e->get_id()) << " 判定 "
                      << ctx.cards->judge_size(e->get_id()) << "\n";
    }

    CliResult<void> cmd_new(const Options &opt, Session &s)
    {
        std::string err;
        auto game = build_game(opt, err);
        if (!game)
            return CliFailure{CliError(err)};
        const std::string verr = validate_humans(*game, opt.humans);
        if (!verr.empty())
            return CliFailure{CliError(verr)};
        auto log = subscribe_event_log(*game, opt.verbose);
        auto ctx = game->context();
        tkw::game::GameSession state;
        if (tkw::game::start_session(ctx, state, "P0", opt.hand).is_err())
            return CliFailure{CliError("开局失败")};
        s.game = std::move(game);
        s.state = std::move(state);
        s.humans = opt.humans;
        s.verbose = opt.verbose;
        s.active = true;
        std::cout << "新对局已开始\n";
        print_status(s);
        return CliResult<void>::Ok();
    }

    CliResult<void> cmd_step(Session &s)
    {
        if (!s.active || !s.game)
            return CliFailure{CliError("没有进行中的对局")};
        auto log = subscribe_event_log(*s.game, s.verbose);
        auto ai = make_decision_source(s.humans);
        auto ctx = s.game->context();
        if (tkw::game::session_over(ctx))
        {
            std::cout << "对局已结束，胜者: " << tkw::game::session_winner(ctx) << "\n";
            return CliResult<void>::Ok();
        }
        auto r = tkw::game::step_session(ctx, *ai, s.state);
        if (r.is_err())
        {
            if (r.unwrap_err() == tkw::game::LoopError::MaxRounds)
            {
                std::cout << "平局（达到最大回合数）\n";
                return CliResult<void>::Ok();
            }
            return CliFailure{CliError("回合执行失败")};
        }
        print_status(s);
        if (tkw::game::session_over(ctx))
            std::cout << "对局结束，胜者: " << tkw::game::session_winner(ctx) << "\n";
        return CliResult<void>::Ok();
    }

    CliResult<void> cmd_run(Session &s)
    {
        if (!s.active || !s.game)
            return CliFailure{CliError("没有进行中的对局")};
        auto log = subscribe_event_log(*s.game, s.verbose);
        auto ai = make_decision_source(s.humans);
        auto ctx = s.game->context();
        while (!tkw::game::session_over(ctx))
        {
            auto r = tkw::game::step_session(ctx, *ai, s.state);
            if (r.is_err())
            {
                if (r.unwrap_err() == tkw::game::LoopError::MaxRounds)
                {
                    std::cout << "平局（达到最大回合数）\n";
                    return CliResult<void>::Ok();
                }
                return CliFailure{CliError("回合执行失败")};
            }
        }
        std::cout << "胜者: " << tkw::game::session_winner(ctx)
                  << "，回合数: " << s.state.turns << "\n";
        return CliResult<void>::Ok();
    }

    CliResult<void> cmd_save(const std::filesystem::path &file, Session &s)
    {
        if (!s.active || !s.game)
            return CliFailure{CliError("没有进行中的对局")};
        const std::string text = tkw::save::write(*s.game, s.state, "deck");
        if (tkw::io::write_text_atomic(file, text).is_err())
            return CliFailure{CliError("写入存档失败: " + file.string())};
        std::cout << "已保存: " << file.string() << "\n";
        return CliResult<void>::Ok();
    }

    CliResult<void> cmd_load(
        const Options &opt, const std::filesystem::path &file, Session &s)
    {
        auto text = tkw::io::read_text(file);
        if (text.is_err())
            return CliFailure{CliError("读取存档失败: " + file.string())};
        std::string err;
        auto game = build_game(opt, err);
        if (!game)
            return CliFailure{CliError(err)};
        tkw::game::GameSession state;
        auto r = tkw::save::read(text.unwrap(), *game, state);
        if (r.is_err())
        {
            const auto &e = r.unwrap_err();
            return CliFailure{CliError(
                "存档加载失败 (kind=" + std::to_string(static_cast<int>(e.kind)) +
                "): " + e.detail)};
        }
        const std::string verr = validate_humans(*game, opt.humans);
        if (!verr.empty())
            return CliFailure{CliError(verr)};
        s.game = std::move(game);
        s.state = std::move(state);
        s.humans = opt.humans;
        s.verbose = opt.verbose;
        s.active = true;
        std::cout << "已加载: " << file.string() << "\n";
        print_status(s);
        return CliResult<void>::Ok();
    }

    CliResult<void> run_game(const Options &opt)
    {
        std::string err;
        auto game = build_game(opt, err);
        if (!game)
            return CliFailure{CliError(err)};

        const auto unsupported = tkw::game::unsupported_cards(game->catalog);
        if (!unsupported.empty())
        {
            std::cerr << "警告: 牌堆含 " << unsupported.size()
                      << " 张引擎未实现的卡:";
            for (const auto &id : unsupported)
                std::cerr << ' ' << id;
            std::cerr << "\n";
        }

        const std::string verr = validate_humans(*game, opt.humans);
        if (!verr.empty())
            return CliFailure{CliError(verr)};

        auto ctx = game->context();
        auto log = subscribe_event_log(*game, opt.verbose);

        auto ai = make_decision_source(opt.humans);
        auto outcome = tkw::game::play_game(ctx, *ai, "P0", opt.hand);
        if (outcome.is_err())
        {
            if (outcome.unwrap_err() == tkw::game::LoopError::MaxRounds)
            {
                std::cout << "平局（达到最大回合数）\n";
                return CliResult<void>::Ok();
            }
            return CliFailure{CliError(
                "对局失败 (LoopError=" +
                std::to_string(static_cast<int>(outcome.unwrap_err())) + ")")};
        }

        const auto &out = outcome.unwrap();
        std::cout << "胜者: " << out.winner << "，回合数: " << out.turns << "\n";
        return CliResult<void>::Ok();
    }

    CliResult<void> audit_deck(const Options &opt)
    {
        std::string err;
        auto game = build_game(opt, err);
        if (!game)
            return CliFailure{CliError(err)};

        const auto unsupported = tkw::game::unsupported_cards(game->catalog);
        if (unsupported.empty())
        {
            std::cout << "牌堆全部可结算\n";
            return CliResult<void>::Ok();
        }
        std::cout << "未实现卡（" << unsupported.size() << " 张）:\n";
        for (const auto &id : unsupported)
            std::cout << "  " << id << "\n";
        return CliResult<void>::Ok();
    }

    /**
     * @brief 执行错误的用户可见文本。
     * @param what 执行错误 what() 文本。
     * @return 剥掉框架统一加的 "Parse Error: " 前缀；无前缀时原样返回。
     * @note 只用于执行路径：这些错误语义上不是解析失败，保留前缀会把「没有对局/
     *       文件不存在」误示为命令行拼写错误。解析路径的错误须保留前缀。
     */
    std::string_view runtime_error_text(std::string_view what)
    {
        constexpr std::string_view prefix = "Parse Error: ";
        return what.starts_with(prefix) ? what.substr(prefix.size()) : what;
    }

    /** 把 "Usage: " 前缀换成中文，其余原样（帮助与 REPL 无匹配提示共用）。 */
    std::string zh_usage_prefix(std::string text)
    {
        constexpr std::string_view prefix = "Usage: ";
        if (text.starts_with(prefix))
            text.replace(0, prefix.size(), "用法: ");
        return text;
    }

    /** 把帮助正文中独占一行的英文段标题替换为中文（只替换首个匹配）。 */
    void replace_heading_line(
        std::string &text, std::string_view from, std::string_view to)
    {
        const std::string needle = "\n" + std::string(from) + ":\n";
        const std::string replacement = "\n" + std::string(to) + ":\n";
        if (std::size_t pos = text.find(needle); pos != std::string::npos)
            text.replace(pos, needle.size(), replacement);
    }

    /**
     * @brief 把命令树的框架帮助数据渲染为中文帮助。
     * @param cmd 请求帮助的命令（根或任一子命令）。
     * @return 中文段标题（用法/选项/参数/子命令）的完整帮助；根命令额外附用法示例。
     * @note 只替换框架渲染结果的段标题与 usage 前缀，选项/参数/子命令的排布与
     *       对齐仍由框架负责，避免自造排版。段标题在渲染后再替换，使框架仍按
     *       英文段名选择列宽上限（选项段 32 字节）。选项标注（如 (repeatable)）
     *       保持框架原文。program_name 用完整命令路径，子命令帮助也带 tkw 前缀。
     *       根帮助示例按「批量一次性」与「REPL 会话」分组：会话流命令只能在
     *       `tkw repl` 内逐条输入，不带 tkw 前缀。
     */
    std::string render_help_zh(const pjh::cli::BaseCommand &cmd)
    {
        std::vector<std::string_view> parts;
        for (const pjh::cli::BaseCommand *c = &cmd; c != nullptr; c = c->parent())
            if (!c->name().empty())
                parts.push_back(c->name());
        std::string path;
        for (auto it = parts.rbegin(); it != parts.rend(); ++it)
        {
            if (!path.empty())
                path += ' ';
            path += *it;
        }

        pjh::cli::HelpInfo info = pjh::cli::HelpFormatter::collect_help(cmd, path);
        pjh::cli::HelpDocument doc = pjh::cli::HelpFormatter::build_document(info);

        std::string text = zh_usage_prefix(pjh::cli::HelpFormatter::format_help(doc));
        replace_heading_line(text, "Options", "选项");
        replace_heading_line(text, "Arguments", "参数");
        replace_heading_line(text, "Subcommands", "子命令");

        if (cmd.parent() == nullptr)
            text += "示例:\n"
                    "  批量一次性:\n"
                    "    tkw                      跑一局 AI 对局\n"
                    "    tkw deal 2 1             按位置参数跑一局（2 人，种子 1）\n"
                    "    tkw audit                审计牌堆\n"
                    "  REPL 会话（先 tkw repl，再逐条输入）:\n"
                    "    new --players 2 --seed 1 开新局\n"
                    "    step                     执行一个回合\n"
                    "    status                   查看会话状态\n"
                    "    save s.json              保存当前对局\n"
                    "    load s.json              载入存档到会话，再 step 继续\n"
                    "  真人参与（先 tkw --human P0 repl，再逐条输入）:\n"
                    "    new --players 2 --seed 1 开新局\n"
                    "    step                     轮到 P0 时按提示输入（play/pass/discard）\n";
        return text;
    }

    /** 按命令名列表渲染「命令名 + 描述」两列（描述取命令树，缺失留空）。 */
    std::string command_lines_zh(
        const pjh::cli::BranchCommand &root,
        const std::vector<std::string> &names)
    {
        std::size_t width = 0;
        for (const auto &n : names)
            if (n.size() > width)
                width = n.size();

        std::string out;
        for (const auto &n : names)
        {
            out += "  " + n;
            out.append(width - n.size(), ' ');
            out += "  ";
            const pjh::cli::BaseCommand *sub = root.find_subcommand(n);
            if (sub != nullptr)
                out += std::string(sub->description());
            out += "\n";
        }
        return out;
    }

    /**
     * @brief REPL `?` 查询结果的中文渲染。
     * @param root   根命令，用于按名查子命令描述。
     * @param result 框架查询结果（列表/匹配/模糊/无匹配）。
     * @return 中文提示 + 命令名与描述两列；无匹配时附中文用法行。
     * @note 只消费框架结构，不重复实现匹配逻辑；描述直接取命令树，保证与
     *       --help 子命令表同源。
     */
    std::string render_query_zh(
        const pjh::cli::BranchCommand &root, const pjh::cli::QueryResult &result)
    {
        using pjh::cli::QueryKind;
        switch (result.kind)
        {
        case QueryKind::Listing:
            return "命令（? <关键词> 过滤，help <命令> 看用法）:\n" +
                   command_lines_zh(root, result.names);
        case QueryKind::Matched:
            return "匹配命令:\n" + command_lines_zh(root, result.names);
        case QueryKind::Fuzzy:
        {
            std::string out = "您是否要找:";
            for (const auto &m : result.suggestions.matches)
                out += " " + m.name;
            return out + "\n";
        }
        case QueryKind::NoMatch:
            // 调用方统一在结果末尾补一个换行，这里不自带换行以免多出空行。
            return "没有匹配的命令。" + zh_usage_prefix(result.usage_line);
        }
        return {};
    }

    /**
     * @brief REPL `help [命令]` 的中文渲染。
     * @param result 框架导航结果。
     * @return 根/子命令帮助走同一中文渲染；叶命令与未知命令用中文提示。
     * @note 帮助正文复用 render_help_zh，保证 REPL `help` 与批量 `--help` 同格式。
     */
    std::string render_help_nav_zh(const pjh::cli::HelpNavigationResult &result)
    {
        using pjh::cli::HelpNavigationKind;
        switch (result.kind)
        {
        case HelpNavigationKind::RootHelp:
        case HelpNavigationKind::SubcommandHelp:
            return render_help_zh(*result.resolved);
        case HelpNavigationKind::NonBranch:
            return "'" + result.failed_command_name + "' 没有子命令。\n";
        case HelpNavigationKind::UnknownCommand:
        {
            std::string out = "未知命令 '" + result.failed_token + "'。";
            if (!result.suggestions.matches.empty())
            {
                out += " 您是否要找:";
                for (const auto &m : result.suggestions.matches)
                    out += " " + m.name;
            }
            return out + "\n";
        }
        }
        return {};
    }
}

int main(int argc, char **argv)
{
    App app("tkw", "0.1.0", "三国杀式卡牌对局引擎");
    app.set_extra_args(ExtraArgsPolicy::Error);  // 未知命令/多余参数即报错
    app.set_help_formatter(
        [](const pjh::cli::BaseCommand &cmd) { return render_help_zh(cmd); });

    const tkw::game::RulesConfig rules{};
    Session session;  // 跨命令持有的对局会话

    // 根命令选项（无子命令时直接跑一局，兼容旧用法）；读取这些选项的 leaf 各自
    // 声明一份标量选项，使子命令名之后的选项也可解析；--human 是 repeatable，
    // 仅根声明以避免父/叶混写时值分落两处。
    declare_common_options(app, rules);
    declare_human_option(app);

    // 根命令：无子命令直接跑一局（兼容旧用法），先打印一行引导，说明其余入口。
    app.action(
        [](ParseContext &ctx) -> CliResult<void>
        {
            std::cout
                << "（无子命令：跑一局 AI 对局；--help 查看命令，repl 进入交互，"
                   "--human P0 真人参与）\n";
            return run_game(options_from(ctx));
        });

    // audit：审计牌堆
    auto &audit = app.add_leaf("audit", "审计牌堆，列出引擎未实现的卡");
    declare_common_options(audit, rules);
    audit.action([](ParseContext &ctx) -> CliResult<void>
                 { return audit_deck(options_from(ctx)); });

    // deal：位置参数跑局（REPL/批量通用）
    auto &deal = app.add_leaf("deal", "跑一局：deal <玩家数> <种子>");
    declare_common_options(deal, rules);
    deal.arg<int, 0>("players", "玩家数").required();
    deal.arg<int, 1>("seed", "随机种子").required();
    deal.action(
        [&rules](ParseContext &ctx) -> CliResult<void>
        {
            Options opt = options_from(ctx);
            opt.players = ctx.get<int, 0>();
            opt.seed = static_cast<std::uint32_t>(ctx.get<int, 1>());
            if (opt.players < rules.min_players || opt.players > rules.max_players)
                return CliFailure{CliError("玩家数超出允许范围")};
            return run_game(opt);
        });

    // new：开新对局（不立即跑），供 step/run/save 续用
    auto &new_cmd = app.add_leaf("new", "开新对局（用 --players/--seed/--hand）");
    declare_common_options(new_cmd, rules);
    new_cmd.action(
        [&session](ParseContext &ctx) -> CliResult<void>
        { return cmd_new(options_from(ctx, session.base), session); });

    // step：执行一个回合
    auto &step_cmd = app.add_leaf("step", "执行当前会话的一个回合");
    step_cmd.action(
        [&session](ParseContext &) -> CliResult<void> { return cmd_step(session); });

    // run：跑到对局结束
    auto &run_cmd = app.add_leaf("run", "跑到当前会话结束");
    run_cmd.action(
        [&session](ParseContext &) -> CliResult<void> { return cmd_run(session); });

    // status：查看会话状态
    auto &status_cmd = app.add_leaf("status", "查看当前会话状态");
    status_cmd.action(
        [&session](ParseContext &) -> CliResult<void>
        {
            print_status(session);
            return CliResult<void>::Ok();
        });

    // save：保存当前对局
    auto &save_cmd = app.add_leaf("save", "保存当前对局：save <file>");
    save_cmd.arg<std::string, 0>("file", "存档路径").required();
    save_cmd.action(
        [&session](ParseContext &ctx) -> CliResult<void>
        { return cmd_save(ctx.get<std::string, 0>(), session); });

    // load：从存档继续
    auto &load_cmd = app.add_leaf("load", "加载存档：load <file>");
    declare_common_options(load_cmd, rules);
    load_cmd.arg<std::string, 0>("file", "存档路径").required();
    load_cmd.action(
        [&session](ParseContext &ctx) -> CliResult<void>
        {
            return cmd_load(
                options_from(ctx, session.base), ctx.get<std::string, 0>(),
                session);
        });

    // repl：交互模式（对局即 MUD 方向）
    auto &repl = app.add_leaf("repl", "进入交互模式（? 查看命令，quit 退出）");
    declare_common_options(repl, rules);
    repl.set_visibility(Visibility::Cli);
    repl.action(
        [&app, &session](ParseContext &ctx) -> CliResult<void>
        {
            session.base = options_from(ctx);
            // 进入 REPL 前打印引导：命令列表与退出方式在提示符处不可见。
            std::cout << "输入 ? 查看命令，help <命令> 看用法，quit 退出\n";
            InteractiveConsole console(
                app, "tkw> ", std::cin, std::cout, std::cerr,
                [&app](const pjh::cli::QueryResult &r)
                { return render_query_zh(app, r); },
                [](const pjh::cli::HelpNavigationResult &r)
                { return render_help_nav_zh(r); });
            console.run();
            const Options &opt = session.base;
            if (session.active && session.game && !opt.autosave.empty())
            {
                const std::string text =
                    tkw::save::write(*session.game, session.state, "deck");
                if (tkw::io::write_text_atomic(opt.autosave, text).is_ok())
                    std::cout << "已自动存档: " << opt.autosave.string() << "\n";
            }
            return CliResult<void>::Ok();
        });

    // 批量入口也启用模糊匹配：唯一近距匹配自动纠错，多候选报歧义。
    auto parsed = app.parse_fuzzy(argc, argv);
    if (parsed.is_err())
    {
        std::cerr << parsed.unwrap_err().what() << "\n";
        return 2;
    }

    auto &ctx = parsed.unwrap();
    if (ctx.help_requested())
    {
        std::cout << ctx.help_text();
        return 0;
    }
    if (ctx.version_requested())
    {
        std::cout << ctx.version_text();
        return 0;
    }

    auto executed = ctx.matched_command()->execute(ctx);
    if (executed.is_err())
    {
        std::cerr << runtime_error_text(executed.unwrap_err().what()) << "\n";
        return 1;
    }
    return 0;
}
