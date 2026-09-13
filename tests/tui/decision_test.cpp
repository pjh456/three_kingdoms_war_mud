#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "card/card.hpp"
#include "card/catalog.hpp"
#include "card/def.hpp"
#include "config/resource.hpp"
#include "game/ai/decider.hpp"
#include "game/ai/simple.hpp"
#include "game/core/decision.hpp"
#include "tui/decision_source.hpp"

namespace
{
    tkw::card::CardDefCatalog load_catalog()
    {
        tkw::config::ResourceStore store(TKW_TEST_RESOURCE_DIR);
        auto r = tkw::card::CardDefCatalog::load(store, "deck");
        REQUIRE(r.is_ok());
        return std::move(r).unwrap();
    }

    tkw::game::ai::DecisionRequest base_request(
        tkw::game::ai::DecisionKind kind, const tkw::card::CardDefCatalog &catalog,
        const std::string &actor = "P0")
    {
        tkw::game::ai::DecisionRequest req;
        req.kind = kind;
        req.actor = actor;
        req.catalog = &catalog;
        return req;
    }

    tkw::card::Card card_of(const std::string &instance_id,
                            const std::string &def_id)
    {
        tkw::card::Card c;
        c.instance_id = instance_id;
        c.def_id = def_id;
        return c;
    }

    /** 在截止时间内等到一次可取的待决面板。 */
    bool wait_pending(tkw::tui::TuiDecisionSource &src,
                      tkw::tui::DecisionPanelView &out,
                      std::chrono::milliseconds budget = std::chrono::seconds(2))
    {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (src.fetch_new(out))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
}

using tkw::game::ai::DecisionChoice;
using tkw::game::ai::DecisionKind;
using tkw::tui::DecisionPanelView;
using tkw::tui::make_choice;
using tkw::tui::make_panel;
using tkw::tui::PanelOption;
using tkw::tui::TuiDecisionSource;

TEST_CASE("tui: play panel maps legal actions to choice payload")
{
    const auto catalog = load_catalog();
    auto req = base_request(DecisionKind::Play, catalog);
    tkw::game::LegalAction sha;
    sha.card = card_of("c1", "sha");
    sha.targets = {"P1"};
    tkw::game::LegalAction tao;
    tao.card = card_of("c2", "tao");
    req.legal = {sha, tao};

    const DecisionPanelView panel = make_panel(req);
    CHECK(panel.kind == DecisionKind::Play);
    CHECK(panel.title.find("出牌") != std::string::npos);
    CHECK(panel.allow_pass);
    REQUIRE(panel.options.size() == 2);
    CHECK(panel.options[0].text.find("杀") != std::string::npos);
    CHECK(panel.options[0].text.find("P1") != std::string::npos);
    CHECK(panel.options[0].targets == std::vector<std::string>({"P1"}));

    DecisionChoice out;
    REQUIRE(make_choice(panel, {0}, false, out));
    REQUIRE(out.instance_id.is_some());
    CHECK(out.instance_id.unwrap() == "c1");
    CHECK(out.targets == std::vector<std::string>({"P1"}));

    DecisionChoice passed;
    REQUIRE(make_choice(panel, {}, true, passed));
    CHECK(passed.instance_id.is_none());

    CHECK_FALSE(make_choice(panel, {5}, false, out));
}

TEST_CASE("tui: response panel maps pairs and single cards")
{
    const auto catalog = load_catalog();

    auto pair_req = base_request(DecisionKind::Response, catalog);
    pair_req.response_kind = tkw::card::ResponseKind::Sha;
    pair_req.response_source = "sha";
    pair_req.response_user = "P1";
    pair_req.response_damage = 1;
    tkw::game::LegalAction pair;
    pair.card = card_of("c1", "shan");
    pair.second_instance_id = "c2";
    pair_req.legal = {pair};

    const DecisionPanelView pair_panel = make_panel(pair_req);
    CHECK(pair_panel.title.find("响应") != std::string::npos);
    CHECK(pair_panel.title.find("1 点伤害") != std::string::npos);
    REQUIRE(pair_panel.options.size() == 1);
    CHECK(pair_panel.options[0].text.find("+") != std::string::npos);

    DecisionChoice pair_choice;
    REQUIRE(make_choice(pair_panel, {0}, false, pair_choice));
    REQUIRE(pair_choice.instance_id.is_some());
    CHECK(pair_choice.instance_id.unwrap() == "c1");
    CHECK(pair_choice.second_instance_id == "c2");

    auto single_req = base_request(DecisionKind::Response, catalog);
    single_req.response_kind = tkw::card::ResponseKind::Jink;
    single_req.options = {card_of("c3", "shan")};
    const DecisionPanelView single_panel = make_panel(single_req);
    REQUIRE(single_panel.options.size() == 1);
    DecisionChoice single_choice;
    REQUIRE(make_choice(single_panel, {0}, false, single_choice));
    REQUIRE(single_choice.instance_id.is_some());
    CHECK(single_choice.instance_id.unwrap() == "c3");
}

TEST_CASE("tui: peach and counter panels carry rescue and trick context")
{
    const auto catalog = load_catalog();

    auto peach_req = base_request(DecisionKind::Peach, catalog);
    peach_req.dying = "P1";
    peach_req.options = {card_of("c1", "tao")};
    const DecisionPanelView peach = make_panel(peach_req);
    CHECK(peach.title.find("濒死") != std::string::npos);
    CHECK(peach.title.find("P1") != std::string::npos);
    DecisionChoice peach_out;
    REQUIRE(make_choice(peach, {0}, false, peach_out));
    REQUIRE(peach_out.instance_id.is_some());
    CHECK(peach_out.instance_id.unwrap() == "c1");

    auto counter_req = base_request(DecisionKind::Counter, catalog);
    counter_req.counter_user = "P1";
    counter_req.counter_targets = {"P0"};
    counter_req.counter_trick = "juedou";
    counter_req.options = {card_of("c2", "wuxie")};
    const DecisionPanelView counter = make_panel(counter_req);
    CHECK(counter.title.find("无懈") != std::string::npos);
    CHECK(counter.title.find("决斗") != std::string::npos);
    CHECK(counter.title.find("P0") != std::string::npos);
    DecisionChoice counter_out;
    REQUIRE(make_choice(counter, {0}, false, counter_out));
    REQUIRE(counter_out.instance_id.is_some());
    CHECK(counter_out.instance_id.unwrap() == "c2");
}

TEST_CASE("tui: trigger panel is a yes no choice")
{
    const auto catalog = load_catalog();
    auto req = base_request(DecisionKind::Trigger, catalog);
    req.ability = tkw::card::Ability::NoShaLimit;

    const DecisionPanelView panel = make_panel(req);
    CHECK(panel.yes_no);
    CHECK(panel.allow_pass);
    REQUIRE(panel.options.size() == 2);
    CHECK(panel.options[0].accepted);
    CHECK_FALSE(panel.options[1].accepted);

    DecisionChoice yes;
    REQUIRE(make_choice(panel, {0}, false, yes));
    CHECK(yes.accepted);
    DecisionChoice no;
    REQUIRE(make_choice(panel, {1}, false, no));
    CHECK_FALSE(no.accepted);
    DecisionChoice passed;
    REQUIRE(make_choice(panel, {}, true, passed));
    CHECK_FALSE(passed.accepted);
    CHECK_FALSE(make_choice(panel, {2}, false, yes));
}

TEST_CASE("tui: pick card panel hides opponent hand and reveals equip")
{
    const auto catalog = load_catalog();
    auto req = base_request(DecisionKind::PickCard, catalog);
    req.target = "P1";
    req.options = {tkw::card::Card{}, card_of("e1", "bagua")};
    req.zone_labels = {tkw::card::Zone::Hand, tkw::card::Zone::Equip};

    const DecisionPanelView panel = make_panel(req);
    CHECK(panel.title.find("P1") != std::string::npos);
    REQUIRE(panel.options.size() == 2);
    CHECK(panel.options[0].text.find("未知手牌") != std::string::npos);
    CHECK(panel.options[0].text.find("[手]") != std::string::npos);
    CHECK(panel.options[0].text.find("八卦") == std::string::npos);
    CHECK(panel.options[1].text.find("[装]") != std::string::npos);
    CHECK(panel.options[1].text.find("八卦") != std::string::npos);
    CHECK(panel.allow_pass);

    DecisionChoice out;
    REQUIRE(make_choice(panel, {1}, false, out));
    REQUIRE(out.option_index.is_some());
    CHECK(out.option_index.unwrap() == 1);
}

TEST_CASE("tui: revealed pick panel is mandatory")
{
    const auto catalog = load_catalog();
    auto req = base_request(DecisionKind::PickRevealed, catalog);
    req.reveal_source = tkw::game::RevealSource::Wugu;
    req.options = {card_of("c1", "sha"), card_of("c2", "tao")};

    const DecisionPanelView panel = make_panel(req);
    CHECK(panel.title.find("五谷") != std::string::npos);
    CHECK_FALSE(panel.allow_pass);
    DecisionChoice out;
    CHECK_FALSE(make_choice(panel, {}, true, out));  // 强制选择不接受 pass
    REQUIRE(make_choice(panel, {1}, false, out));
    REQUIRE(out.option_index.is_some());
    CHECK(out.option_index.unwrap() == 1);
}

TEST_CASE("tui: discard panel enforces count uniqueness and reason pass")
{
    const auto catalog = load_catalog();
    auto req = base_request(DecisionKind::Discard, catalog);
    req.count = 2;
    req.discard_reason = tkw::game::DiscardReason::TurnLimit;
    req.options = {card_of("c1", "sha"), card_of("c2", "tao"),
                   card_of("c3", "shan")};

    const DecisionPanelView panel = make_panel(req);
    CHECK(panel.multi);
    CHECK(panel.toggle);
    CHECK(panel.need_count == 2);
    CHECK_FALSE(panel.allow_pass);

    DecisionChoice out;
    REQUIRE(make_choice(panel, {0, 2}, false, out));
    REQUIRE(out.discards.size() == 2);
    CHECK(out.discards[0] == "c1");
    CHECK(out.discards[1] == "c3");
    CHECK_FALSE(make_choice(panel, {0, 0}, false, out));
    CHECK_FALSE(make_choice(panel, {0}, false, out));
    CHECK_FALSE(make_choice(panel, {0, 9}, false, out));

    auto cixiong = req;
    cixiong.discard_reason = tkw::game::DiscardReason::CixiongChoice;
    cixiong.count = 1;
    const DecisionPanelView cixiong_panel = make_panel(cixiong);
    CHECK(cixiong_panel.allow_pass);
    DecisionChoice passed;
    REQUIRE(make_choice(cixiong_panel, {}, true, passed));
    CHECK(passed.discards.empty());
}

TEST_CASE("tui: ai actor routes to fallback without blocking")
{
    const auto catalog = load_catalog();
    TuiDecisionSource source({"P0"},
                             std::make_unique<tkw::game::ai::SimpleDecider>());
    auto req = base_request(DecisionKind::PickRevealed, catalog, "P1");
    req.options = {card_of("c1", "sha")};

    const DecisionChoice choice = source.decide(req);
    REQUIRE(choice.option_index.is_some());
    CHECK(choice.option_index.unwrap() == 0);
    CHECK_FALSE(source.has_pending());
}

TEST_CASE("tui: human actor waits for submit then returns choice")
{
    const auto catalog = load_catalog();
    TuiDecisionSource source({"P0"},
                             std::make_unique<tkw::game::ai::SimpleDecider>());
    auto req = base_request(DecisionKind::PickRevealed, catalog);
    req.options = {card_of("c1", "sha")};

    DecisionChoice result;
    std::jthread worker([&] { result = source.decide(req); });

    DecisionPanelView panel;
    REQUIRE(wait_pending(source, panel));
    CHECK(panel.actor == "P0");
    REQUIRE(source.submit({0}, false));
    worker.join();

    REQUIRE(result.option_index.is_some());
    CHECK(result.option_index.unwrap() == 0);
    CHECK_FALSE(source.has_pending());
    CHECK_FALSE(source.fetch_new(panel));
}

TEST_CASE("tui: invalid submit keeps the decision pending")
{
    const auto catalog = load_catalog();
    TuiDecisionSource source({"P0"},
                             std::make_unique<tkw::game::ai::SimpleDecider>());
    auto req = base_request(DecisionKind::PickRevealed, catalog);
    req.options = {card_of("c1", "sha")};

    DecisionChoice result;
    std::jthread worker([&] { result = source.decide(req); });

    DecisionPanelView panel;
    REQUIRE(wait_pending(source, panel));
    CHECK_FALSE(source.submit({9}, false));  // 越界：忽略且保持待决
    REQUIRE(source.submit({0}, false));
    worker.join();
    REQUIRE(result.option_index.is_some());
    CHECK(result.option_index.unwrap() == 0);
}

TEST_CASE("tui: second submit before wake rejected")
{
    const auto catalog = load_catalog();
    TuiDecisionSource source({"P0"},
                             std::make_unique<tkw::game::ai::SimpleDecider>());
    auto req = base_request(DecisionKind::PickRevealed, catalog);
    req.options = {card_of("c1", "sha")};

    DecisionChoice result;
    std::jthread worker([&] { result = source.decide(req); });

    DecisionPanelView panel;
    REQUIRE(wait_pending(source, panel));
    REQUIRE(source.submit({0}, false));      // 首次提交被接受
    CHECK_FALSE(source.submit({0}, false));  // worker 唤醒前二次提交被拒
    worker.join();

    REQUIRE(result.option_index.is_some());
    CHECK(result.option_index.unwrap() == 0);
    CHECK_FALSE(source.has_pending());
}

TEST_CASE("tui: cancel unblocks decide with default choice")
{
    const auto catalog = load_catalog();
    TuiDecisionSource source({"P0"},
                             std::make_unique<tkw::game::ai::SimpleDecider>());
    auto req = base_request(DecisionKind::PickRevealed, catalog);
    req.options = {card_of("c1", "sha")};

    DecisionChoice result;
    std::jthread worker([&] { result = source.decide(req); });

    DecisionPanelView panel;
    REQUIRE(wait_pending(source, panel));
    source.cancel();
    worker.join();

    CHECK(result.instance_id.is_none());
    CHECK(result.option_index.is_none());
    CHECK_FALSE(result.accepted);
    CHECK(result.discards.empty());
    CHECK_FALSE(source.has_pending());
    DecisionPanelView after;
    CHECK_FALSE(source.fetch_new(after));
    CHECK_FALSE(source.submit({0}, false));
}
