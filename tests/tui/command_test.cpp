#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "cli/session.hpp"
#include "game/core/roles.hpp"
#include "tui/command.hpp"

using tkw::tui::CommandKind;
using tkw::tui::parse_command;

namespace
{
    tkw::cli::Options base_options()
    {
        tkw::cli::Options base;
        base.deck = "resources";
        base.players = 2;
        base.seed = 7;
        base.ai = tkw::cli::AiLevel::Aggressive;
        base.mode = tkw::game::GameMode::Brawl;
        return base;
    }
}

TEST_CASE("tui: new inherits base options when unspecified")
{
    const auto base = base_options();
    auto r = parse_command("new", base);
    REQUIRE(r.is_ok());
    const auto &cmd = r.unwrap();
    CHECK(cmd.kind == CommandKind::New);
    CHECK(cmd.options.players == 2);
    CHECK(cmd.options.seed == 7);
    CHECK(cmd.options.deck == "resources");
    CHECK(cmd.options.ai == tkw::cli::AiLevel::Aggressive);
    CHECK(cmd.options.mode == tkw::game::GameMode::Brawl);
    CHECK_FALSE(cmd.run_to_end);
}

TEST_CASE("tui: new applies inline overrides over base")
{
    auto r = parse_command(
        "new --players 4 --seed 99 --hand 3 --mode identity --ai simple "
        "--deck /tmp/deck",
        base_options());
    REQUIRE(r.is_ok());
    const auto &cmd = r.unwrap();
    CHECK(cmd.options.players == 4);
    CHECK(cmd.options.seed == 99);
    CHECK(cmd.options.hand == 3);
    CHECK(cmd.options.mode == tkw::game::GameMode::Identity);
    CHECK(cmd.options.ai == tkw::cli::AiLevel::Simple);
    CHECK(cmd.options.deck == "/tmp/deck");
}

TEST_CASE("tui: parse errors carry chinese hints")
{
    const auto base = base_options();

    auto unknown = parse_command("frobnicate", base);
    REQUIRE(unknown.is_err());
    CHECK(unknown.unwrap_err().find("未知命令") != std::string::npos);

    auto bad_option = parse_command("new --bogus 1", base);
    REQUIRE(bad_option.is_err());
    CHECK(bad_option.unwrap_err().find("未知选项") != std::string::npos);

    auto missing = parse_command("new --players", base);
    REQUIRE(missing.is_err());
    CHECK(missing.unwrap_err().find("需要一个值") != std::string::npos);

    auto out_of_range = parse_command("new --players 99", base);
    REQUIRE(out_of_range.is_err());
    CHECK(out_of_range.unwrap_err().find("超出范围") != std::string::npos);

    auto bad_seed = parse_command("new --seed -1", base);
    REQUIRE(bad_seed.is_err());
    CHECK(bad_seed.unwrap_err().find("非负整数") != std::string::npos);

    auto human = parse_command("new --human P0", base);
    REQUIRE(human.is_err());
    CHECK(human.unwrap_err().find("不支持真人") != std::string::npos);

    auto empty = parse_command("   ", base);
    REQUIRE(empty.is_err());
    CHECK(empty.unwrap_err().find("空命令") != std::string::npos);
}

TEST_CASE("tui: cli-only queries point to tkw")
{
    const auto base = base_options();

    auto cards = parse_command("cards", base);
    REQUIRE(cards.is_err());
    CHECK(cards.unwrap_err().find("tkw cards") != std::string::npos);

    auto rules = parse_command("rules", base);
    REQUIRE(rules.is_err());
    CHECK(rules.unwrap_err().find("tkw rules") != std::string::npos);

    auto rules_keyword = parse_command("rules 杀", base);
    REQUIRE(rules_keyword.is_err());
    CHECK(rules_keyword.unwrap_err().find("tkw rules") != std::string::npos);

    auto audit = parse_command("audit", base);
    REQUIRE(audit.is_err());
    CHECK(audit.unwrap_err().find("tkw audit") != std::string::npos);

    auto simulate = parse_command("simulate", base);
    REQUIRE(simulate.is_err());
    CHECK(simulate.unwrap_err().find("tkw simulate") != std::string::npos);

    auto unknown = parse_command("frobnicate", base);
    REQUIRE(unknown.is_err());
    CHECK(unknown.unwrap_err().find("未知命令") != std::string::npos);
}

TEST_CASE("tui: deal takes positional players and seed")
{
    auto r = parse_command("deal 3 11", base_options());
    REQUIRE(r.is_ok());
    const auto &cmd = r.unwrap();
    CHECK(cmd.kind == CommandKind::Deal);
    CHECK(cmd.options.players == 3);
    CHECK(cmd.options.seed == 11);
    CHECK(cmd.run_to_end);

    auto missing = parse_command("deal 3", base_options());
    REQUIRE(missing.is_err());
    CHECK(missing.unwrap_err().find("两个参数") != std::string::npos);
}

TEST_CASE("tui: run and control aliases resolve")
{
    auto run = parse_command("r", base_options());
    REQUIRE(run.is_ok());
    CHECK(run.unwrap().kind == CommandKind::Run);
    CHECK(run.unwrap().run_to_end);

    CHECK(parse_command("st", base_options()).unwrap().kind ==
          CommandKind::Status);
    CHECK(parse_command("q", base_options()).unwrap().kind == CommandKind::Quit);
    CHECK(parse_command("?", base_options()).unwrap().kind == CommandKind::Help);
    CHECK(parse_command("step", base_options()).unwrap().kind ==
          CommandKind::Step);

    auto extra = parse_command("step now", base_options());
    REQUIRE(extra.is_err());
    CHECK(extra.unwrap_err().find("不接受参数") != std::string::npos);
}

TEST_CASE("tui: save and load require exactly one file")
{
    auto save = parse_command("save /tmp/a.json", base_options());
    REQUIRE(save.is_ok());
    CHECK(save.unwrap().kind == CommandKind::Save);
    CHECK(save.unwrap().file == "/tmp/a.json");

    auto load = parse_command("load /tmp/a.json", base_options());
    REQUIRE(load.is_ok());
    CHECK(load.unwrap().kind == CommandKind::Load);

    auto missing = parse_command("save", base_options());
    REQUIRE(missing.is_err());
    CHECK(missing.unwrap_err().find("需要 <file>") != std::string::npos);

    auto too_many = parse_command("load a b", base_options());
    REQUIRE(too_many.is_err());
    CHECK(too_many.unwrap_err().find("只接受一个") != std::string::npos);
}
