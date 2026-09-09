#include <doctest/doctest.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#include <pjh_cli.hpp>

#include "cli/commands.hpp"

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

    auto ran = repl.run("run");
    CHECK(ran.ok);
    CHECK(repl.session.state.turns >= 2);
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
    CHECK(step.error == "没有进行中的对局");

    auto run = repl.run("run");
    CHECK_FALSE(run.ok);
    CHECK(run.error == "没有进行中的对局");

    auto save = repl.run("save /tmp/tkw-cli-missing-session.json");
    CHECK_FALSE(save.ok);
    CHECK(save.error == "没有进行中的对局");

    auto status = repl.run("status");
    CHECK(status.ok);
    CHECK(status.out.find("会话: 无") != std::string::npos);

    auto load = repl.run("load /tmp/tkw-cli-definitely-missing.json");
    CHECK_FALSE(load.ok);
    CHECK(load.error.find("读取存档失败") != std::string::npos);
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

    auto filtered = repl.run("? aud");
    CHECK(filtered.ok);
    CHECK(filtered.console.find("audit") != std::string::npos);

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

TEST_CASE("cli: invalid human seats are rejected")
{
    Repl repl;

    auto missing = repl.run("new --human P9 --players 2 --seed 1");
    CHECK_FALSE(missing.ok);
    CHECK(missing.error.find("真人座位不存在") != std::string::npos);

    auto duplicate = repl.run("new --human P0 --human P0 --players 2 --seed 1");
    CHECK_FALSE(duplicate.ok);
    CHECK(duplicate.error.find("真人座位重复") != std::string::npos);
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
