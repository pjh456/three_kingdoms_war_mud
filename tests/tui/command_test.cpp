#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "cli/session.hpp"
#include "game/core/roles.hpp"
#include "tui/command.hpp"

using tkw::tui::CommandKind;
using tkw::tui::CommandParseError;
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
    CHECK(missing.unwrap_err().detail.find("需要一个值") != std::string::npos);
}

TEST_CASE("tui: parse errors carry chinese hints")
{
    const auto base = base_options();

    auto unknown = parse_command("frobnicate", base);
    REQUIRE(unknown.is_err());
    CHECK(unknown.unwrap_err().kind == CommandParseError::Kind::UnknownCommand);
    CHECK(unknown.unwrap_err().detail.find("未知命令") != std::string::npos);

    auto bad_option = parse_command("new --bogus 1", base);
    REQUIRE(bad_option.is_err());
    CHECK(bad_option.unwrap_err().kind == CommandParseError::Kind::UnknownOption);
    CHECK(bad_option.unwrap_err().detail.find("未知选项") != std::string::npos);

    auto missing = parse_command("new --players", base);
    REQUIRE(missing.is_err());
    CHECK(missing.unwrap_err().kind == CommandParseError::Kind::MissingValue);
    CHECK(missing.unwrap_err().detail.find("需要一个值") != std::string::npos);

    auto out_of_range = parse_command("new --players 99", base);
    REQUIRE(out_of_range.is_err());
    CHECK(out_of_range.unwrap_err().kind ==
          CommandParseError::Kind::PlayerOutOfRange);
    CHECK(out_of_range.unwrap_err().detail.find("超出范围") != std::string::npos);

    auto bad_seed = parse_command("new --seed -1", base);
    REQUIRE(bad_seed.is_err());
    CHECK(bad_seed.unwrap_err().kind == CommandParseError::Kind::InvalidValue);
    CHECK(bad_seed.unwrap_err().detail.find("非负整数") != std::string::npos);

    auto empty = parse_command("   ", base);
    REQUIRE(empty.is_err());
    CHECK(empty.unwrap_err().kind == CommandParseError::Kind::EmptyCommand);
    CHECK(empty.unwrap_err().detail.find("空命令") != std::string::npos);
}

TEST_CASE("tui: simulate parses games players and inline options")
{
    const auto base = base_options();

    auto plain = parse_command("simulate 20", base);
    REQUIRE(plain.is_ok());
    const auto &p = plain.unwrap();
    CHECK(p.kind == CommandKind::Simulate);
    CHECK(p.games == 20);
    // 未给 --seed 时基种子固定为 1（对齐 CLI simulate），不继承启动 seed 7。
    CHECK(p.options.seed == 1);
    CHECK_FALSE(p.deck_provided);

    auto positional = parse_command("simulate 20 2", base);
    REQUIRE(positional.is_ok());
    CHECK(positional.unwrap().games == 20);
    CHECK(positional.unwrap().options.players == 2);

    auto full = parse_command(
        "simulate 20 --seed 7 --ai aggressive --mode identity "
        "--deck /tmp/d --hand 3",
        base);
    REQUIRE(full.is_ok());
    const auto &f = full.unwrap();
    CHECK(f.games == 20);
    CHECK(f.options.seed == 7);
    CHECK(f.options.ai == tkw::cli::AiLevel::Aggressive);
    CHECK(f.options.mode == tkw::game::GameMode::Identity);
    CHECK(f.options.hand == 3);
    CHECK(f.deck_provided);
    CHECK(f.options.deck == "/tmp/d");
}

TEST_CASE("tui: simulate parse errors carry chinese hints")
{
    const auto base = base_options();

    auto missing = parse_command("simulate", base);
    REQUIRE(missing.is_err());
    CHECK(missing.unwrap_err().kind == CommandParseError::Kind::MissingArgument);
    CHECK(missing.unwrap_err().detail.find("需要") != std::string::npos);

    auto zero = parse_command("simulate 0", base);
    REQUIRE(zero.is_err());
    CHECK(zero.unwrap_err().detail.find("正整数") != std::string::npos);

    auto bad_n = parse_command("simulate x", base);
    REQUIRE(bad_n.is_err());
    CHECK(bad_n.unwrap_err().detail.find("无效") != std::string::npos);

    auto players_range = parse_command("simulate 20 99", base);
    REQUIRE(players_range.is_err());
    CHECK(players_range.unwrap_err().detail.find("超出范围") != std::string::npos);

    auto bad_players = parse_command("simulate 20 x", base);
    REQUIRE(bad_players.is_err());
    CHECK(bad_players.unwrap_err().detail.find("无效") != std::string::npos);

    auto unknown = parse_command("simulate 20 --bogus", base);
    REQUIRE(unknown.is_err());
    CHECK(unknown.unwrap_err().detail.find("未知选项") != std::string::npos);

    auto extra = parse_command("simulate 20 2 3", base);
    REQUIRE(extra.is_err());
    CHECK(extra.unwrap_err().kind ==
          CommandParseError::Kind::UnexpectedArgument);
    CHECK(extra.unwrap_err().detail.find("两个位置参数") != std::string::npos);

    // 未知命令仍走「未知命令」。
    auto frob = parse_command("frobnicate", base);
    REQUIRE(frob.is_err());
    CHECK(frob.unwrap_err().detail.find("未知命令") != std::string::npos);
}

TEST_CASE("tui: unknown command suggests nearest command names")
{
    const auto base = base_options();

    auto typo = parse_command("runn", base);
    REQUIRE(typo.is_err());
    const std::string &hint = typo.unwrap_err().detail;
    CHECK(hint.find("未知命令") != std::string::npos);
    CHECK(hint.find("是否想输入") != std::string::npos);
    CHECK(hint.find("run") != std::string::npos);

    // 远距拼写无候选，保持原泛化提示。
    auto far = parse_command("frobnicate", base);
    REQUIRE(far.is_err());
    CHECK(far.unwrap_err().detail.find("是否想输入") == std::string::npos);
}

TEST_CASE("tui: command suggestions respect threshold and cap")
{
    CHECK(tkw::tui::detail::suggest_commands("frobnicate").empty());

    const auto typo = tkw::tui::detail::suggest_commands("runn");
    REQUIRE_FALSE(typo.empty());
    CHECK(typo.size() <= 3);
    CHECK(typo.front() == "run");
}

TEST_CASE("tui: help and ? accept at most one keyword")
{
    const auto base = base_options();

    auto help = parse_command("help", base);
    REQUIRE(help.is_ok());
    CHECK(help.unwrap().kind == CommandKind::Help);
    CHECK(help.unwrap().keyword.empty());

    auto help_new = parse_command("help new", base);
    REQUIRE(help_new.is_ok());
    CHECK(help_new.unwrap().kind == CommandKind::Help);
    CHECK(help_new.unwrap().keyword == "new");

    auto query = parse_command("? 牌", base);
    REQUIRE(query.is_ok());
    CHECK(query.unwrap().kind == CommandKind::Help);
    CHECK(query.unwrap().keyword == "牌");

    auto too_many = parse_command("help new run", base);
    REQUIRE(too_many.is_err());
    CHECK(too_many.unwrap_err().kind ==
          CommandParseError::Kind::TooManyArguments);
    CHECK(too_many.unwrap_err().detail.find("只接受一个") != std::string::npos);
}

TEST_CASE("tui: query_help_lines filters without rewriting the full table")
{
    const auto all = tkw::tui::detail::help_lines();
    const auto unfiltered = tkw::tui::detail::query_help_lines("");
    CHECK(unfiltered == all);

    const auto by_keyword = tkw::tui::detail::query_help_lines("牌");
    CHECK_FALSE(by_keyword.empty());
    for (const auto &line : by_keyword)
        CHECK(line.find("牌") != std::string::npos);
    CHECK(by_keyword.size() < all.size());

    const auto no_match = tkw::tui::detail::query_help_lines("不存在的关键词");
    REQUIRE(no_match.size() == 1);
    CHECK(no_match.front().find("没有匹配") != std::string::npos);
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
    CHECK(cards_bad.unwrap_err().detail.find("只接受") != std::string::npos);

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
    CHECK(rules_many.unwrap_err().detail.find("一个") != std::string::npos);

    auto audit = parse_command("audit", base);
    REQUIRE(audit.is_ok());
    CHECK(audit.unwrap().kind == CommandKind::Audit);

    auto audit_arg = parse_command("audit x", base);
    REQUIRE(audit_arg.is_err());
    CHECK(audit_arg.unwrap_err().detail.find("不接受参数") != std::string::npos);
}

TEST_CASE("tui: decks parses into a query command")
{
    const auto base = base_options();

    auto decks = parse_command("decks", base);
    REQUIRE(decks.is_ok());
    CHECK(decks.unwrap().kind == CommandKind::Decks);
    CHECK_FALSE(decks.unwrap().deck_provided);

    auto with_deck = parse_command("decks --deck /tmp/d", base);
    REQUIRE(with_deck.is_ok());
    CHECK(with_deck.unwrap().kind == CommandKind::Decks);
    CHECK(with_deck.unwrap().deck_provided);
    CHECK(with_deck.unwrap().options.deck == "/tmp/d");

    auto bad = parse_command("decks x", base);
    REQUIRE(bad.is_err());
    CHECK(bad.unwrap_err().detail.find("只接受") != std::string::npos);
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
    CHECK(cards_missing.unwrap_err().detail.find("需要一个值") != std::string::npos);

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
    CHECK(rules_unknown.unwrap_err().detail.find("未知选项") != std::string::npos);

    auto rules_missing = parse_command("rules --deck", base);
    REQUIRE(rules_missing.is_err());
    CHECK(rules_missing.unwrap_err().detail.find("需要一个值") != std::string::npos);

    auto audit = parse_command("audit --deck /tmp/d", base);
    REQUIRE(audit.is_ok());
    CHECK(audit.unwrap().kind == CommandKind::Audit);
    CHECK(audit.unwrap().deck_provided);
    CHECK(audit.unwrap().options.deck == "/tmp/d");

    auto audit_missing = parse_command("audit --deck", base);
    REQUIRE(audit_missing.is_err());
    CHECK(audit_missing.unwrap_err().detail.find("需要一个值") != std::string::npos);

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
    CHECK(missing.unwrap_err().detail.find("两个参数") != std::string::npos);
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
    CHECK(extra.unwrap_err().detail.find("不接受参数") != std::string::npos);
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
    CHECK(missing.unwrap_err().detail.find("需要 <file>") != std::string::npos);

    auto too_many = parse_command("load a b", base_options());
    REQUIRE(too_many.is_err());
    CHECK(too_many.unwrap_err().detail.find("只接受一个") != std::string::npos);
}

TEST_CASE("tui: new parses repeatable hero seats with replace semantics")
{
    auto base = base_options();

    auto single = parse_command("new --hero P0=zhangfei", base);
    REQUIRE(single.is_ok());
    CHECK(single.unwrap().kind == CommandKind::New);
    CHECK(single.unwrap().options.heroes ==
          std::vector<std::string>({"P0=zhangfei"}));

    auto repeated =
        parse_command("new --hero P0=zhangfei --hero P1=guanyu", base);
    REQUIRE(repeated.is_ok());
    CHECK(repeated.unwrap().options.heroes ==
          std::vector<std::string>({"P0=zhangfei", "P1=guanyu"}));

    // 行内 --hero 替换启动继承值（与 CLI 同语义），不与启动值累积。
    base.heroes = {"P2=zhouyu"};
    auto replaced = parse_command("new --hero P0=zhangfei", base);
    REQUIRE(replaced.is_ok());
    CHECK(replaced.unwrap().options.heroes ==
          std::vector<std::string>({"P0=zhangfei"}));

    // 未提供时保持启动继承值。
    auto inherited = parse_command("new", base);
    REQUIRE(inherited.is_ok());
    CHECK(inherited.unwrap().options.heroes ==
          std::vector<std::string>({"P2=zhouyu"}));

    auto missing = parse_command("new --hero", base);
    REQUIRE(missing.is_err());
    CHECK(missing.unwrap_err().detail.find("需要一个值") != std::string::npos);
}

TEST_CASE("tui: heroes parses into a query command")
{
    const auto base = base_options();

    auto plain = parse_command("heroes", base);
    REQUIRE(plain.is_ok());
    CHECK(plain.unwrap().kind == CommandKind::Heroes);
    CHECK_FALSE(plain.unwrap().deck_provided);

    auto with_deck = parse_command("heroes --deck /tmp/d", base);
    REQUIRE(with_deck.is_ok());
    CHECK(with_deck.unwrap().kind == CommandKind::Heroes);
    CHECK(with_deck.unwrap().deck_provided);
    CHECK(with_deck.unwrap().options.deck == "/tmp/d");

    auto missing = parse_command("heroes --deck", base);
    REQUIRE(missing.is_err());
    CHECK(missing.unwrap_err().detail.find("需要一个值") != std::string::npos);

    auto bad = parse_command("heroes x", base);
    REQUIRE(bad.is_err());
    CHECK(bad.unwrap_err().detail.find("只接受") != std::string::npos);
}

TEST_CASE("tui: help lists hero selection and the heroes query")
{
    const auto all = tkw::tui::detail::help_lines();
    std::string joined;
    for (const auto &line : all)
        joined += line + "\n";
    CHECK(joined.find("--hero") != std::string::npos);
    CHECK(joined.find("heroes") != std::string::npos);

    // 关键词过滤只挑选既有行：武将帮助命中且不重写全量表。
    const auto by_keyword = tkw::tui::detail::query_help_lines("武将");
    CHECK_FALSE(by_keyword.empty());
    for (const auto &line : by_keyword)
        CHECK(line.find("武将") != std::string::npos);
}
