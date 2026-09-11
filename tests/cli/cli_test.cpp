#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <pjh_cli.hpp>

#include "cli/commands.hpp"
#include "cli/error_zh.hpp"
#include "cli/help_zh.hpp"
#include "cli/render.hpp"

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

    // 三命令都不运行真人参与的对局：给出 --human 即硬拒绝（纯中文错误）。
    for (const std::string &line :
         {"audit --human P0", "cards --human P0", "simulate 1 --human P0"})
    {
        auto r = repl.run(line);
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("不支持 --human") != std::string::npos);
    }

    // 不带 --human 时行为不变（一次性命令不继承 session.base，显式给牌表目录）
    const std::string deck = TKW_TEST_RESOURCE_DIR;
    CHECK(repl.run("audit --deck " + deck).ok);
    CHECK(repl.run("cards --deck " + deck).ok);
    CHECK(repl.run("simulate 1 2 --deck " + deck).ok);
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
