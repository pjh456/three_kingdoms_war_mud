#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

    bool log_contains(const std::vector<std::string> &lines,
                      const std::string &needle)
    {
        for (const auto &line : lines)
            if (line.find(needle) != std::string::npos)
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
    CHECK(log_has_prefix(c.log_lines(), "—— 回合 1：P0 ——"));
}

TEST_CASE("tui: run writes a turn header per turn")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();

    c.execute_line("run");
    c.wait_idle();

    int headers = 0;
    for (const auto &line : c.log_lines())
        if (line.rfind("—— 回合 ", 0) == 0 &&
            line.size() >= std::string("—— 回合 ").size() + 3 &&
            line.find(" ——") != std::string::npos)
            ++headers;
    CHECK(headers > 1);
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

TEST_CASE("tui: run appends battle stats block")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();

    c.execute_line("run");
    c.wait_idle();

    CHECK(log_has_prefix(c.log_lines(), "对局统计:"));
    CHECK(log_contains(c.log_lines(), "回合数:"));
    CHECK(log_contains(c.log_lines(), "胜者:"));
    CHECK(log_contains(c.log_lines(), "击杀"));
    CHECK(log_contains(c.log_lines(), "伤害"));
    CHECK(log_contains(c.log_lines(), "治疗"));
}

TEST_CASE("tui: standard deck has no unsupported warning")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();

    CHECK_FALSE(log_contains(c.log_lines(), "警告: 牌堆含"));
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

TEST_CASE("tui: help mentions in-place query commands")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();

    c.execute_line("help");

    bool mentions_cards = false;
    bool mentions_rules = false;
    bool mentions_audit = false;
    bool mentions_simulate = false;
    for (const auto &line : c.log_lines())
    {
        if (line.find("cards") != std::string::npos)
            mentions_cards = true;
        if (line.find("rules") != std::string::npos)
            mentions_rules = true;
        if (line.find("audit") != std::string::npos)
            mentions_audit = true;
        if (line.find("tkw simulate") != std::string::npos)
            mentions_simulate = true;
    }
    CHECK(mentions_cards);
    CHECK(mentions_rules);
    CHECK(mentions_audit);
    CHECK(mentions_simulate);
}

TEST_CASE("tui: help lists aliases defaults and log behavior")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();

    c.execute_line("help");

    CHECK(log_contains(c.log_lines(), "save/w"));
    CHECK(log_contains(c.log_lines(), "load/l"));
    CHECK(log_contains(c.log_lines(), "默认"));
    CHECK(log_contains(c.log_lines(), "事件日志"));
}

TEST_CASE("tui: help keyword filters the command table")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();

    c.execute_line("help new");

    CHECK(log_contains(c.log_lines(), "  new ["));
    CHECK_FALSE(log_contains(c.log_lines(), "audit [--deck 路径]"));

    c.execute_line("? 牌");

    CHECK(log_contains(c.log_lines(), "只读牌表查询"));
    CHECK_FALSE(log_contains(c.log_lines(), "tkw simulate"));
}

TEST_CASE("tui: help without keyword still prints the full table")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();

    c.execute_line("help");

    CHECK(log_contains(c.log_lines(), "TUI 命令与用法"));
    CHECK(log_contains(c.log_lines(), "tkw simulate"));
    CHECK(log_contains(c.log_lines(), "键位"));
}

TEST_CASE("tui: unknown command logs did-you-mean hint")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();

    c.execute_line("runn");

    CHECK(log_contains(c.log_lines(), "未知命令"));
    CHECK(log_contains(c.log_lines(), "是否想输入"));
    CHECK(log_contains(c.log_lines(), "run"));
}

TEST_CASE("tui: cards query writes deck listing to log")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();

    c.execute_line("cards");

    CHECK(log_has_prefix(c.log_lines(), "牌表: "));
    bool deck_name = false;
    bool sha_line = false;
    for (const auto &line : c.log_lines())
    {
        if (line.find("标准版") != std::string::npos)
            deck_name = true;
        if (line.find("杀(sha) 基本 30") != std::string::npos)
            sha_line = true;
    }
    CHECK(deck_name);
    CHECK(sha_line);
}

TEST_CASE("tui: rules query filters by keyword")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();

    c.execute_line("rules 杀");

    bool sha_line = false;
    bool guohe_line = false;
    for (const auto &line : c.log_lines())
    {
        if (line.find("杀(sha):") != std::string::npos)
            sha_line = true;
        if (line.find("过河拆桥(guohe):") != std::string::npos)
            guohe_line = true;
    }
    CHECK(sha_line);
    CHECK_FALSE(guohe_line);
}

TEST_CASE("tui: audit query reports settleable deck")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();

    c.execute_line("audit");

    CHECK(log_has_prefix(c.log_lines(), "牌表: "));
    bool reported = false;
    for (const auto &line : c.log_lines())
        if (line.find("牌堆全部可结算") != std::string::npos ||
            line.find("未实现卡") != std::string::npos)
            reported = true;
    CHECK(reported);
}

TEST_CASE("tui: query uses active session deck over startup")
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "tkw-tui-query-deck";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const std::filesystem::path deck_a = root / "a";
    const std::filesystem::path deck_b = root / "b";
    std::filesystem::create_directories(deck_a / "cards", ec);
    std::filesystem::create_directories(deck_b / "cards", ec);
    {
        std::ofstream(deck_a / "deck.json")
            << R"({"name":"甲","cards":["h0"]})";
        std::ofstream(deck_a / "cards" / "h0.json")
            << R"({"id":"h0","name":"甲卡","type":"basic","copies":[{"suit":"spade","number":7}]})";
        std::ofstream(deck_b / "deck.json")
            << R"({"name":"乙","cards":["h0"]})";
        std::ofstream(deck_b / "cards" / "h0.json")
            << R"({"id":"h0","name":"乙卡","type":"basic","copies":[{"suit":"spade","number":7}]})";
    }

    auto opt = test_options();
    opt.deck = deck_a;
    tkw::tui::Controller c;
    c.set_base_options(opt);
    c.bootstrap();

    c.execute_line("new --players 2 --seed 1 --hand 0 --deck " +
                   deck_b.string());
    c.execute_line("cards");

    bool source_b = false;
    bool name_b = false;
    bool card_b = false;
    bool source_a = false;
    for (const auto &line : c.log_lines())
    {
        if (line.find("牌表: " + deck_b.string()) != std::string::npos)
            source_b = true;
        if (line.find("乙") != std::string::npos)
            name_b = true;
        if (line.find("乙卡") != std::string::npos)
            card_b = true;
        if (line.find("牌表: " + deck_a.string()) != std::string::npos)
            source_a = true;
    }
    CHECK(source_b);
    CHECK(name_b);
    CHECK(card_b);
    CHECK_FALSE(source_a);

    std::filesystem::remove_all(root, ec);
}

TEST_CASE("tui: query inline deck overrides active session")
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "tkw-tui-query-inline-deck";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const std::filesystem::path deck_a = root / "a";
    const std::filesystem::path deck_b = root / "b";
    const std::filesystem::path deck_c = root / "c";
    for (const auto &dir : {deck_a, deck_b, deck_c})
        std::filesystem::create_directories(dir / "cards", ec);
    const auto write_deck = [](const std::filesystem::path &dir,
                               const std::string &name,
                               const std::string &card_name)
    {
        std::ofstream(dir / "deck.json")
            << "{\"name\":\"" + name + "\",\"cards\":[\"h0\"]}";
        std::ofstream(dir / "cards" / "h0.json")
            << "{\"id\":\"h0\",\"name\":\"" + card_name +
                   "\",\"type\":\"basic\","
                   "\"copies\":[{\"suit\":\"spade\",\"number\":7}]}";
    };
    write_deck(deck_a, "甲", "甲卡");
    write_deck(deck_b, "乙", "乙卡");
    write_deck(deck_c, "丙", "丙卡");

    auto opt = test_options();
    opt.deck = deck_a;
    tkw::tui::Controller c;
    c.set_base_options(opt);
    c.bootstrap();

    c.execute_line("new --players 2 --seed 1 --hand 0 --deck " +
                   deck_b.string());
    const std::size_t before = c.log_lines().size();

    // 行内 --deck 应同时压过活动会话（B）与启动（A）。
    c.execute_line("cards --deck " + deck_c.string());
    c.execute_line("rules --deck " + deck_c.string());
    c.execute_line("audit --deck " + deck_c.string());

    std::vector<std::string> inline_lines(c.log_lines().begin() +
                                              static_cast<std::ptrdiff_t>(before),
                                          c.log_lines().end());
    CHECK(log_contains(inline_lines, "牌表: " + deck_c.string()));
    CHECK(log_contains(inline_lines, "丙卡"));
    CHECK_FALSE(log_contains(inline_lines, "牌表: " + deck_b.string()));
    CHECK_FALSE(log_contains(inline_lines, "牌表: " + deck_a.string()));

    // 行内 deck 只影响当行：随后不带 --deck 的查询仍读活动会话 B。
    const std::size_t after_inline = c.log_lines().size();
    c.execute_line("cards");
    std::vector<std::string> plain_lines(
        c.log_lines().begin() + static_cast<std::ptrdiff_t>(after_inline),
        c.log_lines().end());
    CHECK(log_contains(plain_lines, "牌表: " + deck_b.string()));
    CHECK_FALSE(log_contains(plain_lines, "牌表: " + deck_c.string()));

    std::filesystem::remove_all(root, ec);
}

TEST_CASE("tui: query bad inline deck reports one error line")
{
    tkw::tui::Controller c;
    c.set_base_options(test_options());
    c.bootstrap();
    REQUIRE(c.snapshot().active);

    const std::size_t before = c.log_lines().size();
    c.execute_line("cards --deck /nonexistent-deck");

    CHECK(c.snapshot().active);
    CHECK_FALSE(c.running());
    CHECK(c.log_lines().size() == before + 1);
    CHECK(log_has_prefix(c.log_lines(), "加载牌堆失败"));
}

TEST_CASE("tui: query during run is rejected")
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

    c.execute_line("cards");
    CHECK(log_has_prefix(c.log_lines(), "引擎运行中"));

    // 逐个提交直到 worker 结束，避免残留阻塞。
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
    CHECK(log_contains(c.log_lines(), "存活: 2"));
}

TEST_CASE("tui: no-session commands point to help")
{
    auto opt = test_options();
    opt.deck = "/nonexistent-deck";  // 建局失败 → 会话 inactive
    tkw::tui::Controller c;
    c.set_base_options(opt);
    c.bootstrap();
    REQUIRE_FALSE(c.snapshot().active);

    c.execute_line("step");
    c.execute_line("save /tmp/x.json");

    bool no_session = false;
    bool mentions_help = false;
    for (const auto &line : c.log_lines())
    {
        if (line.find("没有进行中的对局") == std::string::npos)
            continue;
        no_session = true;
        if (line.find("help") != std::string::npos)
            mentions_help = true;
    }
    CHECK(no_session);
    CHECK(mentions_help);
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
