#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <pjh_cli.hpp>

#include "cli/commands.hpp"
#include "cli/error_zh.hpp"
#include "cli/help_zh.hpp"
#include "cli/query_lines.hpp"
#include "cli/render.hpp"
#include "config/error.hpp"
#include "game/core/roles.hpp"
#include "game/core/state.hpp"
#include "game/flow/loop.hpp"
#include "io/file.hpp"

namespace
{
    /** 单条 REPL 命令的执行结果：成功标志、错误文本、项目输出、框架输出。 */
    struct RunResult
    {
        bool ok = false;
        std::string error;    /**< CliError::what()，含框架 "Parse Error: " 前缀 */
        std::string out;      /**< 命令 action 写 std::cout 的内容 */
        std::string console;  /**< 框架侧（?/help）写注入 output 的内容 */
    };

    /**
     * @brief 可注入流的 REPL 夹具：同一命令树 + 独立 Session，逐行驱动 process_line。
     * @note 输入流仅占位；process_line 不读输入，故无需真终端。deck 指向测试资源，
     *       避免依赖进程工作目录。
     */
    struct Repl
    {
        pjh::cli::App app;
        tkw::cli::Session session;
        std::istringstream in;
        std::ostringstream out;
        std::ostringstream err;
        std::ostringstream captured_cout;
        pjh::cli::InteractiveConsole console;

        Repl() :
            app("tkw", "0.1.0", "三国杀式卡牌对局引擎"),
            console(
                app, "> ", in, out, err,
                [this](const pjh::cli::QueryResult &r)
                { return tkw::cli::render_query_zh(app, r); },
                [](const pjh::cli::HelpNavigationResult &r)
                { return tkw::cli::render_help_nav_zh(r); })
        {
            tkw::cli::build_app(app, session);
            session.base.deck = TKW_TEST_RESOURCE_DIR;
        }

        RunResult run(const std::string &line)
        {
            out.str("");
            out.clear();
            err.str("");
            err.clear();
            captured_cout.str("");
            captured_cout.clear();

            std::streambuf *old = std::cout.rdbuf(captured_cout.rdbuf());
            auto r = console.process_line(line);
            std::cout.rdbuf(old);

            RunResult res;
            res.ok = r.is_ok();
            if (r.is_err())
                res.error = r.unwrap_err().what();
            res.out = captured_cout.str();
            res.console = out.str();
            return res;
        }
    };

    std::filesystem::path temp_save(const std::string &name)
    {
        return std::filesystem::temp_directory_path() / name;
    }

    std::filesystem::path temp_dir(const std::string &name)
    {
        return std::filesystem::temp_directory_path() / name;
    }

    /** 子串出现次数：用于断言角色标签的可见/占位数量。 */
    std::size_t count_substr(const std::string &hay, const std::string &needle)
    {
        std::size_t n = 0;
        for (std::size_t p = hay.find(needle); p != std::string::npos;
             p = hay.find(needle, p + needle.size()))
            ++n;
        return n;
    }

    /**
     * @brief 写一份单卡 mini 牌表（每卡 30 张），供不依赖 cwd 的牌表来源断言。
     * @param dir       牌表目录；不存在则创建，已存在先清空。
     * @param deck_name deck.json 的 name 字段。
     * @param card_name 卡的显示名（用于区分不同牌表）。
     * @return 目录与两个 JSON 文件全部写出成功为真。
     */
    bool write_mini_deck(const std::filesystem::path &dir,
                         const std::string &deck_name,
                         const std::string &card_name)
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        if (!std::filesystem::create_directories(dir / "cards"))
            return false;
        std::string copies;
        for (int i = 0; i < 30; ++i)
        {
            if (i != 0)
                copies += ',';
            copies += R"({"suit":"spade","number":)" +
                      std::to_string(i % 13 + 1) + "}";
        }
        if (tkw::io::write_text(
                dir / "deck.json",
                R"({"name":")" + deck_name + R"(","cards":["h0"]})")
                .is_err())
            return false;
        return tkw::io::write_text(
                   dir / "cards" / "h0.json",
                   R"({"id":"h0","name":")" + card_name +
                       R"(","type":"basic","copies":[)" + copies + "]}")
            .is_ok();
    }

    /**
     * @brief 写一份含未知机制名的 mini 牌表，供 audit 未实现卡分支断言。
     * @param dir 牌表目录；不存在则创建，已存在先清空。
     * @return deck.json 与 cards/ghost.json 均写出成功为真。
     */
    bool write_unknown_mechanism_deck(const std::filesystem::path &dir)
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        if (!std::filesystem::create_directories(dir / "cards"))
            return false;
        if (tkw::io::write_text(dir / "deck.json",
                                R"({"name":"ghost","cards":["ghost"]})")
                .is_err())
            return false;
        return tkw::io::write_text(
                   dir / "cards" / "ghost.json",
                   R"({"id":"ghost","name":"幽魂","type":"basic",)"
                   R"("effect":{"kind":"summon","amount":1,"scope":"one_other"}})")
            .is_ok();
    }

    /** 逐行拼接：每行补行尾 '\n'，与 CLI wrapper 的打印契约一致；空向量得空串。 */
    std::string join_lines(const std::vector<std::string> &lines)
    {
        std::string out;
        for (const auto &line : lines)
            out += line + "\n";
        return out;
    }

    /**
     * @brief 执行 f 期间捕获 std::cout 的字节输出，返回捕获内容。
     * @note RAII 恢复原缓冲：即使 f 抛出（doctest REQUIRE）也不污染后续用例。
     */
    template <typename F>
    std::string capture_cout(F &&f)
    {
        std::ostringstream captured;
        struct Restore
        {
            std::streambuf *old;
            ~Restore() { std::cout.rdbuf(old); }
        } restore{std::cout.rdbuf(captured.rdbuf())};
        f();
        return captured.str();
    }
}

TEST_CASE("cli: new/step/status/run advance the session")
{
    Repl repl;

    auto created = repl.run("new --players 2 --seed 1");
    CHECK(created.ok);
    CHECK(created.error.empty());
    CHECK(repl.session.active);
    REQUIRE(repl.session.game != nullptr);
    CHECK(repl.session.humans.empty());
    CHECK(repl.session.state.turns == 0);
    CHECK(created.out.find("新对局已开始") != std::string::npos);
    CHECK(created.out.find("会话: 进行中") != std::string::npos);

    auto stepped = repl.run("step");
    CHECK(stepped.ok);
    CHECK(repl.session.state.turns == 1);

    auto status = repl.run("status");
    CHECK(status.ok);
    CHECK(status.out.find("下一回合") != std::string::npos);
    CHECK(status.out.find("真人座位: 无") != std::string::npos);
    CHECK(status.out.find("P0 体力 4/4 手牌 4 装备") != std::string::npos);
    // 标准牌表无铁索连环，横置恒 false：局面段不得出现状态标记。
    CHECK(status.out.find("[横置]") == std::string::npos);

    auto ran = repl.run("run");
    CHECK(ran.ok);
    CHECK(repl.session.state.turns >= 2);
}

TEST_CASE("cli: status shows human own hand but keeps opponents as counts")
{
    Repl repl;
    REQUIRE(repl.run("new --players 2 --seed 1 --human P0").ok);

    auto status = repl.run("status");
    REQUIRE(status.ok);
    CHECK(status.out.find("真人座位: P0") != std::string::npos);
    CHECK(status.out.find("P0 体力 4/4 手牌 无懈可击/五谷丰登/无中生有/闪 装备 无 "
                          "判定 无") != std::string::npos);
    CHECK(status.out.find("P1 体力 4/4 手牌 4 装备 无 判定 无") != std::string::npos);
    CHECK(status.out.find("P1 体力 4/4 手牌 无懈") == std::string::npos);
}

TEST_CASE("cli: status expands public equip and judge zone names")
{
    Repl repl;
    REQUIRE(repl.run("new --players 2 --seed 1").ok);

    // 装备区/判定区为明置信息：任何座位均展开牌名，空区回落「无」。
    auto ctx = repl.session.game->context();
    ctx.cards->add_to_equip(
        "P1", tkw::card::Card{"e#0", "bagua", tkw::card::Suit::Spade, 2});
    ctx.cards->add_to_judge(
        "P0", tkw::card::Card{"j#0", "lesi", tkw::card::Suit::Heart, 6});

    auto status = repl.run("status");
    REQUIRE(status.ok);
    CHECK(status.out.find("P0 体力 4/4 手牌 4 装备 无 判定 乐不思蜀") !=
          std::string::npos);
    CHECK(status.out.find("P1 体力 4/4 手牌 4 装备 八卦阵 判定 无") !=
          std::string::npos);
}

TEST_CASE("cli: status marks chained seats")
{
    Repl repl;
    REQUIRE(repl.run("new --players 2 --seed 1").ok);
    auto ctx = repl.session.game->context();
    tkw::game::set_chained(ctx, "P1", true);

    auto status = repl.run("status");
    REQUIRE(status.ok);
    CHECK(status.out.find("P1 体力 4/4 手牌 4 装备 无 判定 无 [横置]") !=
          std::string::npos);
    CHECK(status.out.find("P0 体力 4/4 手牌 4 装备 无 判定 无 [横置]") ==
          std::string::npos);
    CHECK(count_substr(status.out, "[横置]") == 1);
}

TEST_CASE("cli: status marks chained seat despite hidden role")
{
    Repl repl;
    REQUIRE(repl.run("new --mode identity --players 4 --seed 1 --human P0").ok);
    auto ctx = repl.session.game->context();
    tkw::game::set_chained(ctx, "P2", true);

    auto status = repl.run("status");
    REQUIRE(status.ok);
    // 横置独立于身份可见性收敛：隐藏角色座位仍显示状态标记。
    CHECK(status.out.find("P2 体力 4/4 手牌 4 装备 无 判定 无 角色 未知 [横置]") !=
          std::string::npos);
}

TEST_CASE("cli: status shows each human seat own hand")
{
    Repl repl;
    REQUIRE(repl.run("new --players 2 --seed 1 --human P0 --human P1").ok);

    auto status = repl.run("status");
    REQUIRE(status.ok);
    CHECK(status.out.find("P0 体力 4/4 手牌 无懈可击") != std::string::npos);
    CHECK(status.out.find("P1 体力 4/4 手牌 4") == std::string::npos);
}

TEST_CASE("cli: repl history option wires FileHistory and warns on missing dir")
{
    std::ostringstream err;

    // 空路径：交回框架默认（内存），不产生告警。
    CHECK(tkw::cli::detail::make_repl_history("", err) == nullptr);
    CHECK(err.str().empty());

    // 可写路径：构造 FileHistory，连续重复折叠，析构落盘。
    const std::filesystem::path file = temp_save("tkw-cli-history.txt");
    std::error_code ec;
    std::filesystem::remove(file, ec);
    {
        auto hist = tkw::cli::detail::make_repl_history(file, err);
        REQUIRE(hist != nullptr);
        hist->push("alpha");
        hist->push("alpha");
        CHECK(hist->size() == 1);
    }
    CHECK(err.str().empty());
    CHECK(tkw::io::exists(file));
    std::filesystem::remove(file, ec);

    // 父目录缺失：显式告警并回落内存。
    std::ostringstream missing;
    CHECK(tkw::cli::detail::make_repl_history("/nonexistent-dir/x", missing) ==
          nullptr);
    CHECK(missing.str().find("命令历史目录不存在") != std::string::npos);
}

TEST_CASE("cli: inline --verbose enables event log for that command")
{
    Repl repl;

    REQUIRE(repl.run("new --players 2 --seed 1").ok);

    auto loud = repl.run("step --verbose");
    CHECK(loud.ok);
    CHECK(loud.out.find("[摸牌]") != std::string::npos);

    // 行内 verbose 只作用于该次命令，不改变会话默认。
    auto quiet = repl.run("step");
    CHECK(quiet.ok);
    CHECK(quiet.out.find("[摸牌]") == std::string::npos);

    auto ran = repl.run("run --verbose");
    CHECK(ran.ok);
    CHECK(ran.out.find("[摸牌]") != std::string::npos);
}

TEST_CASE("cli: --no-verbose turns off a session-inherited event log")
{
    Repl repl;

    // 建局命令显式开日志，会话默认记住。
    REQUIRE(repl.run("new --players 2 --seed 1 --verbose").ok);
    CHECK(repl.session.verbose);

    auto loud = repl.run("step");
    CHECK(loud.ok);
    CHECK(loud.out.find("[打出]") != std::string::npos);

    // 行内 --no-verbose 只关本次命令的日志，不改写会话默认。
    auto quiet = repl.run("step --no-verbose");
    CHECK(quiet.ok);
    CHECK(quiet.out.find("[") == std::string::npos);
    CHECK(repl.session.verbose);

    // 下一行未显式提供时仍回落会话默认（开）。
    auto again = repl.run("step");
    CHECK(again.ok);
    CHECK(again.out.find("[") != std::string::npos);
}

TEST_CASE("cli: human session defaults event log on")
{
    Repl repl;

    // 真人座位存在且未显式提供 verbose：建局默认开启事件日志。
    REQUIRE(repl.run("--human P0 new --players 2 --seed 1").ok);
    CHECK(repl.session.verbose);
    // 真人 step 会进入决策窗口；无输入时以回合失败收场，但摸牌事件已可见。
    auto stepped = repl.run("step");
    CHECK(stepped.out.find("[摸牌]") != std::string::npos);
    // 失败回合被消费：推进到下一角色并计入回合数，重入不再重跑 P0。
    CHECK(repl.session.state.current == "P1");
    CHECK(repl.session.state.turns == 1);

    // 启动 --no-verbose 是显式选择，能关闭真人默认。
    Repl quiet;
    REQUIRE(quiet.run("--no-verbose --human P0 new --players 2 --seed 1").ok);
    CHECK_FALSE(quiet.session.verbose);

    // 全 AI 局默认保持静默，不产生事件日志。
    Repl ai;
    REQUIRE(ai.run("new --players 2 --seed 1").ok);
    CHECK_FALSE(ai.session.verbose);
}

TEST_CASE("cli: human new logs initial deal by default")
{
    // 真人默认：建局自身的初始发牌也走事件日志，己方显牌名、对手占位未知牌。
    Repl repl;
    auto created = repl.run("--human P0 new --players 2 --seed 1");
    REQUIRE(created.ok);
    CHECK(created.out.find("[摸牌] P0 ") != std::string::npos);
    CHECK(created.out.find("[摸牌] P1 未知牌") != std::string::npos);
    CHECK(repl.session.verbose);

    // 显式 --no-verbose 优先，建局仍静默。
    Repl quiet;
    auto q = quiet.run("--no-verbose --human P0 new --players 2 --seed 1");
    REQUIRE(q.ok);
    CHECK(q.out.find("[摸牌]") == std::string::npos);
    CHECK_FALSE(quiet.session.verbose);

    // 全 AI 建局保持静默，不因日志口径统一而漂移。
    Repl ai;
    auto a = ai.run("new --players 2 --seed 1");
    REQUIRE(a.ok);
    CHECK(a.out.find("[摸牌]") == std::string::npos);
}

TEST_CASE("cli: verbose step and run print turn headers")
{
    Repl repl;

    REQUIRE(repl.run("new --players 2 --seed 1 --verbose").ok);
    auto stepped = repl.run("step");
    CHECK(stepped.out.find("—— 回合 1：P0 ——") != std::string::npos);
    auto ran = repl.run("run --verbose");
    CHECK(ran.out.find("—— 回合 2：P1 ——") != std::string::npos);

    // 默认静默路径（全 AI、未开 verbose）不打印回合头。
    Repl quiet;
    REQUIRE(quiet.run("new --players 2 --seed 1").ok);
    auto q = quiet.run("step");
    CHECK(q.out.find("—— 回合") == std::string::npos);
}

TEST_CASE("cli: one-shot run defaults event log for human seats")
{
    // 一次性跑局（deal/裸 tkw）与建局同口径；真人 EOF 可能以回合失败收场，
    // 但回合头在此之前已打印，故只断言展示门控、不检查成败。
    const auto captured_run = [](tkw::cli::Options opt)
    {
        opt.deck = TKW_TEST_RESOURCE_DIR;
        opt.players = 2;
        opt.seed = 1;

        std::istringstream in;
        std::ostringstream captured;
        std::streambuf *old_in = std::cin.rdbuf(in.rdbuf());
        std::streambuf *old_out = std::cout.rdbuf(captured.rdbuf());
        std::cin.clear();
        const auto result = tkw::cli::detail::run_game(opt);
        (void)result;
        std::cout.rdbuf(old_out);
        std::cin.rdbuf(old_in);
        return captured.str();
    };

    tkw::cli::Options human;
    human.humans = {"P0"};
    CHECK(captured_run(human).find("—— 回合 1：P0 ——") != std::string::npos);

    // 显式 --no-verbose 仍优先，压过真人默认。
    tkw::cli::Options quiet;
    quiet.humans = {"P0"};
    quiet.verbose_explicit = true;
    quiet.verbose = false;
    CHECK(captured_run(quiet).find("—— 回合") == std::string::npos);

    // 全 AI 一次性跑局保持静默。
    CHECK(captured_run(tkw::cli::Options{}).find("—— 回合") == std::string::npos);
}

TEST_CASE("cli: discard event label reflects judge and response semantics")
{
    Repl repl;
    REQUIRE(repl.run("new --players 2 --seed 1").ok);

    auto handles = tkw::cli::detail::subscribe_event_log(*repl.session.game, true);
    const auto publish = [&](tkw::DiscardKind kind)
    {
        auto ev = std::make_shared<tkw::CardDiscardedEvent>();
        ev->entity = "P1";
        ev->instance_id = "x";
        ev->def_id = "sha";
        ev->kind = kind;
        repl.session.game->bus.publish(ev);
    };

    std::streambuf *old = std::cout.rdbuf(repl.captured_cout.rdbuf());
    publish(tkw::DiscardKind::Judgement);
    publish(tkw::DiscardKind::Response);
    publish(tkw::DiscardKind::Normal);
    std::cout.rdbuf(old);

    const std::string out = repl.captured_cout.str();
    CHECK(out.find("[判定] P1 杀") != std::string::npos);
    CHECK(out.find("[打出] P1 杀") != std::string::npos);
    CHECK(out.find("[弃置] P1 杀") != std::string::npos);
}

TEST_CASE("cli: draw event label reflects kill reward semantics")
{
    Repl repl;
    REQUIRE(repl.run("new --players 2 --seed 1").ok);

    auto handles = tkw::cli::detail::subscribe_event_log(*repl.session.game, true);
    const auto publish = [&](tkw::DrawKind kind)
    {
        auto ev = std::make_shared<tkw::CardDrawnEvent>();
        ev->entity = "P1";
        ev->instance_id = "x";
        ev->def_id = "sha";
        ev->kind = kind;
        repl.session.game->bus.publish(ev);
    };

    std::streambuf *old = std::cout.rdbuf(repl.captured_cout.rdbuf());
    publish(tkw::DrawKind::KillReward);
    publish(tkw::DrawKind::Normal);
    std::cout.rdbuf(old);

    const std::string out = repl.captured_cout.str();
    CHECK(out.find("[击杀奖励] P1 杀") != std::string::npos);
    CHECK(out.find("[摸牌] P1 杀") != std::string::npos);
}

TEST_CASE("cli: event log hides drawn card names of non-human seats")
{
    Repl repl;
    REQUIRE(repl.run("new --players 2 --seed 1").ok);

    auto handles = tkw::cli::detail::subscribe_event_log(
        *repl.session.game, true, std::vector<std::string>{"P0"});
    const auto publish = [&](const std::string &entity)
    {
        auto ev = std::make_shared<tkw::CardDrawnEvent>();
        ev->entity = entity;
        ev->instance_id = "x";
        ev->def_id = "sha";
        repl.session.game->bus.publish(ev);
    };

    std::streambuf *old = std::cout.rdbuf(repl.captured_cout.rdbuf());
    publish("P1");
    publish("P0");
    std::cout.rdbuf(old);

    const std::string out = repl.captured_cout.str();
    CHECK(out.find("[摸牌] P1 未知牌") != std::string::npos);
    CHECK(out.find("[摸牌] P1 杀") == std::string::npos);
    CHECK(out.find("[摸牌] P0 杀") != std::string::npos);
}

TEST_CASE("cli: verbose log renders card moved events")
{
    Repl repl;

    REQUIRE(repl.run("new --players 2 --seed 1").ok);

    auto handles = tkw::cli::detail::subscribe_event_log(*repl.session.game, true);

    auto ev = std::make_shared<tkw::CardMovedEvent>();
    ev->from_entity = "P0";
    ev->to_entity = "P1";
    ev->instance_id = "x";
    ev->def_id = "sha";
    ev->from = tkw::Zone::Hand;
    ev->to = tkw::Zone::Judge;
    std::streambuf *old = std::cout.rdbuf(repl.captured_cout.rdbuf());
    repl.session.game->bus.publish(ev);
    std::cout.rdbuf(old);
    CHECK(repl.captured_cout.str().find("[移牌] P0(手牌) -> P1(判定区) 杀") !=
          std::string::npos);

    // 亮牌等非玩家来源：空实体渲染 (无)，Limbo 显示临时区。
    repl.captured_cout.str("");
    repl.captured_cout.clear();
    auto reveal = std::make_shared<tkw::CardMovedEvent>();
    reveal->from_entity = "";
    reveal->to_entity = "P0";
    reveal->instance_id = "y";
    reveal->def_id = "sha";
    reveal->from = tkw::Zone::Limbo;
    reveal->to = tkw::Zone::Hand;
    old = std::cout.rdbuf(repl.captured_cout.rdbuf());
    repl.session.game->bus.publish(reveal);
    std::cout.rdbuf(old);
    CHECK(repl.captured_cout.str().find("[移牌] (无)(临时区) -> P0(手牌) 杀") !=
          std::string::npos);
}

TEST_CASE("cli: repeated --human accumulates across positions")
{
    Repl repl;

    auto mixed = repl.run("--human P0 new --human P1 --players 2 --seed 1");
    CHECK(mixed.ok);
    REQUIRE(repl.session.humans.size() == 2);
    CHECK(repl.session.humans[0] == "P0");
    CHECK(repl.session.humans[1] == "P1");

    auto after = repl.run("new --human P0 --human P1 --players 2 --seed 1");
    CHECK(after.ok);
    REQUIRE(repl.session.humans.size() == 2);
    CHECK(repl.session.humans[0] == "P0");
    CHECK(repl.session.humans[1] == "P1");
}

TEST_CASE("cli: session commands report missing session")
{
    Repl repl;

    auto step = repl.run("step");
    CHECK_FALSE(step.ok);
    CHECK(step.error == "没有进行中的对局（先运行 new 开局，或进入 tkw repl）");

    auto run = repl.run("run");
    CHECK_FALSE(run.ok);
    CHECK(run.error == "没有进行中的对局（先运行 new 开局，或进入 tkw repl）");

    auto save = repl.run("save /tmp/tkw-cli-missing-session.json");
    CHECK_FALSE(save.ok);
    CHECK(save.error == "没有进行中的对局（先运行 new 开局，或进入 tkw repl）");

    auto status = repl.run("status");
    CHECK(status.ok);
    CHECK(status.out.find("会话: 无") != std::string::npos);

    auto load = repl.run("load /tmp/tkw-cli-definitely-missing.json");
    CHECK_FALSE(load.ok);
    CHECK(load.error.find("读取存档失败") != std::string::npos);
    // 标签与路径都要暴露给用户，便于直接排错。
    CHECK(load.error.find("文件不存在") != std::string::npos);
    CHECK(load.error.find("/tmp/tkw-cli-definitely-missing.json") !=
          std::string::npos);
}

TEST_CASE("cli: save then load restores the session")
{
    Repl repl;
    const std::filesystem::path file = temp_save("tkw-cli-roundtrip.json");
    std::error_code ec;
    std::filesystem::remove(file, ec);

    REQUIRE(repl.run("new --players 2 --seed 1").ok);
    REQUIRE(repl.run("step").ok);
    const std::string saved_current = repl.session.state.current;
    const int saved_turns = repl.session.state.turns;

    auto saved = repl.run("save " + file.string());
    CHECK(saved.ok);
    CHECK(std::filesystem::exists(file));

    REQUIRE(repl.run("new --players 4 --seed 9").ok);
    CHECK(repl.session.game->entities.size() == 4);

    auto loaded = repl.run("load " + file.string());
    CHECK(loaded.ok);
    CHECK(repl.session.active);
    CHECK(repl.session.state.current == saved_current);
    CHECK(repl.session.state.turns == saved_turns);
    CHECK(repl.session.game->entities.size() == 2);

    std::filesystem::remove(file, ec);
}

TEST_CASE("cli: repl query and help render commands")
{
    Repl repl;

    auto listing = repl.run("?");
    CHECK(listing.ok);
    CHECK(listing.console.find("命令") != std::string::npos);
    CHECK(listing.console.find("new") != std::string::npos);
    CHECK(listing.console.find("audit") != std::string::npos);
    CHECK(listing.console.find("repl") == std::string::npos);
    // 新注册的批量命令自动进 ? 列表，无硬编码同步面。
    CHECK(listing.console.find("simulate") != std::string::npos);

    // ? sim 子串唯一命中 simulate（Matched 列表渲染），不走模糊建议。
    auto sim_filter = repl.run("? sim");
    CHECK(sim_filter.ok);
    CHECK(sim_filter.console.find("匹配命令") != std::string::npos);
    CHECK(sim_filter.console.find("simulate") != std::string::npos);
    CHECK(sim_filter.console.find("您是否要找") == std::string::npos);

    // 带空格关键词走子串命中列表渲染（Matched），不是模糊建议：查询串
    // 若残留前导空格，会退化到「您是否要找」路径，该断言方向即红。
    auto filtered = repl.run("? aud");
    CHECK(filtered.ok);
    CHECK(filtered.console.find("匹配命令") != std::string::npos);
    CHECK(filtered.console.find("audit") != std::string::npos);
    CHECK(filtered.console.find("您是否要找") == std::string::npos);

    auto help = repl.run("help new");
    CHECK(help.ok);
    CHECK(help.console.find("用法:") != std::string::npos);

    auto unknown = repl.run("help boguszz");
    CHECK(unknown.ok);
    CHECK(unknown.console.find("未知命令") != std::string::npos);

    // REPL 内 `new --help` 走 App 注入的 help_formatter，与批量 --help 同一
    // 中文渲染路径：钉中文章节标题齐全，且不残留英文段标题。
    auto inline_help = repl.run("new --help");
    CHECK(inline_help.ok);
    CHECK(inline_help.console.find("开新对局") != std::string::npos);
    CHECK(inline_help.console.find("用法:") != std::string::npos);
    CHECK(inline_help.console.find("选项:") != std::string::npos);
    CHECK(inline_help.console.find("公共选项:") != std::string::npos);
    CHECK(inline_help.console.find("Usage:") == std::string::npos);
    CHECK(inline_help.console.find("Inherited Options") == std::string::npos);
}

TEST_CASE("cli: high-frequency leaf help appends usage examples")
{
    Repl repl;

    // 高频 leaf 的 --help 末尾附「示例:」段，每段含可辨识的示例行；示例须与
    // 当前选项面/位置参数一致，改声明时同步改这里。
    struct LeafExample
    {
        const char *cmd;
        const char *snippet;
    };
    const LeafExample cases[] = {
        {"new", "tkw new --players 2 --seed 1"},
        {"deal", "tkw --ai aggressive deal 2 1"},
        {"load", "tkw load s.json --ai aggressive"},
        {"save", "保存当前对局（别名 w）"},
        {"run", "跑到对局结束（别名 r）"},
        {"step", "执行一个回合（可重复）"},
        {"simulate", "tkw --ai aggressive simulate 100 2"},
        {"cards", "tkw --deck resources cards"},
        {"audit", "tkw --deck resources audit"},
    };
    for (const auto &c : cases)
    {
        auto help = repl.run(std::string(c.cmd) + " --help");
        CHECK(help.ok);
        CHECK(help.console.find("示例:") != std::string::npos);
        CHECK(help.console.find(c.snippet) != std::string::npos);
    }

    // 非高频 leaf 不附示例段，避免帮助冗长；REPL 的 help <命令> 与批量 --help
    // 走同一渲染，示例同样可见。
    auto repl_help = repl.run("repl --help");
    CHECK(repl_help.ok);
    CHECK(repl_help.console.find("示例:") == std::string::npos);

    auto nav_help = repl.run("help deal");
    CHECK(nav_help.ok);
    CHECK(nav_help.console.find("示例:") != std::string::npos);
    CHECK(nav_help.console.find("tkw deal 2 1") != std::string::npos);
}

TEST_CASE("cli: aliases dispatch to canonical commands")
{
    Repl repl;

    // 无会话时 st 精确命中 status（而非模糊歧义）。
    auto st_empty = repl.run("st");
    CHECK(st_empty.ok);
    CHECK(st_empty.out.find("会话: 无") != std::string::npos);

    REQUIRE(repl.run("new --players 2 --seed 1").ok);
    auto st = repl.run("st");
    CHECK(st.ok);
    CHECK(st.out.find("会话: 进行中") != std::string::npos);

    // r 精确命中 run，跑到对局结束（seed 1 2p 确定性胜者）。
    auto r = repl.run("r");
    CHECK(r.ok);
    CHECK(r.out.find("胜者") != std::string::npos);

    // ? 列表以括号展示别名（REPL 内唯一浏览面，保可发现性）。
    auto listing = repl.run("?");
    CHECK(listing.console.find("status (st)") != std::string::npos);
    CHECK(listing.console.find("run (r)") != std::string::npos);
    CHECK(listing.console.find("save (w)") != std::string::npos);
    CHECK(listing.console.find("load (l)") != std::string::npos);

    // ? 过滤按子串命中：st 应同时列出 status 与 step（Matched 列表渲染，
    // 别名 status (st) 也经子串命中而非模糊建议）。
    auto filtered = repl.run("? st");
    CHECK(filtered.console.find("匹配命令") != std::string::npos);
    CHECK(filtered.console.find("status") != std::string::npos);
    CHECK(filtered.console.find("step") != std::string::npos);
    CHECK(filtered.console.find("您是否要找") == std::string::npos);
}

TEST_CASE("cli: unknown command and bad options are errors")
{
    Repl repl;

    auto unknown = repl.run("zzzzzzzz");
    CHECK_FALSE(unknown.ok);

    auto bad_option = repl.run("new --bogus");
    CHECK_FALSE(bad_option.ok);

    auto out_of_range = repl.run("new --players 99 --seed 1");
    CHECK_FALSE(out_of_range.ok);

    auto bad_value = repl.run("new --players abc");
    CHECK_FALSE(bad_value.ok);
}

TEST_CASE("cli: new accepts the eight-player cap and rejects one above it")
{
    Repl repl;

    // 上界含 8：满座开局成功，实体数如实为 8。
    auto created = repl.run("new --players 8 --seed 1");
    REQUIRE(created.ok);
    REQUIRE(repl.session.game != nullptr);
    CHECK(repl.session.game->entities.size() == 8);

    // 上界之上拒绝（与既有 --players 99 互补，钉住 8 是合法上界）。
    auto over = repl.run("new --players 9 --seed 1");
    CHECK_FALSE(over.ok);
}

TEST_CASE("cli: invalid human seats are rejected")
{
    Repl repl;

    auto missing = repl.run("new --human P9 --players 2 --seed 1");
    CHECK_FALSE(missing.ok);
    CHECK(missing.error.find("真人座位不存在") != std::string::npos);
    CHECK(missing.error.find("可用座位") != std::string::npos);
    CHECK(missing.error.find("P0") != std::string::npos);

    auto duplicate = repl.run("new --human P0 --human P0 --players 2 --seed 1");
    CHECK_FALSE(duplicate.ok);
    CHECK(duplicate.error.find("真人座位重复") != std::string::npos);
    CHECK(duplicate.error.find("每个座位只能指定一次") != std::string::npos);
}

TEST_CASE("cli: --human completion offers seat ids")
{
    Repl repl;

    // 空前缀：候选为规则允许的全部座位号；带 P 前缀仍全量命中；越界前缀为空。
    auto all = pjh::cli::complete_line_result(repl.app, "--human ", 8);
    REQUIRE(all.candidates.size() == 8);
    std::vector<std::string> got;
    for (const auto &c : all.candidates)
        got.push_back(c.display);
    CHECK(std::find(got.begin(), got.end(), "P0") != got.end());
    CHECK(std::find(got.begin(), got.end(), "P7") != got.end());

    auto prefixed = pjh::cli::complete_line_result(repl.app, "--human P", 9);
    CHECK(prefixed.candidates.size() == 8);

    // leaf 位置经祖先链同样命中（--human 仅根声明，new 继承查找）。
    auto leaf = pjh::cli::complete_line_result(repl.app, "new --human P", 13);
    CHECK(leaf.candidates.size() == 8);

    auto none = pjh::cli::complete_line_result(repl.app, "--human Q", 9);
    CHECK(none.candidates.empty());
}

TEST_CASE("cli: --ai completion offers level names")
{
    Repl repl;

    // 空前缀：两个档位全量；s/a 前缀单命中；越界前缀为空。
    auto all = pjh::cli::complete_line_result(repl.app, "--ai ", 5);
    REQUIRE(all.candidates.size() == 2);
    std::vector<std::string> got;
    for (const auto &c : all.candidates)
        got.push_back(c.display);
    CHECK(std::find(got.begin(), got.end(), "simple") != got.end());
    CHECK(std::find(got.begin(), got.end(), "aggressive") != got.end());

    auto s = pjh::cli::complete_line_result(repl.app, "--ai s", 6);
    REQUIRE(s.candidates.size() == 1);
    CHECK(s.candidates[0].display == "simple");

    // leaf 位置经祖先链同样命中（--ai 每个 leaf 都声明）。
    auto leaf = pjh::cli::complete_line_result(repl.app, "new --ai ", 9);
    CHECK(leaf.candidates.size() == 2);

    auto none = pjh::cli::complete_line_result(repl.app, "--ai x", 6);
    CHECK(none.candidates.empty());
}

TEST_CASE("cli: --mode completion offers mode names")
{
    Repl repl;

    // 空前缀：两个模式全量；i 前缀单命中；越界前缀为空。
    auto all = pjh::cli::complete_line_result(repl.app, "--mode ", 7);
    REQUIRE(all.candidates.size() == 2);
    std::vector<std::string> got;
    for (const auto &c : all.candidates)
        got.push_back(c.display);
    CHECK(std::find(got.begin(), got.end(), "brawl") != got.end());
    CHECK(std::find(got.begin(), got.end(), "identity") != got.end());

    auto identity = pjh::cli::complete_line_result(repl.app, "--mode i", 8);
    REQUIRE(identity.candidates.size() == 1);
    CHECK(identity.candidates[0].display == "identity");

    // leaf 位置经祖先链同样命中（--mode 每个 leaf 都声明）。
    auto leaf = pjh::cli::complete_line_result(repl.app, "new --mode ", 11);
    CHECK(leaf.candidates.size() == 2);

    auto none = pjh::cli::complete_line_result(repl.app, "--mode x", 8);
    CHECK(none.candidates.empty());
}

TEST_CASE("cli: audit entry name renders chinese name with id")
{
    tkw::config::ResourceStore store(TKW_TEST_RESOURCE_DIR);
    auto r = tkw::card::CardDefCatalog::load(store, "deck");
    REQUIRE(r.is_ok());
    auto cat = std::move(r).unwrap();
    CHECK(tkw::cli::detail::audit_entry_name(cat, "cixiong") ==
          "雌雄双股剑(cixiong)");
    CHECK(tkw::cli::detail::audit_entry_name(cat, "nope") == "nope(nope)");
}

TEST_CASE("cli: rules lists and filters card effect text")
{
    Repl repl;
    const std::string deck = TKW_TEST_RESOURCE_DIR;

    auto all = repl.run("rules --deck " + deck);
    CHECK(all.ok);
    CHECK(all.out.find("牌表: " + deck) != std::string::npos);
    CHECK(all.out.find("卡牌说明（") != std::string::npos);
    CHECK(all.out.find("杀(sha): ") != std::string::npos);

    auto filtered = repl.run("rules 过河拆桥 --deck " + deck);
    CHECK(filtered.ok);
    CHECK(filtered.out.find("过河拆桥(guohe): 出牌阶段") != std::string::npos);
    CHECK(filtered.out.find("弃置其区域内的一张牌") != std::string::npos);
    CHECK(filtered.out.find("杀(sha):") == std::string::npos);

    auto none = repl.run("rules 不存在的牌zzz --deck " + deck);
    CHECK(none.ok);
    CHECK(none.out.find("没有匹配的卡牌说明") != std::string::npos);
}

TEST_CASE("cli: cards --text appends the effect text")
{
    Repl repl;
    const std::string deck = TKW_TEST_RESOURCE_DIR;

    auto plain = repl.run("cards --deck " + deck);
    REQUIRE(plain.ok);
    CHECK(plain.out.find("杀(sha) 基本 30") != std::string::npos);
    CHECK(plain.out.find("目标需打出一张「闪」") == std::string::npos);

    auto with_text = repl.run("cards --text --deck " + deck);
    REQUIRE(with_text.ok);
    CHECK(with_text.out.find("杀(sha) 基本 30: 出牌阶段限一次") !=
          std::string::npos);
}

TEST_CASE("cli: unsupported-card warning stays silent for a supported deck")
{
    // 建局/批量入口共用的告警口径：标准牌表全部可结算时不误报。
    tkw::config::ResourceStore store(TKW_TEST_RESOURCE_DIR);
    auto catalog = tkw::card::CardDefCatalog::load(store, "deck");
    REQUIRE(catalog.is_ok());

    std::ostringstream err;
    tkw::cli::detail::warn_unsupported_cards(catalog.unwrap(), err);
    CHECK(err.str().empty());
}

TEST_CASE("cli: new with --ai aggressive runs to the end in the session")
{
    Repl repl;

    auto created = repl.run("new --players 2 --seed 1 --ai aggressive");
    CHECK(created.ok);
    CHECK(repl.session.ai == tkw::cli::AiLevel::Aggressive);

    // 会话难度档被 step/run 消费：确定性种子跑完（胜者或回合上限平局）
    auto ran = repl.run("run");
    CHECK(ran.ok);
    const bool has_outcome = ran.out.find("胜者") != std::string::npos ||
                             ran.out.find("平局") != std::string::npos;
    CHECK(has_outcome);
}

TEST_CASE("cli: --ai rejects an unmapped value")
{
    Repl repl;

    auto bad = repl.run("new --ai bogus");
    CHECK_FALSE(bad.ok);
    CHECK(bad.error.find("Parse Error") != std::string::npos);
}

TEST_CASE("cli: identity status shows mode and roles")
{
    Repl repl;

    auto created = repl.run("new --mode identity --players 5 --seed 1");
    CHECK(created.ok);
    REQUIRE(repl.session.game != nullptr);
    CHECK(repl.session.game->mode == tkw::game::GameMode::Identity);
    CHECK(created.out.find("模式: 身份局") != std::string::npos);
    // 5 人配比覆盖全部四种角色标签。
    CHECK(created.out.find("角色 主公") != std::string::npos);
    CHECK(created.out.find("角色 忠臣") != std::string::npos);
    CHECK(created.out.find("角色 反贼") != std::string::npos);
    CHECK(created.out.find("角色 内奸") != std::string::npos);
}

TEST_CASE("cli: identity human status hides non-lord non-self roles")
{
    Repl repl;
    // 真人为 P1（非主公）：P0 主公与 P1 自身可见，其余两座应收敛为「未知」。
    REQUIRE(repl.run("new --mode identity --players 4 --seed 1 --human P1").ok);

    auto status = repl.run("status");
    REQUIRE(status.ok);
    CHECK(status.out.find("模式: 身份局") != std::string::npos);
    CHECK(count_substr(status.out, "角色 ") == 4);      // 每座一个角色字段
    CHECK(count_substr(status.out, "角色 未知") == 2);  // P2/P3 隐藏
    CHECK(count_substr(status.out, "角色 主公") == 1);  // P0 主公公开
    // P1 自身角色可见：4 人配比下恰为忠臣/反贼/内奸之一
    CHECK(count_substr(status.out, "角色 忠臣") +
              count_substr(status.out, "角色 反贼") +
              count_substr(status.out, "角色 内奸") ==
          1);
    CHECK(status.out.find("P1 体力 4/4 手牌 4 装备 无 判定 无 角色 未知") ==
          std::string::npos);
}

TEST_CASE("cli: identity human status reveals roles once the game is over")
{
    Repl repl;
    REQUIRE(repl.run("new --mode identity --players 4 --seed 1 --human P1").ok);
    auto ctx = repl.session.game->context();
    tkw::game::declare_death(ctx, "P0");
    REQUIRE(tkw::game::session_over(ctx));

    auto status = repl.run("status");
    REQUIRE(status.ok);
    CHECK(status.out.find("角色 未知") == std::string::npos);
}

TEST_CASE("cli: identity new rejects too few players")
{
    Repl repl;

    auto created = repl.run("new --mode identity --players 2");
    CHECK_FALSE(created.ok);
    CHECK(created.error.find("身份模式人数须为 4–8 人（当前 2）") !=
          std::string::npos);
    CHECK(created.error.find("请用 --players 4") != std::string::npos);
    CHECK_FALSE(repl.session.active);
}

TEST_CASE("cli: brawl status has no mode or role line")
{
    Repl repl;

    REQUIRE(repl.run("new --players 2 --seed 1").ok);
    auto status = repl.run("status");
    CHECK(status.ok);
    // 乱斗输出逐字节不变：模式行与逐座角色均只在 identity 分支产出。
    CHECK(status.out.find("模式") == std::string::npos);
    CHECK(status.out.find("角色") == std::string::npos);
}

TEST_CASE("cli: brawl zero-survivor stats keep the raw empty winner")
{
    Repl repl;

    REQUIRE(repl.run("new --players 2 --seed 1").ok);
    auto ctx = repl.session.game->context();
    tkw::game::declare_death(ctx, "P0");
    tkw::game::declare_death(ctx, "P1");

    // 终局行回落同归于尽标签；统计块必须保留乱斗原始空胜者（print_battle_stats
    // 显示「胜者: 无」），二者拆分正是为此，不可合并为一个标签。
    CHECK(tkw::cli::detail::game_end_label(ctx) == "平局（同归于尽）");
    CHECK(tkw::cli::detail::game_stats_label(ctx).empty());
}

TEST_CASE("cli: unsupported cards warning text is empty when no cards")
{
    // 空目录回落 id 作展示名；空清单返回空串，非空返回单行纯文本。
    CHECK(tkw::cli::detail::unsupported_cards_warning_text(
              tkw::card::CardDefCatalog{}, {})
              .empty());
    CHECK(tkw::cli::detail::unsupported_cards_warning_text(
              tkw::card::CardDefCatalog{}, {"foo", "bar"}) ==
          "警告: 牌堆含 2 张引擎未实现的卡: foo(foo) bar(bar)");
}

TEST_CASE("cli: parse errors render in Chinese")
{
    using pjh::cli::ErrorFactory;

    // 解析错误统一带中文前缀，正文不再泄漏框架英文关键词。
    const auto check_parse = [](const pjh::cli::CliError &err,
                                const std::string &needle)
    {
        const std::string text = tkw::cli::render_error_zh(err);
        CHECK(text.starts_with("参数错误: "));
        CHECK(text.find(needle) != std::string::npos);
        CHECK(text.find("unknown option") == std::string::npos);
        CHECK(text.find("unknown command") == std::string::npos);
        CHECK(text.find("ambiguous command") == std::string::npos);
        CHECK(text.find("out of range") == std::string::npos);
        CHECK(text.find("invalid value") == std::string::npos);
    };

    check_parse(ErrorFactory::unknown_option("--bogus"), "未知选项");
    check_parse(ErrorFactory::unknown_option("--bogus", {"--verbose"}),
                "您是否要找");
    check_parse(ErrorFactory::unknown_command("zzz", {}), "未知命令");
    check_parse(ErrorFactory::unknown_command("zzz", {"new"}), "您是否要找");
    check_parse(ErrorFactory::ambiguous_command("card", {"cards", "simulate"}),
                "有歧义");
    check_parse(ErrorFactory::value_out_of_range("--players", "99", 2, 8),
                "超出范围");
    check_parse(
        ErrorFactory::enum_value_error("--ai", "bogus", {"simple", "aggressive"}),
        "期望以下之一");
    check_parse(ErrorFactory::missing_value("--players"), "需要一个值");
    check_parse(ErrorFactory::missing_required_arg("file"), "缺少必需参数");
    check_parse(ErrorFactory::type_conversion_error("--players", "abc", "integer"),
                "整数");
    check_parse(ErrorFactory::no_command_matched(), "没有匹配的命令");

    // RawMessage 负载（Parse 类别）同样按 tag 分派：带中文前缀 + 原消息。
    const pjh::cli::CliError raw{
        pjh::cli::ErrorInfo{pjh::cli::RawMessageError{"自定义解析失败"}}};
    CHECK(tkw::cli::render_error_zh(raw) == "参数错误: 自定义解析失败");

    // 运行时错误逐字返回消息，不加任何前缀（退出码契约不变）。
    const auto runtime = ErrorFactory::runtime_error("没有进行中的对局");
    CHECK(tkw::cli::render_error_zh(runtime) == "没有进行中的对局");
}

TEST_CASE("cli: all-dead session reports the mutual destruction draw label")
{
    Repl repl;

    REQUIRE(repl.run("new --players 2 --seed 1").ok);

    // 标准牌堆的同回合伤害路径（杀/全场/决斗/闪电）总留下一名存活者，
    // 完整对局打不出全员阵亡；用引擎死亡路径构造等效终态：
    // 0 存活、回合数远低于上限。
    auto ctx = repl.session.game->context();
    tkw::game::declare_death(ctx, "P0");
    tkw::game::declare_death(ctx, "P1");
    CHECK(tkw::game::session_over(ctx));
    CHECK(tkw::game::session_winner(ctx).empty());

    // 对已结束会话 step：胜者行回落平局标签，不留空串
    auto stepped = repl.run("step");
    CHECK(stepped.ok);
    CHECK(stepped.out.find("对局已结束，胜者: 平局（同归于尽）") !=
          std::string::npos);

    // 对已结束会话 run：同一回落（会话不重复执行），回合数如实
    auto ran = repl.run("run");
    CHECK(ran.ok);
    CHECK(ran.out.find("胜者: 平局（同归于尽），回合数: 0") != std::string::npos);
    CHECK(ran.out.find("胜者: ，") == std::string::npos);
}

TEST_CASE("cli: audit/cards/simulate reject --human")
{
    Repl repl;

    // 三命令都不运行真人参与的对局：给出 --human 即硬拒绝（纯中文错误），
    // 并给出可复制的「直接运行 tkw <cmd>」替代出口。
    const std::vector<std::pair<std::string, std::string>> reject_cases = {
        {"audit", "audit --human P0"},
        {"cards", "cards --human P0"},
        {"decks", "decks --human P0"},
        {"rules", "rules --human P0"},
        {"simulate", "simulate 1 --human P0"}};
    for (const auto &c : reject_cases)
    {
        auto r = repl.run(c.second);
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("不支持 --human") != std::string::npos);
        CHECK(r.error.find("请直接运行 tkw " + c.first) != std::string::npos);
    }

    // 不带 --human 时行为不变（只读/批量命令继承启动牌表，显式 --deck 仍覆盖）
    const std::string deck = TKW_TEST_RESOURCE_DIR;
    CHECK(repl.run("audit --deck " + deck).ok);
    CHECK(repl.run("cards --deck " + deck).ok);
    CHECK(repl.run("decks --deck " + deck).ok);
    CHECK(repl.run("simulate 1 2 --deck " + deck).ok);
}

TEST_CASE("cli: repl read-only commands inherit startup deck")
{
    Repl repl;

    // 自建 mini 牌表：目录名区别于内置 resources，排除 cwd 恰为源码根时命中的假绿。
    const std::filesystem::path dir = temp_dir("tkw_cli_inherit_deck");
    const std::filesystem::path missing = temp_dir("tkw_cli_inherit_missing");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove_all(missing, ec);
    REQUIRE(std::filesystem::create_directories(dir / "cards"));
    REQUIRE(tkw::io::write_text(
                dir / "deck.json", R"({"name":"inherit","cards":["h0"]})")
                .is_ok());
    REQUIRE(tkw::io::write_text(
                dir / "cards" / "h0.json",
                R"({"id":"h0","name":"继承测","type":"basic",)"
                R"("copies":[{"suit":"spade","number":7}]})")
                .is_ok());

    // 启动牌表被 cards/rules/audit 继承（改动前三者回落 resources）。
    repl.session.base.deck = dir;
    auto cards = repl.run("cards");
    CHECK(cards.ok);
    CHECK(cards.out.find("牌表: " + dir.string()) != std::string::npos);
    CHECK(cards.out.find("继承测(h0)") != std::string::npos);

    auto rules = repl.run("rules");
    CHECK(rules.ok);
    CHECK(rules.out.find("牌表: " + dir.string()) != std::string::npos);

    auto audit = repl.run("audit");
    CHECK(audit.ok);
    CHECK(audit.out.find("牌表: " + dir.string()) != std::string::npos);

    // 行内 --deck 仍优先于启动选项。
    auto overridden =
        repl.run("cards --deck " + std::string(TKW_TEST_RESOURCE_DIR));
    CHECK(overridden.ok);
    CHECK(overridden.out.find("牌表: " + std::string(TKW_TEST_RESOURCE_DIR)) !=
          std::string::npos);
    CHECK(overridden.out.find("标准版") != std::string::npos);

    // 启动 --human 的座位不被只读命令继承（否则被 reject_humans 误拒）；
    // 行内显式 --human 仍须被拒绝。
    repl.session.base.humans = {"P0"};
    CHECK(repl.run("cards").ok);
    CHECK(repl.run("rules").ok);
    CHECK(repl.run("audit").ok);
    auto explicit_human = repl.run("cards --human P0");
    CHECK_FALSE(explicit_human.ok);
    CHECK(explicit_human.error.find("不支持 --human") != std::string::npos);

    // deal/simulate 同样继承启动牌表：指向不存在的目录廉价钉住红→绿——改动前两者
    // 回落 resources 会成功跑局，改动后按 base 目录加载失败。
    repl.session.base.humans.clear();
    repl.session.base.deck = missing;
    auto deal = repl.run("deal 2 1");
    CHECK_FALSE(deal.ok);
    CHECK(deal.error.find("加载牌堆失败") != std::string::npos);
    CHECK(deal.error.find(missing.string()) != std::string::npos);

    auto simulate = repl.run("simulate 1 2");
    CHECK_FALSE(simulate.ok);
    CHECK(simulate.error.find("加载牌堆失败") != std::string::npos);
    CHECK(simulate.error.find(missing.string()) != std::string::npos);

    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove_all(missing, ec);
}

TEST_CASE("cli: read-only/batch queries prefer active session deck")
{
    Repl repl;

    const std::filesystem::path deck_a = temp_dir("tkw_cli_active_deck_a");
    const std::filesystem::path deck_b = temp_dir("tkw_cli_active_deck_b");
    const std::filesystem::path save = temp_save("tkw-cli-active-deck.json");
    std::error_code ec;
    std::filesystem::remove(save, ec);
    REQUIRE(write_mini_deck(deck_a, "active-a", "A测"));
    REQUIRE(write_mini_deck(deck_b, "active-b", "B测"));

    // 活动会话优先：启动 deck 为 A，行内 new --deck B 后只读命令默认读 B（与 status 一致）。
    repl.session.base.deck = deck_a;
    REQUIRE(repl.run("new --players 2 --seed 1 --hand 0 --deck " + deck_b.string())
                .ok);
    CHECK(repl.session.deck == deck_b);

    auto cards = repl.run("cards");
    CHECK(cards.ok);
    CHECK(cards.out.find("牌表: " + deck_b.string()) != std::string::npos);
    CHECK(cards.out.find("B测") != std::string::npos);
    CHECK(cards.out.find("A测") == std::string::npos);

    auto rules = repl.run("rules");
    CHECK(rules.ok);
    CHECK(rules.out.find("牌表: " + deck_b.string()) != std::string::npos);

    auto audit = repl.run("audit");
    CHECK(audit.ok);
    CHECK(audit.out.find("牌表: " + deck_b.string()) != std::string::npos);

    // 行内 --deck 仍最高优先，可取回启动牌表 A。
    auto overridden = repl.run("cards --deck " + deck_a.string());
    CHECK(overridden.ok);
    CHECK(overridden.out.find("牌表: " + deck_a.string()) != std::string::npos);
    CHECK(overridden.out.find("A测") != std::string::npos);

    // 批量命令同样跟随活动会话牌表，不再回落启动 deck。
    CHECK(repl.run("deal 2 1 --hand 0").ok);
    CHECK(repl.run("simulate 1 2 --hand 0").ok);

    // 启动 --human 不被查询命令继承；行内显式 --human 仍被拒绝。
    repl.session.base.humans = {"P0"};
    CHECK(repl.run("cards").ok);
    CHECK(repl.run("rules").ok);
    CHECK(repl.run("audit").ok);
    auto explicit_human = repl.run("cards --human P0");
    CHECK_FALSE(explicit_human.ok);
    CHECK(explicit_human.error.find("不支持 --human") != std::string::npos);
    repl.session.base.humans.clear();

    // load 刻意非对称：仍以启动 base 匹配存档指纹，不跟随活动会话。在 A 上建局存档，
    // 切到活动会话 B 后 load 无 --deck，应由 base=A 命中成功（若误跟 B 则报牌表不符）。
    REQUIRE(repl.run("new --players 2 --seed 1 --hand 0").ok);
    CHECK(repl.session.deck == deck_a);
    REQUIRE(repl.run("save " + save.string()).ok);
    REQUIRE(repl.run("new --players 2 --seed 1 --hand 0 --deck " + deck_b.string())
                .ok);
    CHECK(repl.session.deck == deck_b);
    auto loaded = repl.run("load " + save.string());
    CHECK(loaded.ok);
    CHECK(repl.session.deck == deck_a);
    auto after_load = repl.run("cards");
    CHECK(after_load.out.find("牌表: " + deck_a.string()) != std::string::npos);

    std::filesystem::remove(save, ec);
    std::filesystem::remove_all(deck_a, ec);
    std::filesystem::remove_all(deck_b, ec);
}

TEST_CASE("cli: step/run/status/save help lists common options")
{
    Repl repl;

    for (const std::string &cmd : {"step", "run", "status", "save"})
    {
        auto help = repl.run(cmd + " --help");
        CHECK(help.ok);
        // 本命令「选项」段（位于继承段「公共选项」之前）应含 --players：
        // 用法行也列选项，故从「选项:」处起找，确保命中本命令选项段。
        const auto opt_at = help.console.find("选项:");
        const auto players_at =
            opt_at == std::string::npos
                ? std::string::npos
                : help.console.find("--players", opt_at);
        const auto inherited_at = help.console.find("公共选项:");
        CHECK(opt_at != std::string::npos);
        CHECK(players_at != std::string::npos);
        CHECK(inherited_at != std::string::npos);
        CHECK(players_at < inherited_at);
    }
}

TEST_CASE("cli: --no-human clears human seats")
{
    Repl repl;

    auto with_human = repl.run("new --human P0 --players 2 --seed 1");
    CHECK(with_human.ok);
    REQUIRE(repl.session.humans.size() == 1);
    CHECK(repl.session.humans[0] == "P0");

    // 启动选项带入的座位（REPL session.base）也能被行内 --no-human 清空
    repl.session.base.humans = {"P0"};
    auto cleared = repl.run("new --no-human --players 2 --seed 1");
    CHECK(cleared.ok);
    CHECK(repl.session.humans.empty());

    // 同一命令 --human 与 --no-human 并存：清空优先
    auto both = repl.run("new --human P0 --no-human --players 2 --seed 1");
    CHECK(both.ok);
    CHECK(repl.session.humans.empty());
}

TEST_CASE("cli: repl startup options survive option defaults")
{
    Repl repl;

    // 启动选项存为 session.base；REPL 每行命令都以 ctx 未命中回落 base。
    // 若任一公共选项误加 .default_value()，默认值会写进 ctx 并压过 base，
    // 此处 new 会建 4 人局而非 2 人。
    repl.session.base.players = 2;
    REQUIRE(repl.run("new").ok);
    auto status = repl.run("status");
    CHECK(status.ok);
    CHECK(status.out.find("存活: 2") != std::string::npos);
}

TEST_CASE("cli: status shows ai level")
{
    Repl repl;

    auto created = repl.run("new --players 2 --seed 1 --ai aggressive");
    CHECK(created.ok);
    CHECK(created.out.find("AI 难度: aggressive") != std::string::npos);
}

TEST_CASE("cli: load restores ai and stats")
{
    Repl repl;
    const std::filesystem::path file = temp_save("tkw-cli-meta.json");
    std::error_code ec;
    std::filesystem::remove(file, ec);

    REQUIRE(repl.run("new --players 2 --seed 1 --ai aggressive").ok);
    REQUIRE(repl.run("run").ok);
    const tkw::cli::BattleStats saved_stats = repl.session.stats;
    // 完整跑局至少产生击杀/伤害/阵亡之一，否则该用例观测不到恢复效果
    const bool stats_present =
        !saved_stats.damage_dealt.empty() || !saved_stats.healing.empty() ||
        !saved_stats.kills.empty() || !saved_stats.last_hit_source.empty() ||
        !saved_stats.died.empty();
    REQUIRE(stats_present);
    REQUIRE(repl.run("save " + file.string()).ok);

    // 重置为 simple 且统计清空
    REQUIRE(repl.run("new --players 2 --seed 1").ok);
    CHECK(repl.session.ai == tkw::cli::AiLevel::Simple);
    CHECK(repl.session.stats.damage_dealt.empty());

    auto loaded = repl.run("load " + file.string());
    CHECK(loaded.ok);
    CHECK(repl.session.ai == tkw::cli::AiLevel::Aggressive);
    CHECK(repl.session.stats.damage_dealt == saved_stats.damage_dealt);
    CHECK(repl.session.stats.healing == saved_stats.healing);
    CHECK(repl.session.stats.kills == saved_stats.kills);
    CHECK(repl.session.stats.last_hit_source == saved_stats.last_hit_source);
    CHECK(repl.session.stats.died == saved_stats.died);

    // 显式 --ai 覆盖存档 AI 档
    auto overridden = repl.run("load " + file.string() + " --ai simple");
    CHECK(overridden.ok);
    CHECK(repl.session.ai == tkw::cli::AiLevel::Simple);

    std::filesystem::remove(file, ec);
}

TEST_CASE("cli: load restores identity mode from a save")
{
    Repl repl;
    const std::filesystem::path file = temp_save("tkw-cli-identity.json");
    std::error_code ec;
    std::filesystem::remove(file, ec);

    REQUIRE(repl.run("new --mode identity --players 4 --seed 1").ok);
    REQUIRE(repl.run("step").ok);
    REQUIRE(repl.run("save " + file.string()).ok);

    // 重置为乱斗，再读回身份局档：模式与角色以存档为准。
    REQUIRE(repl.run("new --players 2 --seed 1").ok);
    REQUIRE(repl.session.game != nullptr);
    CHECK(repl.session.game->mode == tkw::game::GameMode::Brawl);

    auto loaded = repl.run("load " + file.string());
    CHECK(loaded.ok);
    REQUIRE(repl.session.game != nullptr);
    CHECK(repl.session.game->mode == tkw::game::GameMode::Identity);
    CHECK(repl.session.game->roles.at("P0") == tkw::game::Role::Lord);
    CHECK(loaded.out.find("模式: 身份局") != std::string::npos);

    std::filesystem::remove(file, ec);
}

TEST_CASE("cli: load rejects a non-JSON archive and keeps the session")
{
    Repl repl;
    const std::filesystem::path file = temp_save("tkw-cli-badjson.json");
    std::error_code ec;
    std::filesystem::remove(file, ec);
    REQUIRE(tkw::io::write_text(file, "{not json").is_ok());

    REQUIRE(repl.run("new --players 2 --seed 1").ok);
    REQUIRE(repl.run("step").ok);
    const std::string current = repl.session.state.current;
    const int turns = repl.session.state.turns;
    REQUIRE(repl.session.game != nullptr);
    const std::size_t entities = repl.session.game->entities.size();

    // 文本非 JSON：CLI 应把 reader 的 ParseError 包成「存档加载失败（JSON 非法）」
    auto bad = repl.run("load " + file.string());
    CHECK_FALSE(bad.ok);
    CHECK(bad.error.find("存档加载失败") != std::string::npos);
    CHECK(bad.error.find("JSON 非法") != std::string::npos);
    CHECK(bad.error.find("JSON") != std::string::npos);

    // 解析失败不污染会话：仍是原对局、原进度、原实体数
    CHECK(repl.session.active);
    REQUIRE(repl.session.game != nullptr);
    CHECK(repl.session.game->entities.size() == entities);
    CHECK(repl.session.state.current == current);
    CHECK(repl.session.state.turns == turns);

    std::filesystem::remove(file, ec);
}

TEST_CASE("cli: load rejects an archive written for a different deck")
{
    Repl repl;
    const std::filesystem::path dir = temp_dir("tkw_cli_bad_deck");
    const std::filesystem::path save = temp_save("tkw-cli-deck-mismatch.json");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(save, ec);

    // 合法一卡牌表：能成功加载，仅在读档指纹比对时与标准牌表不符
    REQUIRE(std::filesystem::create_directories(dir / "cards"));
    REQUIRE(tkw::io::write_text(
                dir / "deck.json", R"({"name":"cli","cards":["h0"]})")
                .is_ok());
    REQUIRE(tkw::io::write_text(
                dir / "cards" / "h0.json",
                R"({"id":"h0","name":"测","type":"basic",)"
                R"("copies":[{"suit":"spade","number":7}]})")
                .is_ok());

    // 标准牌表存档；读档换异牌堆 → deck.hash 不符
    REQUIRE(repl.run("new --players 2 --seed 1").ok);
    REQUIRE(repl.run("save " + save.string()).ok);

    auto mismatched =
        repl.run("load " + save.string() + " --deck " + dir.string());
    CHECK_FALSE(mismatched.ok);
    CHECK(mismatched.error.find("存档加载失败") != std::string::npos);
    CHECK(mismatched.error.find("牌表不符") != std::string::npos);
    CHECK(mismatched.error.find("deck.hash") != std::string::npos);

    // 建局成功但校验失败，仍不替换会话
    CHECK(repl.session.active);
    REQUIRE(repl.session.game != nullptr);
    CHECK(repl.session.game->entities.size() == 2);

    std::filesystem::remove(save, ec);
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("cli: save reports a write failure when the parent is missing")
{
    Repl repl;
    const std::filesystem::path dir = temp_dir("tkw_cli_no_parent_dir");
    const std::filesystem::path file = dir / "s.json";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    REQUIRE(repl.run("new --players 2 --seed 1").ok);

    // 父目录不存在，原子写的临时文件落盘即失败，不生成目标文件
    auto saved = repl.run("save " + file.string());
    CHECK_FALSE(saved.ok);
    CHECK(saved.error.find("写入存档失败") != std::string::npos);
    CHECK(saved.error.find("父目录不存在") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(file));

    // 会话未受影响，仍可继续推进
    CHECK(repl.session.active);
    CHECK(repl.run("step").ok);

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("cli: load rejects a directory with a non-file reason")
{
    Repl repl;
    const std::filesystem::path dir = temp_dir("tkw_cli_load_dir");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    REQUIRE(std::filesystem::create_directories(dir));

    REQUIRE(repl.run("new --players 2 --seed 1").ok);
    const std::string current = repl.session.state.current;

    // 目录存在但不是常规文件：read_text 报 NotAFile，文案保留路径与根因。
    auto loaded = repl.run("load " + dir.string());
    CHECK_FALSE(loaded.ok);
    CHECK(loaded.error.find("读取存档失败") != std::string::npos);
    CHECK(loaded.error.find("不是常规文件") != std::string::npos);
    CHECK(loaded.error.find(dir.string()) != std::string::npos);
    CHECK(repl.session.active);
    CHECK(repl.session.state.current == current);

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("cli: resource and loop failures render Chinese reason labels")
{
    using tkw::config::ConfigErrorKind;
    using tkw::game::LoopError;
    using tkw::game::TurnError;

    // 配置加载失败：六类根因各自的中文标签（穷举，防数值 kind 回归）。
    CHECK(tkw::cli::config_error_kind_zh(ConfigErrorKind::FileNotFound) ==
          "文件不存在");
    CHECK(tkw::cli::config_error_kind_zh(ConfigErrorKind::IoFailed) ==
          "文件读写失败");
    CHECK(tkw::cli::config_error_kind_zh(ConfigErrorKind::ParseError) ==
          "JSON 非法");
    CHECK(tkw::cli::config_error_kind_zh(ConfigErrorKind::MissingField) ==
          "缺少字段");
    CHECK(tkw::cli::config_error_kind_zh(ConfigErrorKind::TypeMismatch) ==
          "字段类型不符");
    CHECK(tkw::cli::config_error_kind_zh(ConfigErrorKind::InvalidValue) ==
          "字段值非法");

    // 对局流程失败：标签 + 合成文案前缀固定。
    CHECK(tkw::cli::loop_error_label_zh(LoopError::NoPlayers) == "无可用玩家");
    CHECK(tkw::cli::loop_error_label_zh(LoopError::TurnFailed) == "回合流程失败");
    CHECK(tkw::cli::loop_error_label_zh(LoopError::MaxRounds) == "达到最大回合数");
    CHECK(tkw::cli::loop_error_zh(LoopError::NoPlayers) ==
          "对局失败（无可用玩家）");

    // 回合根因：十类各自的中文标签（穷举，防漏/防回退）。
    CHECK(tkw::cli::turn_error_label_zh(TurnError::UnknownPlayer) ==
          "角色不存在");
    CHECK(tkw::cli::turn_error_label_zh(TurnError::UnknownCard) ==
          "卡牌定义缺失");
    CHECK(tkw::cli::turn_error_label_zh(TurnError::CardNotInHand) ==
          "手牌中没有该牌");
    CHECK(tkw::cli::turn_error_label_zh(TurnError::InvalidTarget) ==
          "目标非法");
    CHECK(tkw::cli::turn_error_label_zh(TurnError::ShaLimitExceeded) ==
          "本回合杀已达上限");
    CHECK(tkw::cli::turn_error_label_zh(TurnError::NotEquipment) ==
          "该牌不是装备");
    CHECK(tkw::cli::turn_error_label_zh(TurnError::DelayedDuplicate) ==
          "判定区已有同名延时锦囊");
    CHECK(tkw::cli::turn_error_label_zh(TurnError::PlayRejected) ==
          "出牌被拒绝");
    CHECK(tkw::cli::turn_error_label_zh(TurnError::DiscardInsufficient) ==
          "弃牌数量不足");
    CHECK(tkw::cli::turn_error_label_zh(TurnError::JudgeEmptyDeck) ==
          "判定时牌堆已空");

    // 合成文案：根因经出参渲染进「回合执行失败」；NoPlayers 回落角色不存在。
    // TurnFailed 追加恢复引导：失败回合可能已部分结算，但已被引擎消费并推进。
    CHECK(tkw::cli::detail::format_turn_failure(
              LoopError::TurnFailed, TurnError::DiscardInsufficient, "P0") ==
          "回合执行失败（角色 P0，弃牌数量不足；本回合已终止并跳过（已部分结算），可继续推进）");
    CHECK(tkw::cli::detail::format_turn_failure(
              LoopError::TurnFailed, TurnError::PlayRejected, "P0") ==
          "回合执行失败（角色 P0，出牌被拒绝；本回合已终止并跳过（已部分结算），可继续推进）");
    CHECK(tkw::cli::detail::format_turn_failure(
              LoopError::NoPlayers, TurnError::PlayRejected, "P1") ==
          "回合执行失败（角色 P1，角色不存在）");
}

TEST_CASE("cli: status reports a finished session and its winner")
{
    Repl repl;

    // 2p seed1 确定性终局：status 显示已结束与胜者，不再提示下一回合。
    REQUIRE(repl.run("new --players 2 --seed 1").ok);
    REQUIRE(repl.run("run").ok);
    auto finished = repl.run("status");
    CHECK(finished.ok);
    CHECK(finished.out.find("会话: 已结束") != std::string::npos);
    CHECK(finished.out.find("胜者: P0") != std::string::npos);
    CHECK(finished.out.find("下一回合") == std::string::npos);

    // 同归于尽（0 存活）：胜者回落「平局（同归于尽）」，不落空串。
    REQUIRE(repl.run("new --players 2 --seed 1").ok);
    auto ctx = repl.session.game->context();
    tkw::game::declare_death(ctx, "P0");
    tkw::game::declare_death(ctx, "P1");
    auto draw = repl.run("status");
    CHECK(draw.ok);
    CHECK(draw.out.find("会话: 已结束") != std::string::npos);
    CHECK(draw.out.find("胜者: 平局（同归于尽）") != std::string::npos);
    CHECK(draw.out.find("下一回合") == std::string::npos);
}

TEST_CASE("cli: identity terminal labels use camp names")
{
    Repl repl;

    REQUIRE(repl.run("new --mode identity --players 4 --seed 1").ok);
    auto ctx = repl.session.game->context();
    // 只手杀主公：终局为反贼阵营；反贼代表 id 可能已阵亡，不展示。
    tkw::game::declare_death(ctx, "P0");
    CHECK(tkw::game::session_over(ctx));

    auto finished = repl.run("status");
    CHECK(finished.ok);
    CHECK(finished.out.find("胜者: 反贼阵营胜") != std::string::npos);

    auto stepped = repl.run("step");
    CHECK(stepped.ok);
    CHECK(stepped.out.find("对局已结束，胜者: 反贼阵营胜") != std::string::npos);
}

TEST_CASE("cli: status shows the session deck source")
{
    Repl repl;

    // 默认牌表继承 REPL 启动选项，开局后 status 展示该来源。
    REQUIRE(repl.run("new --players 2 --seed 1 --hand 0").ok);
    CHECK(repl.session.deck == std::filesystem::path(TKW_TEST_RESOURCE_DIR));
    auto status = repl.run("status");
    CHECK(status.ok);
    CHECK(status.out.find("牌表: " + std::string(TKW_TEST_RESOURCE_DIR)) !=
          std::string::npos);

    // 行内 --deck 覆盖启动选项并记入会话；status 展示行内来源。
    const std::filesystem::path dir = temp_dir("tkw_cli_deck_source");
    const std::filesystem::path save = temp_save("tkw-cli-deck-source.json");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(save, ec);
    REQUIRE(std::filesystem::create_directories(dir / "cards"));
    REQUIRE(tkw::io::write_text(
                dir / "deck.json", R"({"name":"cli","cards":["h0"]})")
                .is_ok());
    REQUIRE(tkw::io::write_text(
                dir / "cards" / "h0.json",
                R"({"id":"h0","name":"测","type":"basic",)"
                R"("copies":[{"suit":"spade","number":7}]})")
                .is_ok());

    auto overridden = repl.run("new --players 2 --seed 1 --hand 0 --deck " +
                               dir.string());
    CHECK(overridden.ok);
    CHECK(repl.session.deck == dir);
    CHECK(overridden.out.find("牌表: " + dir.string()) != std::string::npos);

    // 读档同样以命令行牌表落子并记入会话（换回默认牌表后再读回）。
    REQUIRE(repl.run("save " + save.string()).ok);
    REQUIRE(repl.run("new --players 2 --seed 1 --hand 0").ok);
    CHECK(repl.session.deck == std::filesystem::path(TKW_TEST_RESOURCE_DIR));
    auto loaded = repl.run("load " + save.string() + " --deck " + dir.string());
    CHECK(loaded.ok);
    CHECK(repl.session.deck == dir);
    CHECK(loaded.out.find("牌表: " + dir.string()) != std::string::npos);

    std::filesystem::remove(save, ec);
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("cli: status flags a session at the round cap")
{
    Repl repl;

    // 普通进行中不出现上限提示。
    REQUIRE(repl.run("new --players 2 --seed 1").ok);
    auto normal = repl.run("status");
    CHECK(normal.ok);
    CHECK(normal.out.find("会话: 进行中") != std::string::npos);
    CHECK(normal.out.find("已达回合上限") == std::string::npos);

    // 上限压到 0：2p seed1 首回合无人阵亡，run 必走 MaxRounds 且会话未终结。
    repl.session.game->rules.max_turns = 0;
    auto ran = repl.run("run");
    CHECK(ran.ok);
    CHECK(ran.out.find("平局（达到最大回合数）") != std::string::npos);

    auto capped = repl.run("status");
    CHECK(capped.ok);
    CHECK(capped.out.find("会话: 进行中") != std::string::npos);
    CHECK(capped.out.find("已达回合上限") != std::string::npos);
    CHECK(capped.out.find("下一回合") != std::string::npos);
    CHECK(capped.out.find("会话: 已结束") == std::string::npos);
}

// ── 纯行构造器与 CLI 打印 wrapper 的行级字节护栏 ────────────────────────

TEST_CASE("cli: query_lines pure vectors are byte-exact")
{
    const std::filesystem::path deck = temp_dir("tkw_cli_lines_deck_reg");
    const std::filesystem::path ghost = temp_dir("tkw_cli_lines_deck_ghost");
    std::error_code ec;
    REQUIRE(write_mini_deck(deck, "reg-a", "A测"));
    REQUIRE(write_unknown_mechanism_deck(ghost));

    tkw::cli::Options opt;
    opt.deck = deck;
    const std::string head = "牌表: " + deck.string();

    auto plain = tkw::cli::detail::cards_lines(opt, false);
    REQUIRE(plain.is_ok());
    CHECK(plain.unwrap() ==
          std::vector<std::string>{head, "牌堆 reg-a（1 种 / 30 张）",
                                   "  A测(h0) 基本 30"});

    auto with_text = tkw::cli::detail::cards_lines(opt, true);
    REQUIRE(with_text.is_ok());
    CHECK(with_text.unwrap() ==
          std::vector<std::string>{head, "牌堆 reg-a（1 种 / 30 张）",
                                   "  A测(h0) 基本 30: （无说明）"});

    auto all = tkw::cli::detail::rules_lines(opt, "");
    REQUIRE(all.is_ok());
    CHECK(all.unwrap() == std::vector<std::string>{
                              head, "卡牌说明（1 种）:", "  A测(h0): （无说明）"});

    auto hit = tkw::cli::detail::rules_lines(opt, "A测");
    REQUIRE(hit.is_ok());
    CHECK(hit.unwrap() ==
          std::vector<std::string>{head, "卡牌说明（匹配「A测」的 1 种）:",
                                   "  A测(h0): （无说明）"});

    auto miss = tkw::cli::detail::rules_lines(opt, "zzz");
    REQUIRE(miss.is_ok());
    CHECK(miss.unwrap() ==
          std::vector<std::string>{head, "卡牌说明（匹配「zzz」的 0 种）:",
                                   "  没有匹配的卡牌说明。"});

    auto supported = tkw::cli::detail::audit_lines(opt);
    REQUIRE(supported.is_ok());
    CHECK(supported.unwrap() ==
          std::vector<std::string>{head, "牌堆全部可结算"});

    tkw::cli::Options ghost_opt;
    ghost_opt.deck = ghost;
    auto unsupported = tkw::cli::detail::audit_lines(ghost_opt);
    REQUIRE(unsupported.is_ok());
    CHECK(unsupported.unwrap() ==
          std::vector<std::string>{"牌表: " + ghost.string(),
                                   "未实现卡（1 张）:", "  幽魂(ghost)"});

    std::filesystem::remove_all(deck, ec);
    std::filesystem::remove_all(ghost, ec);
}

TEST_CASE("cli: decks_lines scans root and direct subdirectories")
{
    const std::filesystem::path root = temp_dir("tkw_cli_decks_root");
    const std::filesystem::path sub = root / "sub";
    const std::filesystem::path broken = root / "broken";
    const std::filesystem::path ignored = root / "ignored";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    // 根自身是牌表，sub 是直接子牌表；broken 有 deck.json 但 JSON 非法；ignored 无 deck.json。
    REQUIRE(write_mini_deck(root, "甲", "A测"));
    REQUIRE(write_mini_deck(sub, "乙", "B测"));
    REQUIRE(std::filesystem::create_directories(broken));
    REQUIRE(tkw::io::write_text(broken / "deck.json", "{not json").is_ok());
    REQUIRE(std::filesystem::create_directories(ignored));

    auto lines = tkw::cli::detail::decks_lines(root);
    REQUIRE(lines.is_ok());
    const auto &v = lines.unwrap();
    REQUIRE(v.size() == 4);
    CHECK(v[0] == "可用牌表（tkw decks [目录] 扫描；用 --deck <路径> 选择）:");
    CHECK(v[1] == "  " + root.string() + "  甲 1 种/30 张");
    CHECK(v[2].rfind("  " + broken.string() + "  加载牌堆失败（JSON 非法）: ", 0) ==
          0);
    CHECK(v[3] == "  " + sub.string() + "  乙 1 种/30 张");

    // 扫描根不存在：中文加载错误而非空清单。
    auto missing = tkw::cli::detail::decks_lines(root / "nope");
    REQUIRE(missing.is_err());
    CHECK(missing.unwrap_err().find("加载牌堆失败（文件不存在）") !=
          std::string::npos);

    std::filesystem::remove_all(root, ec);
}

TEST_CASE("cli: query wrappers print pure lines with trailing newline")
{
    const std::filesystem::path deck = temp_dir("tkw_cli_lines_deck_print");
    const std::filesystem::path ghost = temp_dir("tkw_cli_lines_deck_print_ghost");
    std::error_code ec;
    REQUIRE(write_mini_deck(deck, "reg-a", "A测"));
    REQUIRE(write_unknown_mechanism_deck(ghost));

    tkw::cli::Options opt;
    opt.deck = deck;

    {
        auto pure = tkw::cli::detail::cards_lines(opt, false);
        REQUIRE(pure.is_ok());
        const std::string out =
            capture_cout([&] { (void)tkw::cli::detail::cards_list(opt, false); });
        CHECK(out == join_lines(pure.unwrap()));
    }
    {
        auto pure = tkw::cli::detail::cards_lines(opt, true);
        REQUIRE(pure.is_ok());
        const std::string out =
            capture_cout([&] { (void)tkw::cli::detail::cards_list(opt, true); });
        CHECK(out == join_lines(pure.unwrap()));
    }
    {
        auto pure = tkw::cli::detail::rules_lines(opt, "");
        REQUIRE(pure.is_ok());
        const std::string out =
            capture_cout([&] { (void)tkw::cli::detail::rules_lookup(opt, ""); });
        CHECK(out == join_lines(pure.unwrap()));
    }
    {
        auto pure = tkw::cli::detail::rules_lines(opt, "A测");
        REQUIRE(pure.is_ok());
        const std::string out = capture_cout(
            [&] { (void)tkw::cli::detail::rules_lookup(opt, "A测"); });
        CHECK(out == join_lines(pure.unwrap()));
    }
    {
        auto pure = tkw::cli::detail::audit_lines(opt);
        REQUIRE(pure.is_ok());
        const std::string out =
            capture_cout([&] { (void)tkw::cli::detail::audit_deck(opt); });
        CHECK(out == join_lines(pure.unwrap()));
    }
    {
        tkw::cli::Options ghost_opt;
        ghost_opt.deck = ghost;
        auto pure = tkw::cli::detail::audit_lines(ghost_opt);
        REQUIRE(pure.is_ok());
        const std::string out =
            capture_cout([&] { (void)tkw::cli::detail::audit_deck(ghost_opt); });
        CHECK(out == join_lines(pure.unwrap()));
    }
    {
        auto pure = tkw::cli::detail::decks_lines(deck);
        REQUIRE(pure.is_ok());
        const std::string out =
            capture_cout([&] { (void)tkw::cli::detail::decks_list(opt); });
        CHECK(out == join_lines(pure.unwrap()));
    }

    std::filesystem::remove_all(deck, ec);
    std::filesystem::remove_all(ghost, ec);
}

TEST_CASE("cli: turn header and battle stats are byte-exact")
{
    tkw::game::GameSession session;
    session.current = "P0";
    session.turns = 0;
    CHECK(tkw::cli::detail::turn_header_text(session) == "—— 回合 1：P0 ——");
    session.current = "P2";
    session.turns = 6;
    CHECK(tkw::cli::detail::turn_header_text(session) == "—— 回合 7：P2 ——");
    CHECK(capture_cout([&] { tkw::cli::detail::print_turn_header(session); }) ==
          join_lines({tkw::cli::detail::turn_header_text(session)}));

    Repl repl;
    REQUIRE(repl.run("new --players 2 --seed 1").ok);
    REQUIRE(repl.session.game != nullptr);
    const tkw::game::Game &game = *repl.session.game;

    const std::vector<std::string> fresh = {
        "对局统计:", "  回合数: 5", "  胜者: 无",
        "  P0: 体力 4/4，击杀 0，伤害 0，治疗 0",
        "  P1: 体力 4/4，击杀 0，伤害 0，治疗 0"};
    CHECK(tkw::cli::detail::battle_stats_lines(tkw::save::BattleStats{}, game, "",
                                               5) == fresh);

    tkw::save::BattleStats stats;
    stats.kills["P0"] = 2;
    stats.damage_dealt["P0"] = 7;
    stats.healing["P0"] = 3;
    stats.died.insert("P1");
    auto ctx = repl.session.game->context();
    tkw::game::declare_death(ctx, "P1");
    const std::vector<std::string> after = {
        "对局统计:", "  回合数: 7", "  胜者: P0",
        "  P0: 体力 4/4，击杀 2，伤害 7，治疗 3",
        "  P1: 阵亡，击杀 0，伤害 0，治疗 0"};
    CHECK(tkw::cli::detail::battle_stats_lines(stats, game, "P0", 7) == after);
    CHECK(capture_cout(
              [&] { tkw::cli::detail::print_battle_stats(stats, game, "P0", 7); }) ==
          join_lines(after));
}

TEST_CASE("cli: unsupported cards warning lines stay empty without a deck")
{
    CHECK(tkw::cli::detail::unsupported_cards_warning_lines(
              tkw::card::CardDefCatalog{})
              .empty());
    std::ostringstream err;
    tkw::cli::detail::warn_unsupported_cards(tkw::card::CardDefCatalog{}, err);
    CHECK(err.str().empty());
}

TEST_CASE("cli: heroes lines list catalog and mark implemented skills")
{
    auto lines =
        tkw::cli::detail::heroes_lines(TKW_TEST_RESOURCE_DIR);
    REQUIRE(lines.is_ok());
    const std::string out = join_lines(lines.unwrap());
    CHECK(out.find("可用武将") != std::string::npos);
    CHECK(out.find("张飞(zhangfei)") != std::string::npos);
    CHECK(out.find("4体力") != std::string::npos);
    CHECK(out.find("技能: 咆哮") != std::string::npos);
    // 关羽武圣已实现：不再标「（未实现）」
    CHECK(out.find("关羽(guanyu)") != std::string::npos);
    CHECK(out.find("武圣") != std::string::npos);
    CHECK(out.find("武圣（未实现）") == std::string::npos);
    // 马超马术 / 黄月英奇才：新增锁定距离技一并列出且均已实现
    CHECK(out.find("马超(machao)") != std::string::npos);
    CHECK(out.find("技能: 马术") != std::string::npos);
    CHECK(out.find("马术（未实现）") == std::string::npos);
    CHECK(out.find("黄月英(huangyueying)") != std::string::npos);
    CHECK(out.find("3体力") != std::string::npos);
    CHECK(out.find("技能: 奇才") != std::string::npos);
    CHECK(out.find("奇才（未实现）") == std::string::npos);
    // 赵云龙胆：转化技，列出且已实现
    CHECK(out.find("赵云(zhaoyun)") != std::string::npos);
    CHECK(out.find("技能: 龙胆") != std::string::npos);
    CHECK(out.find("龙胆（未实现）") == std::string::npos);
    // 甄姬倾国：黑色牌当闪，列出且已实现
    CHECK(out.find("甄姬(zhenji)") != std::string::npos);
    CHECK(out.find("3体力") != std::string::npos);
    CHECK(out.find("技能: 倾国") != std::string::npos);
    CHECK(out.find("倾国（未实现）") == std::string::npos);

    // 无 heroes.json 的目录回落空数据而不报错
    const std::filesystem::path empty_dir =
        temp_dir("tkw_cli_heroes_empty");
    std::error_code ec;
    std::filesystem::remove_all(empty_dir, ec);
    REQUIRE(std::filesystem::create_directories(empty_dir));
    auto none = tkw::cli::detail::heroes_lines(empty_dir);
    REQUIRE(none.is_ok());
    CHECK(join_lines(none.unwrap()).find("无武将数据") != std::string::npos);
    std::filesystem::remove_all(empty_dir, ec);
}

TEST_CASE("cli: --hero selects a hero and rejects bad assignments")
{
    Repl repl;
    repl.session.base.deck = TKW_TEST_RESOURCE_DIR;

    // 指定座位武将后 status 显示武将名
    auto created = repl.run(
        "new --hero P0=zhangfei --players 2 --seed 1");
    REQUIRE(created.ok);
    CHECK(created.out.find("武将 张飞") != std::string::npos);
    CHECK(created.out.find("武将 无") != std::string::npos);
    REQUIRE(repl.session.game != nullptr);
    CHECK(repl.session.game->entities.find("P0").unwrap()->get_hero() ==
          "zhangfei");

    // 未知武将：中文错误并指出座位
    auto unknown = repl.run("new --hero P0=nobody --players 2 --seed 1");
    CHECK_FALSE(unknown.ok);
    CHECK(unknown.error.find("武将不存在") != std::string::npos);
    CHECK(unknown.error.find("P0") != std::string::npos);

    // 座位/格式/重复/越界：解析期即拒绝
    for (const char *bad : {"new --hero P0 --players 2 --seed 1",
                            "new --hero X=zhangfei --players 2 --seed 1",
                            "new --hero P0=zhangfei --hero P0=guanyu "
                            "--players 2 --seed 1",
                            "new --hero P9=zhangfei --players 2 --seed 1"})
    {
        auto r = repl.run(bad);
        CHECK_FALSE(r.ok);
    }
}

TEST_CASE("cli: no hero keeps default status without a hero column")
{
    Repl repl;
    repl.session.base.deck = TKW_TEST_RESOURCE_DIR;
    auto created = repl.run("new --players 2 --seed 1");
    REQUIRE(created.ok);
    CHECK(created.out.find("武将") == std::string::npos);
}

TEST_CASE("cli: --hero normalizes non-canonical seat keys")
{
    Repl repl;
    repl.session.base.deck = TKW_TEST_RESOURCE_DIR;

    // 前导零座位与规范写法等价：P00 仍命中 P0，不静默忽略
    auto created = repl.run("new --hero P00=zhangfei --players 2 --seed 1");
    REQUIRE(created.ok);
    REQUIRE(repl.session.game != nullptr);
    CHECK(repl.session.game->entities.find("P0").unwrap()->get_hero() ==
          "zhangfei");

    // 归一化键参与查重：P0 与 P00 是同一座位
    auto dup = repl.run(
        "new --hero P0=zhangfei --hero P00=guanyu --players 2 --seed 1");
    CHECK_FALSE(dup.ok);
    CHECK(dup.error.find("武将座位重复") != std::string::npos);

    // 越界仍拒绝，并给出可用座位范围
    auto oob = repl.run("new --hero P8=zhangfei --players 2 --seed 1");
    CHECK_FALSE(oob.ok);
    CHECK(oob.error.find("超出玩家数") != std::string::npos);
    CHECK(oob.error.find("可用座位") != std::string::npos);
}

TEST_CASE("cli: load rejects an explicit --hero before reading the file")
{
    Repl repl;
    repl.session.base.deck = TKW_TEST_RESOURCE_DIR;

    // 显式 --hero：中文拒绝并给出替代命令；拒绝早于读文件（不报文件不存在）。
    auto rejected = repl.run(
        "load /tmp/tkw-cli-definitely-missing.json --hero P0=zhangfei");
    CHECK_FALSE(rejected.ok);
    CHECK(rejected.error.find("不支持 --hero") != std::string::npos);
    CHECK(rejected.error.find("new --hero") != std::string::npos);
    CHECK(rejected.error.find("文件不存在") == std::string::npos);
}

TEST_CASE("cli: load ignores startup --hero when the line omits it")
{
    Repl repl;
    repl.session.base.deck = TKW_TEST_RESOURCE_DIR;

    const std::filesystem::path file =
        temp_save("tkw-cli-startup-hero-load.json");
    std::error_code ec;
    std::filesystem::remove(file, ec);

    // 启动 --hero 只是会话默认：new 选中后存档，无 --hero 的 load 仍走存档武将。
    repl.session.base.heroes = {"P0=zhangfei"};
    REQUIRE(repl.run("new --players 2 --seed 1").ok);
    REQUIRE(repl.run("save " + file.string()).ok);

    auto loaded = repl.run("load " + file.string());
    CHECK(loaded.ok);
    REQUIRE(repl.session.game != nullptr);
    CHECK(repl.session.game->entities.find("P0").unwrap()->get_hero() ==
          "zhangfei");

    std::filesystem::remove(file, ec);
}

TEST_CASE("cli: --hero selects different heroes on multiple seats")
{
    Repl repl;
    repl.session.base.deck = TKW_TEST_RESOURCE_DIR;

    auto created = repl.run(
        "new --hero P0=zhangfei --hero P1=guanyu --players 2 --seed 1");
    REQUIRE(created.ok);
    CHECK(created.out.find("武将 张飞") != std::string::npos);
    CHECK(created.out.find("武将 关羽") != std::string::npos);
    REQUIRE(repl.session.game != nullptr);
    CHECK(repl.session.game->entities.find("P0").unwrap()->get_hero() ==
          "zhangfei");
    CHECK(repl.session.game->entities.find("P1").unwrap()->get_hero() ==
          "guanyu");
}

TEST_CASE("cli: --hero coexists with identity roles")
{
    Repl repl;
    repl.session.base.deck = TKW_TEST_RESOURCE_DIR;

    // 引擎先分配武将再做身份角色洗牌：同一实体可同时有角色与武将，互不覆盖。
    auto created = repl.run(
        "new --mode identity --players 4 --seed 1 "
        "--hero P0=zhangfei --hero P1=guanyu");
    REQUIRE(created.ok);
    CHECK(created.out.find("模式: 身份局") != std::string::npos);
    CHECK(created.out.find("角色 主公") != std::string::npos);
    CHECK(created.out.find("武将 张飞") != std::string::npos);
    CHECK(created.out.find("武将 关羽") != std::string::npos);
    REQUIRE(repl.session.game != nullptr);
    CHECK(repl.session.game->roles.at("P0") == tkw::game::Role::Lord);
    CHECK(repl.session.game->entities.find("P0").unwrap()->get_hero() ==
          "zhangfei");
}
