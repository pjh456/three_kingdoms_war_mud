#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "cli/session.hpp"
#include "tui/controller.hpp"

namespace
{
    tkw::cli::Options test_options(int players = 2, std::uint32_t seed = 1)
    {
        tkw::cli::Options opt;
        opt.deck = TKW_TEST_RESOURCE_DIR;
        opt.players = players;
        opt.seed = seed;
        opt.autosave = "";  // 单测默认关自动存档，避免写工作目录
        return opt;
    }

    /** 临时存档路径；使用前清理旧文件。 */
    std::filesystem::path temp_save(const char *name)
    {
        auto path = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return path;
    }

    bool log_has_prefix(const std::vector<std::string> &lines,
                        const std::string &prefix)
    {
        for (const auto &line : lines)
            if (line.rfind(prefix, 0) == 0)
                return true;
        return false;
    }
}

TEST_CASE("tui: controller bootstrap starts an active session")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();

    CHECK(c.snapshot().active);
    REQUIRE(c.snapshot().players.size() == 2);
    CHECK(c.snapshot().turns == 0);
    CHECK_FALSE(c.running());
    CHECK(log_has_prefix(c.log_lines(), "[摸牌]"));
}

TEST_CASE("tui: step advances exactly one turn on the worker")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();

    c.execute_line("step");
    c.wait_idle();

    CHECK(c.snapshot().turns == 1);
    CHECK(c.snapshot().current == "P1");
    CHECK_FALSE(c.running());
}

TEST_CASE("tui: run drives the session to its end")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();

    c.execute_line("run");
    c.wait_idle();

    CHECK(c.snapshot().over);
    CHECK_FALSE(c.snapshot().winner_label.empty());
    CHECK_FALSE(c.running());
    CHECK(log_has_prefix(c.log_lines(), "对局结束"));
}

TEST_CASE("tui: save then load round-trips session progress")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();

    const auto path = temp_save("tkw-tui-controller-roundtrip.json");
    c.execute_line("new --players 2 --seed 1");
    c.execute_line("step");
    c.wait_idle();
    c.execute_line("save " + path.string());
    REQUIRE(std::filesystem::exists(path));

    const int turns = c.snapshot().turns;
    const std::string current = c.snapshot().current;

    c.execute_line("new --players 4 --seed 9");
    REQUIRE(c.snapshot().players.size() == 4);

    c.execute_line("load " + path.string());
    CHECK(c.snapshot().turns == turns);
    CHECK(c.snapshot().current == current);
    CHECK(c.snapshot().players.size() == 2);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("tui: request quit during run joins without hanging")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options(4, 1));
    c.bootstrap();

    c.execute_line("run");
    c.request_quit();
    c.wait_idle();

    CHECK_FALSE(c.running());
}

TEST_CASE("tui: quit autosaves and reports success")
{
    const auto path = temp_save("tkw-tui-autosave-ok.json");
    auto opt = test_options();
    opt.autosave = path;

    tkw::tui::Controller c;
    c.set_base_options(opt);
    c.bootstrap();
    c.execute_line("quit");

    CHECK(c.exit_message().find("已自动存档") != std::string::npos);
    CHECK(std::filesystem::exists(path));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("tui: autosave failure is not silent")
{
    auto opt = test_options();
    opt.autosave = "/nonexistent-dir/tkw-tui-autosave.json";

    tkw::tui::Controller c;
    c.set_base_options(opt);
    c.bootstrap();
    c.execute_line("quit");

    CHECK(c.exit_message().find("自动存档失败") != std::string::npos);
}

TEST_CASE("tui: invalid input logs a hint and leaves state untouched")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();

    const auto turns = c.snapshot().turns;
    const auto players = c.snapshot().players.size();
    const auto before = c.log_lines().size();

    c.execute_line("frobnicate");
    c.execute_line("new --players 99");
    c.execute_line("new --human P0");

    CHECK(c.snapshot().turns == turns);
    CHECK(c.snapshot().players.size() == players);
    CHECK(c.log_lines().size() > before);
    CHECK(log_has_prefix(c.log_lines(), "未知命令"));
}

TEST_CASE("tui: status appends a summary without advancing")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();

    const auto turns = c.snapshot().turns;
    c.execute_line("status");

    CHECK(c.snapshot().turns == turns);
    CHECK(log_has_prefix(c.log_lines(), "状态:"));
}

TEST_CASE("tui: repeated new and destruction release subscriptions safely")
{
    {
        tkw::tui::Controller c;
        c.set_base_options(test_options());
        c.bootstrap();
        c.execute_line("new --players 2 --seed 1");
        c.execute_line("new --players 4 --seed 2");
        CHECK(c.snapshot().players.size() == 4);
    }
    CHECK(true);  // 析构未崩溃即通过（句柄先于 Game 退订）
}
