#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

TEST_CASE("tui: help mentions cli-only card queries")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();

    c.execute_line("help");

    bool mentions_rules = false;
    for (const auto &line : c.log_lines())
        if (line.find("tkw rules") != std::string::npos)
            mentions_rules = true;
    CHECK(mentions_rules);
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
    c.execute_line("new --human P9");

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

TEST_CASE("tui: new --human validates and fills session seats")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();

    c.execute_line("new --human P0 --players 2 --seed 1");
    REQUIRE(c.snapshot().active);
    CHECK(c.snapshot().humans == std::vector<std::string>({"P0"}));
    CHECK(c.snapshot().viewer == "P0");
    CHECK_FALSE(c.running());

    // 非法座位只提示、不替换旧会话。
    c.execute_line("new --human P9 --players 2 --seed 1");
    CHECK(log_has_prefix(c.log_lines(), "真人座位不存在"));
    CHECK(c.snapshot().humans == std::vector<std::string>({"P0"}));
    CHECK(c.snapshot().players.size() == 2);
}

TEST_CASE("tui: human step blocks until decision submitted")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();
    c.execute_line("new --human P0 --players 2 --seed 1");
    REQUIRE_FALSE(c.running());

    c.execute_line("step");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool saw_pending = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (c.has_pending_decision())
        {
            saw_pending = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(saw_pending);

    // 逐个提交直到 worker 结束；弃牌按需选满，其余取首项（无候选则放弃）。
    while (c.running() && std::chrono::steady_clock::now() < deadline)
    {
        tkw::tui::DecisionPanelView panel;
        if (!c.fetch_new_decision(panel))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        std::vector<std::size_t> selected;
        if (panel.options.empty())
        {
            c.submit_decision(selected, panel.allow_pass);
            continue;
        }
        if (panel.kind == tkw::game::ai::DecisionKind::Discard)
            for (int i = 0; i < panel.need_count &&
                            static_cast<std::size_t>(i) < panel.options.size();
                 ++i)
                selected.push_back(static_cast<std::size_t>(i));
        else
            selected.push_back(0);
        c.submit_decision(selected, false);
    }
    c.wait_idle();

    CHECK_FALSE(c.running());
    CHECK(c.snapshot().turns == 1);
}

TEST_CASE("tui: pending human decision exposes post-draw hand")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());

    // 用队列式 Post 把模型写回主线程，读取快照与 worker 入队不竞争。
    std::mutex tasks_mutex;
    std::vector<std::function<void()>> tasks;
    c.set_post(
        [&](std::function<void()> task)
        {
            std::lock_guard<std::mutex> lock(tasks_mutex);
            tasks.push_back(std::move(task));
        });
    const auto drain = [&]
    {
        std::vector<std::function<void()>> pending;
        {
            std::lock_guard<std::mutex> lock(tasks_mutex);
            pending.swap(tasks);
        }
        for (auto &task : pending)
            task();
    };
    const auto find_hand = [](const tkw::tui::UiSnapshot &snap,
                              const std::string &id) -> const tkw::tui::ZoneView *
    {
        for (const auto &row : snap.players)
            if (row.id == id)
                return &row.hand;
        return nullptr;
    };

    c.bootstrap();
    c.execute_line("new --human P0 --players 2 --seed 1");
    REQUIRE_FALSE(c.running());

    const auto *before = find_hand(c.snapshot(), "P0");
    REQUIRE(before != nullptr);
    const std::size_t expected =
        before->count + static_cast<std::size_t>(
                            tkw::game::RulesConfig{}.draw_per_turn);

    c.execute_line("step");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::size_t observed = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        drain();
        if (c.has_pending_decision())
        {
            const auto *row = find_hand(c.snapshot(), "P0");
            observed = row ? row->count : 0;
            if (observed == expected)
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 阻塞等待出牌决策时，摸牌阶段已结束，快照应已是摸牌后的手牌。
    CHECK(observed == expected);
    CHECK(c.running());
    CHECK(c.has_pending_decision());
    const auto *row = find_hand(c.snapshot(), "P0");
    REQUIRE(row != nullptr);
    CHECK(row->revealed);
    CHECK(row->cards.size() == expected);

    // 排空待决让 worker 收尾，避免析构时残留阻塞。
    while (c.running() && std::chrono::steady_clock::now() < deadline)
    {
        drain();
        tkw::tui::DecisionPanelView panel;
        if (!c.fetch_new_decision(panel))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        std::vector<std::size_t> selected;
        if (panel.options.empty())
        {
            c.submit_decision(selected, panel.allow_pass);
            continue;
        }
        if (panel.kind == tkw::game::ai::DecisionKind::Discard)
            for (int i = 0; i < panel.need_count &&
                            static_cast<std::size_t>(i) < panel.options.size();
                 ++i)
                selected.push_back(static_cast<std::size_t>(i));
        else
            selected.push_back(0);
        c.submit_decision(selected, false);
    }
    drain();
    c.request_quit();
    c.wait_idle();
}

TEST_CASE("tui: quit during pending human decision joins promptly")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();
    c.execute_line("new --human P0 --players 2 --seed 1");
    c.execute_line("step");

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool saw_pending = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (c.has_pending_decision())
        {
            saw_pending = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(saw_pending);

    c.request_quit();
    c.wait_idle();
    CHECK_FALSE(c.running());
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
