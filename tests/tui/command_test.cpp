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

TEST_CASE("tui: new accepts repeatable human seats and no-human reset")
{
    auto base = base_options();
    base.humans = {"P1"};

    auto single = parse_command("new --human P0", base);
    REQUIRE(single.is_ok());
    REQUIRE(single.unwrap().options.humans.size() == 2);
    CHECK(single.unwrap().options.humans[0] == "P1");
    CHECK(single.unwrap().options.humans[1] == "P0");

    auto repeated = parse_command("new --human P0 --human P2", base);
    REQUIRE(repeated.is_ok());
    CHECK(repeated.unwrap().options.humans ==
          std::vector<std::string>({"P1", "P0", "P2"}));

    auto cleared = parse_command("new --no-human --human P3", base);
    REQUIRE(cleared.is_ok());
    CHECK(cleared.unwrap().options.humans ==
          std::vector<std::string>({"P3"}));

    auto missing = parse_command("new --human", base);
    REQUIRE(missing.is_err());
    CHECK(missing.unwrap_err().find("需要一个值") != std::string::npos);
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

    auto empty = parse_command("   ", base);
    REQUIRE(empty.is_err());
    CHECK(empty.unwrap_err().find("空命令") != std::string::npos);
}

TEST_CASE("tui: simulate points to cli and unknown stays unknown")
{
    const auto base = base_options();

    auto simulate = parse_command("simulate", base);
    REQUIRE(simulate.is_err());
    CHECK(simulate.unwrap_err().find("tkw simulate") != std::string::npos);

    auto unknown = parse_command("frobnicate", base);
    REQUIRE(unknown.is_err());
    CHECK(unknown.unwrap_err().find("未知命令") != std::string::npos);
}

TEST_CASE("tui: cards/rules/audit parse into query commands")
{
    const auto base = base_options();

    auto cards = parse_command("cards", base);
    REQUIRE(cards.is_ok());
    CHECK(cards.unwrap().kind == CommandKind::Cards);
    CHECK_FALSE(cards.unwrap().with_text);

    auto cards_text = parse_command("cards --text", base);
    REQUIRE(cards_text.is_ok());
    CHECK(cards_text.unwrap().kind == CommandKind::Cards);
    CHECK(cards_text.unwrap().with_text);

    auto cards_bad = parse_command("cards bogus", base);
    REQUIRE(cards_bad.is_err());
    CHECK(cards_bad.unwrap_err().find("只接受") != std::string::npos);

    auto rules = parse_command("rules", base);
    REQUIRE(rules.is_ok());
    CHECK(rules.unwrap().kind == CommandKind::Rules);
    CHECK(rules.unwrap().keyword.empty());

    auto rules_keyword = parse_command("rules 杀", base);
    REQUIRE(rules_keyword.is_ok());
    CHECK(rules_keyword.unwrap().kind == CommandKind::Rules);
    CHECK(rules_keyword.unwrap().keyword == "杀");

    auto rules_many = parse_command("rules a b", base);
    REQUIRE(rules_many.is_err());
    CHECK(rules_many.unwrap_err().find("一个") != std::string::npos);

    auto audit = parse_command("audit", base);
    REQUIRE(audit.is_ok());
    CHECK(audit.unwrap().kind == CommandKind::Audit);

    auto audit_arg = parse_command("audit x", base);
    REQUIRE(audit_arg.is_err());
    CHECK(audit_arg.unwrap_err().find("不接受参数") != std::string::npos);
}

TEST_CASE("tui: query commands accept inline deck override")
{
    const auto base = base_options();

    auto cards = parse_command("cards --deck /tmp/d", base);
    REQUIRE(cards.is_ok());
    CHECK(cards.unwrap().kind == CommandKind::Cards);
    CHECK(cards.unwrap().deck_provided);
    CHECK(cards.unwrap().options.deck == "/tmp/d");
    CHECK_FALSE(cards.unwrap().with_text);

    auto cards_both = parse_command("cards --text --deck /tmp/d", base);
    REQUIRE(cards_both.is_ok());
    CHECK(cards_both.unwrap().with_text);
    CHECK(cards_both.unwrap().deck_provided);
    CHECK(cards_both.unwrap().options.deck == "/tmp/d");

    auto cards_missing = parse_command("cards --deck", base);
    REQUIRE(cards_missing.is_err());
    CHECK(cards_missing.unwrap_err().find("需要一个值") != std::string::npos);

    auto rules_before = parse_command("rules --deck /tmp/d 杀", base);
    REQUIRE(rules_before.is_ok());
    CHECK(rules_before.unwrap().keyword == "杀");
    CHECK(rules_before.unwrap().deck_provided);
    CHECK(rules_before.unwrap().options.deck == "/tmp/d");

    auto rules_after = parse_command("rules 杀 --deck /tmp/d", base);
    REQUIRE(rules_after.is_ok());
    CHECK(rules_after.unwrap().keyword == "杀");
    CHECK(rules_after.unwrap().deck_provided);
    CHECK(rules_after.unwrap().options.deck == "/tmp/d");

    auto rules_deck_only = parse_command("rules --deck /tmp/d", base);
    REQUIRE(rules_deck_only.is_ok());
    CHECK(rules_deck_only.unwrap().keyword.empty());
    CHECK(rules_deck_only.unwrap().deck_provided);

    auto rules_unknown = parse_command("rules --bogus", base);
    REQUIRE(rules_unknown.is_err());
    CHECK(rules_unknown.unwrap_err().find("未知选项") != std::string::npos);

    auto rules_missing = parse_command("rules --deck", base);
    REQUIRE(rules_missing.is_err());
    CHECK(rules_missing.unwrap_err().find("需要一个值") != std::string::npos);

    auto audit = parse_command("audit --deck /tmp/d", base);
    REQUIRE(audit.is_ok());
    CHECK(audit.unwrap().kind == CommandKind::Audit);
    CHECK(audit.unwrap().deck_provided);
    CHECK(audit.unwrap().options.deck == "/tmp/d");

    auto audit_missing = parse_command("audit --deck", base);
    REQUIRE(audit_missing.is_err());
    CHECK(audit_missing.unwrap_err().find("需要一个值") != std::string::npos);

    // 不显式给 --deck 时不得置位，控制器才会沿用会话/启动来源。
    auto cards_plain = parse_command("cards", base);
    REQUIRE(cards_plain.is_ok());
    CHECK_FALSE(cards_plain.unwrap().deck_provided);
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
    CHECK(parse_command("w /tmp/a.json", base_options()).unwrap().kind ==
          CommandKind::Save);
    CHECK(parse_command("l /tmp/a.json", base_options()).unwrap().kind ==
          CommandKind::Load);

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

    auto alias_save = parse_command("w /tmp/a.json", base_options());
    REQUIRE(alias_save.is_ok());
    CHECK(alias_save.unwrap().file == "/tmp/a.json");

    auto alias_load = parse_command("l /tmp/a.json", base_options());
    REQUIRE(alias_load.is_ok());
    CHECK(alias_load.unwrap().file == "/tmp/a.json");

    auto missing = parse_command("save", base_options());
    REQUIRE(missing.is_err());
    CHECK(missing.unwrap_err().find("需要 <file>") != std::string::npos);

    auto too_many = parse_command("load a b", base_options());
    REQUIRE(too_many.is_err());
    CHECK(too_many.unwrap_err().find("只接受一个") != std::string::npos);
}
