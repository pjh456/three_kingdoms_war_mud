#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#include "game/ai/aggressive.hpp"
#include "game/ai/evaluator.hpp"
#include "game/ai/human.hpp"
#include "game/ai/legal.hpp"
#include "game/ai/simple.hpp"
#include "game/ai/view.hpp"
#include "io/file.hpp"
#include "test_game.hpp"

namespace
{
    using tkw::test::TestGame;
    using namespace tkw::game::ai;

    struct NoopDecider : Decider
    {
        DecisionChoice decide(const DecisionRequest &) override
        {
            return DecisionChoice{};
        }
    };

    struct RecordingDecider : Decider
    {
        DecisionKind last = DecisionKind::Play;
        std::size_t legal_count = 0;
        std::size_t pick_index = 0;     /**< 选中的 legal 动作下标（默认首个） */
        std::size_t option_index = 0;   /**< PickCard/PickRevealed 选中的候选下标 */
        std::string second_instance_id; /**< 最近一次透传的第二张手牌 */
        bool converted_sha = false;     /**< 最近一次透传的单张转化当杀标记 */
        std::vector<tkw::card::Card> options;     /**< 最近一次请求的候选牌 */
        std::vector<tkw::card::Zone> zone_labels; /**< 与 options 等长的来源分区 */

        DecisionChoice decide(const DecisionRequest &req) override
        {
            last = req.kind;
            legal_count = req.legal.size();
            options = req.options;
            zone_labels = req.zone_labels;
            DecisionChoice out;
            if (req.kind == DecisionKind::Play && req.legal.size() > pick_index)
            {
                const auto &act = req.legal[pick_index];
                out.instance_id = tkw::Option<std::string>::Some(
                    act.card.instance_id);
                out.targets = act.targets;
                out.second_instance_id = act.second_instance_id;
                out.recast = act.recast;
                out.converted_sha = act.converted_sha;
                second_instance_id = act.second_instance_id;
                converted_sha = act.converted_sha;
            }
            if (req.kind == DecisionKind::PickCard ||
                req.kind == DecisionKind::PickRevealed)
                out.option_index = tkw::Option<std::size_t>::Some(option_index);
            return out;
        }
    };
}

TEST_CASE("ai: view captures self and others")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 3);
    g.add_player("c", 2, 4);
    g.equip("b", "qinglong", "e#1");
    g.give("b", "sha", "s#1");
    g.give("a", "shan", "s#2");

    const auto v = make_view(g.ctx, "a");
    CHECK(v.self == "a");
    CHECK(v.self_seat == 0);
    CHECK(v.self_hp == 4);
    CHECK(v.hand.size() == 1);
    CHECK(v.equip.empty());
    CHECK(v.judge.empty());
    REQUIRE(v.others.size() == 2);
    CHECK(v.others[0].id == "b");
    CHECK(v.others[0].seat == 1);
    CHECK(v.others[0].hp == 3);
    CHECK(v.others[0].hand_size == 1);
    CHECK(v.others[0].has_weapon);
    CHECK(v.others[0].distance == 1);
    CHECK(v.others[0].in_attack_range);
    REQUIRE(v.others[0].equip.size() == 1);
    CHECK(v.others[0].equip[0].def_id == "qinglong");
    CHECK(v.others[0].judge.empty());
}

TEST_CASE("ai: view carries roles from the context")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Lord},
               {"b", tkw::game::Role::Rebel},
               {"c", tkw::game::Role::Loyalist}};

    const auto v = make_view(g.ctx, "a");
    CHECK(v.mode == tkw::game::GameMode::Identity);
    CHECK(v.self_role == tkw::game::Role::Lord);
    REQUIRE(v.others.size() == 2);
    CHECK(v.others[0].role == tkw::game::Role::Rebel);
    CHECK(v.others[1].role == tkw::game::Role::Loyalist);

    TestGame b("deck");
    b.add_player("a", 0, 4);
    b.add_player("b", 1, 4);
    const auto bv = make_view(b.ctx, "a");
    CHECK(bv.mode == tkw::game::GameMode::Brawl);
    CHECK(bv.self_role == tkw::game::Role::None);
    REQUIRE(bv.others.size() == 1);
    CHECK(bv.others[0].role == tkw::game::Role::None);
}

TEST_CASE("ai: evaluator scores cards")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);

    CHECK(card_value(*g.catalog.find("sha").unwrap()) > 0);
    CHECK(card_value(*g.catalog.find("tao").unwrap()) >
          card_value(*g.catalog.find("sha").unwrap()));
}

TEST_CASE("ai: simple choose_play only returns legal_actions")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");
    g.give("a", "tao", "t#1");

    tkw::game::SimpleAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto chosen = ai.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());

    const auto legal = tkw::game::legal_actions(g.ctx, "a", turn);
    bool found = false;
    for (const auto &a : legal)
        if (a.card.instance_id == chosen.unwrap().instance_id &&
            a.targets == chosen.unwrap().targets)
            found = true;
    CHECK(found);
}

TEST_CASE("ai: jiu is legal until used and only offered as self rescue")
{
    TestGame g("deck", 1, TKW_TEST_RESOURCE_DIR "/junzheng");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "jiu", "j#0");

    // 合法动作：本回合未用酒时产出，已用后不再产出
    CHECK(tkw::game::legal_actions(g.ctx, "a", tkw::game::TurnContext{"a", 0, 1, false})
              .size() == 1);
    CHECK(tkw::game::legal_actions(g.ctx, "a", tkw::game::TurnContext{"a", 0, 1, true})
              .empty());

    tkw::game::SimpleAI ai;
    // 濒死者本人持酒：进入救场候选（乱斗 AI 恒救）
    const auto self = ai.play_peach(g.ctx, "a", "a");
    REQUIRE(self.is_some());
    CHECK(self.unwrap() == "j#0");
    // 非本人 saver 持酒：酒不进入候选，无法救他人
    CHECK(ai.play_peach(g.ctx, "a", "b").is_none());
}

TEST_CASE("ai: recast candidates are not chosen proactively")
{
    TestGame g("deck", 1, TKW_TEST_RESOURCE_DIR "/junzheng");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "tiesuo", "t#0");

    const tkw::game::TurnContext turn{"a", 0, 1};
    tkw::game::SimpleAI simple;
    const auto s = simple.choose_play(g.ctx, turn);
    REQUIRE(s.is_some());
    CHECK_FALSE(s.unwrap().recast);
    CHECK_FALSE(s.unwrap().targets.empty());

    tkw::game::AggressiveAI aggressive;
    const auto ag = aggressive.choose_play(g.ctx, turn);
    REQUIRE(ag.is_some());
    CHECK_FALSE(ag.unwrap().recast);
    CHECK_FALSE(ag.unwrap().targets.empty());
}

TEST_CASE("ai: recast choice passes through the request source")
{
    TestGame g("deck", 1, TKW_TEST_RESOURCE_DIR "/junzheng");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "tiesuo", "t#0");

    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto legal = tkw::game::legal_actions(g.ctx, "a", turn);
    std::size_t recast_index = legal.size();
    for (std::size_t i = 0; i < legal.size(); ++i)
        if (legal[i].recast)
            recast_index = i;
    REQUIRE(recast_index < legal.size());

    RecordingDecider decider;
    decider.pick_index = recast_index;
    tkw::game::ai::RequestDecisionSource source(decider);
    const auto chosen = source.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().recast);
    CHECK(chosen.unwrap().targets.empty());
}

TEST_CASE("ai: recast candidate does not break single-target selection")
{
    namespace fs = std::filesystem;
    const fs::path dir =
        fs::temp_directory_path() / "tkw_ai_recast_single_target";
    std::error_code ec;
    fs::remove_all(dir, ec);
    REQUIRE(fs::create_directories(dir / "cards"));
    REQUIRE(tkw::io::write_text(
                dir / "deck.json",
                R"({"name":"recast_scope","cards":["zhadan"]})")
                .is_ok());
    REQUIRE(tkw::io::write_text(
                dir / "cards" / "zhadan.json",
                R"({"id":"zhadan","name":"炸弹","type":"basic",)"
                R"("effect":{"kind":"damage","amount":1,"scope":"one_other"},)"
                R"("recast":true,"copies":[{"suit":"spade","number":1}]})")
                .is_ok());

    TestGame g("deck", 1, dir.string().c_str());
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "zhadan", "z#0");

    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto legal = tkw::game::legal_actions(g.ctx, "a", turn);
    bool has_normal = false;
    bool has_recast = false;
    for (const auto &a : legal)
    {
        if (a.recast)
            has_recast = true;
        else
            has_normal = true;
    }
    // 单目标重铸卡：正常动作与空目标重铸候选同组，目标选择不得越界取 front
    CHECK(has_normal);
    CHECK(has_recast);

    tkw::game::SimpleAI simple;
    const auto s = simple.choose_play(g.ctx, turn);
    REQUIRE(s.is_some());
    CHECK_FALSE(s.unwrap().recast);
    CHECK(s.unwrap().targets == std::vector<std::string>{"b"});

    tkw::game::AggressiveAI aggressive;
    const auto ag = aggressive.choose_play(g.ctx, turn);
    REQUIRE(ag.is_some());
    CHECK_FALSE(ag.unwrap().recast);
    CHECK(ag.unwrap().targets == std::vector<std::string>{"b"});

    fs::remove_all(dir, ec);
}

TEST_CASE("ai: simple borrowed sword targets the lowest-hp victim")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.add_player("d", 3, 2);           // 最低体力，应被集火
    g.equip("b", "qinglong", "e#0");   // b 持武器，攻击范围 3 够到 c、d
    g.give("a", "jiedao", "j#0");

    tkw::game::SimpleAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto chosen = ai.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "j#0");
    CHECK(chosen.unwrap().targets == std::vector<std::string>{"b", "d"});
}

TEST_CASE("ai: identity lord camp targets the hostile not a lower-hp loyalist")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);  // 主公持杀
    g.add_player("b", 1, 1);  // 忠臣，体力最低但非敌意
    g.add_player("c", 2, 4);  // 反贼
    g.give("a", "sha", "s#1");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Lord},
               {"b", tkw::game::Role::Loyalist},
               {"c", tkw::game::Role::Rebel}};

    tkw::game::SimpleAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto chosen = ai.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().targets == std::vector<std::string>{"c"});
}

TEST_CASE("ai: identity rebel targets the lord before a lower-hp loyalist")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);  // 主公
    g.add_player("b", 1, 4);  // 反贼持杀
    g.add_player("c", 2, 1);  // 忠臣，体力最低
    g.give("b", "sha", "s#1");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Lord},
               {"b", tkw::game::Role::Rebel},
               {"c", tkw::game::Role::Loyalist}};

    tkw::game::SimpleAI ai;
    const tkw::game::TurnContext turn{"b", 0, 1};
    const auto chosen = ai.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().targets == std::vector<std::string>{"a"});
}

TEST_CASE("ai: identity traitor keeps the lowest-hp rule")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);  // 内奸持杀
    g.add_player("b", 1, 4);  // 主公
    g.add_player("c", 2, 1);  // 反贼，体力最低
    g.give("a", "sha", "s#1");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Traitor},
               {"b", tkw::game::Role::Lord},
               {"c", tkw::game::Role::Rebel}};

    tkw::game::SimpleAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto chosen = ai.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().targets == std::vector<std::string>{"c"});
}

TEST_CASE("ai: identity traitor strikes the loyalist before the lord")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);  // 内奸持杀
    g.add_player("b", 1, 1);  // 主公，体力最低
    g.add_player("c", 2, 4);  // 忠臣
    g.give("a", "sha", "s#1");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Traitor},
               {"b", tkw::game::Role::Lord},
               {"c", tkw::game::Role::Loyalist}};

    tkw::game::SimpleAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    // 忠臣尚存：主公进避让集，即使体力最低也不打，先清忠臣
    const auto chosen = ai.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().targets == std::vector<std::string>{"c"});
}

TEST_CASE("ai: identity traitor passes when only the lord is reachable")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);  // 内奸持杀
    g.add_player("b", 1, 1);  // 主公，唯一可达
    g.add_player("c", 2, 4);  // 忠臣装备 +1 马，超出攻击范围
    g.give("a", "sha", "s#1");
    g.equip("c", "dilu", "h#1");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Traitor},
               {"b", tkw::game::Role::Lord},
               {"c", tkw::game::Role::Loyalist}};

    tkw::game::SimpleAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    // 有害组唯一可达目标就是应避让的主公：整组跳过，内奸过牌而非冒险落刀
    CHECK(ai.choose_play(g.ctx, turn).is_none());
}

TEST_CASE("ai: identity traitor kills the lord once no other faction remains")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);  // 内奸持杀
    g.add_player("b", 1, 4);  // 主公，已是最后一名非内奸
    g.give("a", "sha", "s#1");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Traitor},
               {"b", tkw::game::Role::Lord}};

    tkw::game::SimpleAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    // 无反贼也无忠臣：落刀相位打开，直接打主公
    const auto chosen = ai.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().targets == std::vector<std::string>{"b"});
}

TEST_CASE("ai: brawl targeting stays lowest-hp free-for-all")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 1);
    g.add_player("c", 2, 4);
    g.give("a", "sha", "s#1");
    // 不设 mode/roles：默认乱斗，全体无角色

    tkw::game::SimpleAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto chosen = ai.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().targets == std::vector<std::string>{"b"});
}

TEST_CASE("ai: RequestDecisionSource forwards choices to decider")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    NoopDecider noop;
    RequestDecisionSource src(noop);
    const tkw::game::TurnContext turn{"a", 0, 1};
    CHECK(src.choose_play(g.ctx, turn).is_none());
    CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha, {}).is_none());
    CHECK(src.choose_discards(g.ctx, "a", 1, tkw::game::DiscardReason::TurnLimit)
              .empty());
    CHECK_FALSE(src.trigger_effect(g.ctx, "a", tkw::card::Ability::NoShaLimit));
}

TEST_CASE("ai: decider receives play request with legal moves")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    RecordingDecider rec;
    RequestDecisionSource src(rec);
    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto act = src.choose_play(g.ctx, turn);
    CHECK(rec.last == DecisionKind::Play);
    CHECK(rec.legal_count > 0);
    REQUIRE(act.is_some());
    CHECK(act.unwrap().instance_id == "s#1");
}

TEST_CASE("ai: decider forwards the zhangba second card")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");  // 无真杀：两张当杀 pair 进入候选

    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto legal = tkw::game::legal_actions(g.ctx, "a", turn);
    std::size_t pair_index = legal.size();
    for (std::size_t i = 0; i < legal.size(); ++i)
        if (!legal[i].second_instance_id.empty())
        {
            pair_index = i;
            break;
        }
    REQUIRE(pair_index < legal.size());

    RecordingDecider rec;
    rec.pick_index = pair_index;
    RequestDecisionSource src(rec);
    const auto act = src.choose_play(g.ctx, turn);
    REQUIRE(act.is_some());
    CHECK(act.unwrap().second_instance_id == legal[pair_index].second_instance_id);
    CHECK(act.unwrap().second_instance_id == "x#2");
    CHECK(rec.second_instance_id == "x#2");
}

TEST_CASE("ai: decider forwards the wusheng converted sha")
{
    TestGame g("deck");
    g.load_heroes();
    g.add_player("a", 0, 4, tkw::entity::Gender::Male, "guanyu");
    g.add_player("b", 1, 4);
    g.cards.add_to_hand("a", tkw::card::Card{"x#1", "tao", tkw::card::Suit::Heart, 3});  // 红牌可当杀

    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto legal = tkw::game::legal_actions(g.ctx, "a", turn);
    std::size_t index = legal.size();
    for (std::size_t i = 0; i < legal.size(); ++i)
        if (legal[i].converted_sha)
        {
            index = i;
            break;
        }
    REQUIRE(index < legal.size());

    RecordingDecider rec;
    rec.pick_index = index;
    RequestDecisionSource src(rec);
    const auto act = src.choose_play(g.ctx, turn);
    REQUIRE(act.is_some());
    CHECK(act.unwrap().converted_sha);
    CHECK(act.unwrap().second_instance_id.empty());
    CHECK(rec.converted_sha);
}

TEST_CASE("ai: response options include wusheng red cards")
{
    TestGame g("deck");
    g.load_heroes();
    g.add_player("a", 0, 4, tkw::entity::Gender::Male, "guanyu");
    g.add_player("b", 1, 4);
    g.cards.add_to_hand("a", tkw::card::Card{"x#1", "tao", tkw::card::Suit::Heart, 3});
    g.cards.add_to_hand("a", tkw::card::Card{"x#2", "shan", tkw::card::Suit::Spade, 4});  // 黑色牌不可转化

    RecordingDecider rec;
    RequestDecisionSource src(rec);
    const auto chosen = src.play_response(
        g.ctx, "a", tkw::card::ResponseKind::Sha, tkw::game::ResponsePrompt{});
    CHECK(chosen.is_none());

    bool saw_red = false;
    bool saw_black = false;
    for (const auto &c : rec.options)
    {
        if (c.instance_id == "x#1")
            saw_red = true;
        if (c.instance_id == "x#2")
            saw_black = true;
    }
    CHECK(saw_red);
    CHECK_FALSE(saw_black);

    // 无武将座位不追加红牌转化候选
    TestGame plain("deck");
    plain.add_player("a", 0, 4);
    plain.add_player("b", 1, 4);
    plain.cards.add_to_hand("a", tkw::card::Card{"x#1", "tao", tkw::card::Suit::Heart, 3});
    RecordingDecider rec2;
    RequestDecisionSource src2(rec2);
    const auto plain_chosen = src2.play_response(
        plain.ctx, "a", tkw::card::ResponseKind::Sha, tkw::game::ResponsePrompt{});
    CHECK(plain_chosen.is_none());
    CHECK(rec2.options.empty());
}

TEST_CASE("ai: simple trigger respects the discard cost")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    tkw::game::SimpleAI ai;
    CHECK_FALSE(ai.trigger_effect(
        g.ctx, "a", tkw::card::Ability::DiscardTwoForceDamage));
    g.give("a", "sha", "s#9");
    CHECK(ai.trigger_effect(
        g.ctx, "a", tkw::card::Ability::DiscardTwoForceDamage));
    CHECK(ai.trigger_effect(
        g.ctx, "a", tkw::card::Ability::ExtraShaAfterJink));
}

TEST_CASE("ai: human decider plays chosen legal action")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    std::istringstream in("play 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};

    const auto chosen = src.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "s#1");
    CHECK(out.str().find("s#1") != std::string::npos);
}

TEST_CASE("ai: human windows advertise help and card lookup exits")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");
    g.give("a", "shan", "j#2");

    {
        // 有候选的窗口 Prompt 同时暴露 ? 与 card <序号>
        std::istringstream in("card 1\npass\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK(src.choose_play(g.ctx, tkw::game::TurnContext{"a", 0, 1}).is_none());
        CHECK(out.str().find("? 看用法，card <序号> 看牌面") !=
              std::string::npos);
    }
    {
        // 非法输入的通用兜底句尾附 ? 出口
        std::istringstream in("foo\nplay 1\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK(src.choose_play(g.ctx, tkw::game::TurnContext{"a", 0, 1}).is_some());
        CHECK(out.str().find("（? 看用法）") != std::string::npos);
    }
    {
        // 触发确认无候选：只提示 ?，不宣称 card
        std::istringstream in("?\nn\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK_FALSE(
            src.trigger_effect(g.ctx, "a", tkw::card::Ability::NoShaLimit));
        CHECK(out.str().find("（? 看用法）") != std::string::npos);
        CHECK(out.str().find("card <序号>") == std::string::npos);
    }
}

TEST_CASE("ai: human warns when borrowed sword targets self")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.equip("b", "qinglong", "e#0");
    g.give("a", "jiedao", "j#0");

    std::istringstream in("pass\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const auto chosen =
        src.choose_play(g.ctx, tkw::game::TurnContext{"a", 0, 1});
    CHECK(chosen.is_none());  // pass 结束出牌阶段

    const std::string text = out.str();
    // 自指候选带固定告警
    CHECK(text.find("-> b,a（警告：b 将对你出杀") != std::string::npos);
    // 非自指候选不带告警
    CHECK(text.find("-> b,c（警告") == std::string::npos);
    // 整次渲染只出现一条告警
    CHECK(text.find("将对你出杀") == text.rfind("将对你出杀"));
}

TEST_CASE("ai: human decider renders the situation summary")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 3);
    g.give("a", "sha", "s#1");
    g.give("b", "tao", "t#9");
    g.equip("a", "bagua", "e#0");
    g.equip("b", "qinglong", "e#1");
    const auto lesi = g.catalog.find("lesi");
    REQUIRE(lesi.is_some());
    const auto &copy = lesi.unwrap()->copies[0];
    g.cards.add_to_judge(
        "a", tkw::card::Card{"d#1", "lesi", copy.suit, copy.number});

    std::istringstream in("pass\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};
    CHECK(src.choose_play(g.ctx, turn).is_none());

    const std::string text = out.str();
    // 己方：体力/完整手牌牌名/装备/判定逐项渲染
    CHECK(text.find("[a] 体力 4/4  手牌 杀  装备 八卦阵  判定 乐不思蜀") !=
          std::string::npos);
    // 对手：体力/手牌数/装备/距离逐项渲染
    CHECK(text.find("b 体力 3/3  手牌 1  装备 青龙偃月刀  距离 1") !=
          std::string::npos);
    CHECK(text.find("s#1") != std::string::npos);      // 候选仍渲染
    CHECK(text.find("t#9") == std::string::npos);      // 对手手牌内容不泄露
}

TEST_CASE("ai: human decider lists the full own hand including unplayable cards")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");
    g.give("a", "shan", "j#2");  // 闪仅在响应窗口可用，不应进合法出牌候选
    g.give("b", "tao", "t#9");

    std::istringstream in("pass\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};
    CHECK(src.choose_play(g.ctx, turn).is_none());

    const std::string text = out.str();
    // 完整手牌按手牌序展开；对手仍只给数量
    CHECK(text.find("[a] 体力 4/4  手牌 杀/闪  装备 无  判定 无") !=
          std::string::npos);
    CHECK(text.find("b 体力 4/4  手牌 1") != std::string::npos);
    CHECK(text.find("t#9") == std::string::npos);
}

TEST_CASE("ai: human decider prints candidate card text before playing")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "guohe", "g#1");
    g.give("b", "sha", "s#2");  // 目标区域有牌，过河拆桥才进候选

    // card 1 先打印过河拆桥卡文，再 play 1 正常出牌
    std::istringstream in("card 1\nplay 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};

    const auto chosen = src.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "g#1");
    CHECK(out.str().find("过河拆桥：出牌阶段") != std::string::npos);
    CHECK(out.str().find("弃置其区域内的一张牌") != std::string::npos);
    CHECK(out.str().find("输入无效") == std::string::npos);
}

TEST_CASE("ai: human decider prints candidate card text in response and discard")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "shan", "j#1");
    g.give("a", "sha", "s#2");

    {
        // 响应窗口：card 1 打印闪的卡文后放弃
        std::istringstream in("card 1\npass\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Jink, {})
                  .is_none());
        CHECK(out.str().find("当你成为「杀」或「万箭齐发」的目标时") !=
              std::string::npos);
    }
    {
        // 弃牌窗口：card 1 打印候选卡文后再弃牌
        std::istringstream in("card 1\ndiscard 1 2\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        const auto chosen = src.choose_discards(
            g.ctx, "a", 2, tkw::game::DiscardReason::TurnLimit);
        REQUIRE(chosen.size() == 2);
        CHECK(chosen[0] == "j#1");
        CHECK(chosen[1] == "s#2");
        CHECK(out.str().find("闪：当你成为") != std::string::npos);
    }
}

TEST_CASE("ai: human decider reprompts on an out-of-range card lookup")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    std::istringstream in("card 9\nplay 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};

    const auto chosen = src.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "s#1");
    CHECK(out.str().find("输入无效：序号需为 1-") != std::string::npos);
}

TEST_CASE("ai: human decider plays the zhangba two-card action")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");

    // legal 顺序：1) 无中生有 2) 丈八两张当杀（满体力桃不列为候选）
    std::istringstream in("play 2\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};

    const auto chosen = src.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "x#1");
    CHECK(chosen.unwrap().second_instance_id == "x#2");
    CHECK(chosen.unwrap().targets == std::vector<std::string>{"b"});
    CHECK(out.str().find("x#2") != std::string::npos);  // 渲染了第二张牌
}

TEST_CASE("ai: simple ai answers a sha window with the zhangba pair")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");  // 无真杀：两张当杀

    SimpleAI ai;
    const auto chosen =
        ai.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha, {});
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "x#1");
    CHECK(chosen.unwrap().second_instance_id == "x#2");

    // 手牌有真杀：真杀优先，不出两张当杀
    g.give("a", "sha", "s#1");
    const auto again =
        ai.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha, {});
    REQUIRE(again.is_some());
    CHECK(again.unwrap().instance_id == "s#1");
    CHECK(again.unwrap().second_instance_id.empty());
}

TEST_CASE("ai: human decider answers a sha with a card pair")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");

    std::istringstream in("play 1 + 2\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto chosen =
        src.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha, {});
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "x#1");
    CHECK(chosen.unwrap().second_instance_id == "x#2");
    CHECK(out.str().find("x#2") != std::string::npos);  // 渲染了手牌列表
}

TEST_CASE("ai: human decider reprompts on a single index in the pair response")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");

    std::istringstream in("play 1\nplay 1 + 2\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto chosen =
        src.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha, {});
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "x#1");
    CHECK(chosen.unwrap().second_instance_id == "x#2");
    CHECK(out.str().find("输入无效") != std::string::npos);
}

TEST_CASE("ai: human decider declines the zhangba pair response")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");

    std::istringstream in("pass\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    CHECK(src.play_response(
                g.ctx, "a", tkw::card::ResponseKind::Sha, {})
              .is_none());
}

TEST_CASE("ai: human decider treats eof as decline in the pair response")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("a", "zhangba", "e#0");
    g.give("a", "wuzhong", "x#1");
    g.give("a", "tao", "x#2");

    std::istringstream in;  // 空流：首次读取即 EOF
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    CHECK(src.play_response(
                g.ctx, "a", tkw::card::ResponseKind::Sha, {})
              .is_none());
}

TEST_CASE("ai: human decider declines response")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "shan", "j#1");

    std::istringstream in("pass\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Jink, {}).is_none());
}

TEST_CASE("ai: human decider names the required response card")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");
    g.give("a", "shan", "j#1");

    {
        std::istringstream in("pass\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha, {})
                  .is_none());
        CHECK(out.str().find("需打出杀") != std::string::npos);
    }
    {
        std::istringstream in("pass\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Jink, {})
                  .is_none());
        CHECK(out.str().find("需打出闪") != std::string::npos);
    }
}

TEST_CASE("ai: human decider picks discards by indices")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");
    g.give("a", "shan", "j#2");
    g.give("a", "tao", "t#3");

    std::istringstream in("discard 1 1\ndiscard 1\ndiscard 1 3\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto chosen = src.choose_discards(
        g.ctx, "a", 2, tkw::game::DiscardReason::TurnLimit);
    REQUIRE(chosen.size() == 2);
    CHECK(chosen[0] == "s#1");
    CHECK(chosen[1] == "t#3");
    CHECK(out.str().find("输入无效") != std::string::npos);
    // 弃牌提示与非法原因统一用「序号」，重复选择给出专门原因
    CHECK(out.str().find("下标") == std::string::npos);
    CHECK(out.str().find("序号不能重复") != std::string::npos);
}

TEST_CASE("ai: human decider may pass the cixiong discard choice")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    {
        std::istringstream in("pass\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        const auto chosen = src.choose_discards(
            g.ctx, "a", 1, tkw::game::DiscardReason::CixiongChoice);
        CHECK(chosen.empty());
        CHECK(out.str().find("雌雄双股剑（可放弃）") != std::string::npos);
        CHECK(out.str().find("pass") != std::string::npos);
    }
    {
        // 其他弃牌原因不接受 pass：重提示后按序号弃牌
        std::istringstream in("pass\ndiscard 1\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        const auto chosen = src.choose_discards(
            g.ctx, "a", 1, tkw::game::DiscardReason::TurnLimit);
        REQUIRE(chosen.size() == 1);
        CHECK(chosen[0] == "s#1");
        CHECK(out.str().find("输入无效") != std::string::npos);
    }
}

TEST_CASE("ai: human decider reprompts on invalid input")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    std::istringstream in("foo\nplay 99\nplay 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};

    const auto chosen = src.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "s#1");

    std::size_t reprompts = 0;
    const std::string text = out.str();
    for (std::size_t pos = text.find("输入无效"); pos != std::string::npos;
         pos = text.find("输入无效", pos + 1))
        ++reprompts;
    CHECK(reprompts == 2);
    // 非法输入给出具体原因：动词/数量不对走用法提示，序号越界走范围提示
    CHECK(text.find("输入无效：请输入 play <序号> 或 pass。") !=
          std::string::npos);
    CHECK(text.find("输入无效：序号需为 1-") != std::string::npos);
}

TEST_CASE("ai: human decider silently reprompts on a blank line")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    std::istringstream in("\n   \nplay 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};

    const auto chosen = src.choose_play(g.ctx, turn);
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap().instance_id == "s#1");
    CHECK(out.str().find("输入无效") == std::string::npos);
}

TEST_CASE("ai: human decider trigger reads yes and no")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);

    {
        std::istringstream in("y\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK(src.trigger_effect(g.ctx, "a", tkw::card::Ability::NoShaLimit));
    }
    {
        std::istringstream in("n\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK_FALSE(
            src.trigger_effect(g.ctx, "a", tkw::card::Ability::NoShaLimit));
    }
    {
        std::istringstream in("maybe\ny\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK(src.trigger_effect(g.ctx, "a", tkw::card::Ability::NoShaLimit));
    }
}

TEST_CASE("ai: human decider treats eof as decline")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");
    g.give("a", "shan", "j#1");
    g.give("a", "tao", "t#1");
    g.give("b", "sha", "s#2");

    std::istringstream in;
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};

    CHECK(src.choose_play(g.ctx, turn).is_none());
    CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Sha, {}).is_none());
    CHECK(src.play_response(g.ctx, "a", tkw::card::ResponseKind::Jink, {}).is_none());
    CHECK(src.play_peach(g.ctx, "a", "b").is_none());
    CHECK(src.play_counter(g.ctx, "a", "", {}, "").is_none());
    CHECK_FALSE(src.trigger_effect(g.ctx, "a", tkw::card::Ability::NoShaLimit));
    CHECK(src.pick_card_from_target(
              g.ctx, "a", "b", tkw::game::PickCardScope::HandEquipJudge)
              .is_none());
    const auto revealed = g.ctx.cards->hand("a");
    CHECK(src.pick_from_revealed(
              g.ctx, "a", revealed, tkw::game::RevealSource::Wugu)
              .is_none());
    CHECK(src.choose_discards(
                  g.ctx, "a", 1, tkw::game::DiscardReason::TurnLimit)
              .empty());
}

TEST_CASE("ai: human decider picks the hidden hand slot")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("b", "sha", "s#2");

    std::istringstream in("pick 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto picked = src.pick_card_from_target(
        g.ctx, "a", "b", tkw::game::PickCardScope::HandEquipJudge);
    REQUIRE(picked.is_some());
    CHECK(picked.unwrap().zone == tkw::card::Zone::Hand);
    CHECK(picked.unwrap().card.is_none());  // 隐藏手牌身份不由决策源回传
}

TEST_CASE("ai: pick_card_from_target filters the judgement zone by scope")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("b", "sha", "s#2");
    g.equip("b", "qinglong", "e#1");
    const auto lesi = g.catalog.find("lesi");
    REQUIRE(lesi.is_some());
    const auto &copy = lesi.unwrap()->copies[0];
    g.cards.add_to_judge(
        "b", tkw::card::Card{"d#1", "lesi", copy.suit, copy.number});

    RecordingDecider dec;
    RequestDecisionSource src(dec);

    // 寒冰剑范围：判定区不进候选，手牌/装备相对序不变；手牌为无身份占位槽
    dec.option_index = 0;
    const auto hand = src.pick_card_from_target(
        g.ctx, "a", "b", tkw::game::PickCardScope::HandEquip);
    REQUIRE(hand.is_some());
    CHECK(hand.unwrap().zone == tkw::card::Zone::Hand);
    CHECK(hand.unwrap().card.is_none());  // 脱敏：不回传对手手牌身份
    REQUIRE(dec.zone_labels.size() == 2);
    CHECK(dec.zone_labels[0] == tkw::card::Zone::Hand);
    CHECK(dec.zone_labels[1] == tkw::card::Zone::Equip);
    REQUIRE(dec.options.size() == 2);
    CHECK(dec.options[0].def_id.empty());
    CHECK(dec.options[0].instance_id.empty());
    CHECK(dec.options[1].instance_id == "e#1");

    // 明置装备槽仍按真实牌回传身份（槽位映射不受脱敏影响）
    dec.option_index = 1;
    const auto equip = src.pick_card_from_target(
        g.ctx, "a", "b", tkw::game::PickCardScope::HandEquip);
    REQUIRE(equip.is_some());
    CHECK(equip.unwrap().zone == tkw::card::Zone::Equip);
    REQUIRE(equip.unwrap().card.is_some());
    CHECK(equip.unwrap().card.unwrap().instance_id == "e#1");

    // 顺手牵羊/过河拆桥范围：判定区照常进入候选，槽位映射到真实牌
    dec.option_index = 2;
    const auto judge = src.pick_card_from_target(
        g.ctx, "a", "b", tkw::game::PickCardScope::HandEquipJudge);
    REQUIRE(judge.is_some());
    CHECK(judge.unwrap().zone == tkw::card::Zone::Judge);
    REQUIRE(judge.unwrap().card.is_some());
    CHECK(judge.unwrap().card.unwrap().instance_id == "d#1");
    REQUIRE(dec.zone_labels.size() == 3);
    CHECK(dec.zone_labels[2] == tkw::card::Zone::Judge);
    REQUIRE(dec.options.size() == 3);
    CHECK(dec.options[2].instance_id == "d#1");

    // 槽位越界：适配器返回 None，不产生越界访问
    dec.option_index = 99;
    CHECK(src.pick_card_from_target(
              g.ctx, "a", "b", tkw::game::PickCardScope::HandEquip)
              .is_none());
}

TEST_CASE("ai: human decider labels the target card zone")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("b", "sha", "s#2");
    g.equip("b", "qinglong", "e#1");
    const auto lesi = g.catalog.find("lesi");
    REQUIRE(lesi.is_some());
    const auto &copy = lesi.unwrap()->copies[0];
    g.cards.add_to_judge(
        "b", tkw::card::Card{"d#1", "lesi", copy.suit, copy.number});

    // 首槽 = 对手手牌：只渲染分区标签与遮挡占位，落子仍是真实牌
    {
        std::istringstream in("pick 1\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        const auto picked = src.pick_card_from_target(
            g.ctx, "a", "b", tkw::game::PickCardScope::HandEquipJudge);
        REQUIRE(picked.is_some());
        CHECK(picked.unwrap().zone == tkw::card::Zone::Hand);
        CHECK(picked.unwrap().card.is_none());  // 隐藏手牌落子由引擎暗抽

        const std::string text = out.str();
        CHECK(text.find("[手] （未知手牌）") != std::string::npos);
        CHECK(text.find("s#2") == std::string::npos);
        CHECK(text.find("杀") == std::string::npos);
        // 装备/判定区明置，照常显示
        CHECK(text.find("[装] 青龙偃月刀 e#1") != std::string::npos);
        CHECK(text.find("[判] 乐不思蜀 d#1") != std::string::npos);
    }
    // 遮挡不破坏跨区索引：第 2/3 槽仍映射到装备/判定区的真实牌
    {
        std::istringstream in("pick 2\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        const auto picked = src.pick_card_from_target(
            g.ctx, "a", "b", tkw::game::PickCardScope::HandEquipJudge);
        REQUIRE(picked.is_some());
        CHECK(picked.unwrap().zone == tkw::card::Zone::Equip);
        REQUIRE(picked.unwrap().card.is_some());
        CHECK(picked.unwrap().card.unwrap().instance_id == "e#1");
    }
    {
        std::istringstream in("pick 3\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        const auto picked = src.pick_card_from_target(
            g.ctx, "a", "b", tkw::game::PickCardScope::HandEquipJudge);
        REQUIRE(picked.is_some());
        CHECK(picked.unwrap().zone == tkw::card::Zone::Judge);
        REQUIRE(picked.unwrap().card.is_some());
        CHECK(picked.unwrap().card.unwrap().instance_id == "d#1");
    }
}

TEST_CASE("ai: human decider hides target hand card text on card lookup")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("b", "sha", "s#2");
    g.equip("b", "qinglong", "e#1");

    // card 1 命中对手手牌：只出遮挡提示，不打印该牌效果文案
    std::istringstream in("card 1\npick 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto picked = src.pick_card_from_target(
        g.ctx, "a", "b", tkw::game::PickCardScope::HandEquipJudge);
    REQUIRE(picked.is_some());
    CHECK(picked.unwrap().card.is_none());  // 隐藏手牌不回传身份

    const std::string text = out.str();
    CHECK(text.find("无法查看") != std::string::npos);
    CHECK(text.find("出牌阶段限一次") == std::string::npos);
    CHECK(text.find("s#2") == std::string::npos);
}

TEST_CASE("ai: human decider still shows revealed zone card text")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("b", "sha", "s#2");
    g.equip("b", "qinglong", "e#1");

    // card 2 命中明置装备区：照常打印装备效果文案与实例号
    std::istringstream in("card 2\npick 2\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto picked = src.pick_card_from_target(
        g.ctx, "a", "b", tkw::game::PickCardScope::HandEquipJudge);
    REQUIRE(picked.is_some());
    REQUIRE(picked.unwrap().card.is_some());
    CHECK(picked.unwrap().card.unwrap().instance_id == "e#1");

    const std::string text = out.str();
    CHECK(text.find("再使用一张") != std::string::npos);
    CHECK(text.find("e#1") != std::string::npos);
}

TEST_CASE("ai: routed ai sends human actor to human and falls back")
{
    TestGame g("deck");
    auto *a = g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    a->take_damage("b", 1, false);  // 受伤：桃成为合法候选，真人可显式选杀
    g.give("a", "tao", "t#1");
    g.give("a", "sha", "s#1");
    g.give("b", "sha", "s#2");

    std::istringstream in("play 2\n");
    std::ostringstream out;
    RoutedAI routed({"a"}, in, out);

    const tkw::game::TurnContext ta{"a", 0, 1};
    const auto a_choice = routed.choose_play(g.ctx, ta);
    REQUIRE(a_choice.is_some());
    CHECK(a_choice.unwrap().instance_id == "s#1");

    const tkw::game::TurnContext tb{"b", 0, 1};
    const auto b_choice = routed.choose_play(g.ctx, tb);
    REQUIRE(b_choice.is_some());
    CHECK(b_choice.unwrap().instance_id == "s#2");
}

TEST_CASE("ai: routed ai falls back for non-human response window")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "shan", "j#1");
    g.give("b", "shan", "j#2");

    std::istringstream in("play 1\n");
    std::ostringstream out;
    RoutedAI routed({"a"}, in, out);

    const auto a_card =
        routed.play_response(g.ctx, "a", tkw::card::ResponseKind::Jink, {});
    REQUIRE(a_card.is_some());
    CHECK(a_card.unwrap().instance_id == "j#1");

    // b 非真人：SimpleAI 取首张闪，不消费输入流（输入已被 a 读空）。
    const auto b_card =
        routed.play_response(g.ctx, "b", tkw::card::ResponseKind::Jink, {});
    REQUIRE(b_card.is_some());
    CHECK(b_card.unwrap().instance_id == "j#2");
}

TEST_CASE("ai: simple discard prefers the lowest value cards")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "tao", "t#1");    // 桃 50
    g.give("a", "shan", "j#2");  // 闪 35
    g.give("a", "sha", "s#3");   // 杀 40
    g.give("a", "wuxie", "w#4");  // 无懈可击 55

    SimpleAI ai;
    const auto chosen = ai.choose_discards(
        g.ctx, "a", 2, tkw::game::DiscardReason::TurnLimit);
    REQUIRE(chosen.size() == 2);
    CHECK(chosen[0] == "j#2");  // 价值最低的先弃
    CHECK(chosen[1] == "s#3");
}

TEST_CASE("ai: simple discard keeps hand order for equal values")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "shan", "j#1");  // 闪 35
    g.give("a", "sha", "s#2");   // 杀 40
    g.give("a", "shan", "j#3");  // 闪 35

    SimpleAI ai;
    const auto chosen = ai.choose_discards(
        g.ctx, "a", 2, tkw::game::DiscardReason::TurnLimit);
    REQUIRE(chosen.size() == 2);
    CHECK(chosen[0] == "j#1");  // 同价值保持手牌序
    CHECK(chosen[1] == "j#3");
}

TEST_CASE("ai: simple discard orders the whole hand by value")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "tao", "t#1");
    g.give("a", "shan", "j#2");
    g.give("a", "sha", "s#3");
    g.give("a", "wuxie", "w#4");

    SimpleAI ai;
    const auto chosen = ai.choose_discards(
        g.ctx, "a", 4, tkw::game::DiscardReason::TurnLimit);
    REQUIRE(chosen.size() == 4);
    CHECK(chosen[0] == "j#2");  // 35
    CHECK(chosen[1] == "s#3");  // 40
    CHECK(chosen[2] == "t#1");  // 50
    CHECK(chosen[3] == "w#4");  // 55
}

TEST_CASE("ai: simple counter plays against an enemy trick targeting self")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("b", "wuxie", "w#0");

    SimpleAI ai;
    const auto chosen = ai.play_counter(g.ctx, "b", "a", {"b"}, "");
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap() == "w#0");
}

TEST_CASE("ai: simple counter declines own trick")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "wuxie", "w#0");

    SimpleAI ai;
    CHECK(ai.play_counter(g.ctx, "a", "a", {"b"}, "").is_none());  // 冲别人的
    CHECK(ai.play_counter(g.ctx, "a", "a", {"a"}, "").is_none());  // 自益
}

TEST_CASE("ai: simple counter declines a third party's trick")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.add_player("d", 3, 4);
    g.give("b", "wuxie", "w#2");
    g.give("c", "wuxie", "w#0");
    g.give("d", "wuxie", "w#3");

    SimpleAI ai;
    // a 的锦囊冲 c：旁观者 b/d 持无懈也不出（省牌），目标本人 c 出
    CHECK(ai.play_counter(g.ctx, "b", "a", {"c"}, "").is_none());
    CHECK(ai.play_counter(g.ctx, "d", "a", {"c"}, "").is_none());
    const auto chosen = ai.play_counter(g.ctx, "c", "a", {"c"}, "");
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap() == "w#0");
}

TEST_CASE("ai: simple counter plays on a delayed trick judged on self")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "wuxie", "w#1");
    g.give("b", "wuxie", "w#0");

    SimpleAI ai;
    // 判定窗口：使用者空串哨兵，目标 = 被判定玩家
    const auto chosen = ai.play_counter(g.ctx, "b", "", {"b"}, "");
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap() == "w#0");
    // 第三方持无懈也不救
    CHECK(ai.play_counter(g.ctx, "a", "", {"b"}, "").is_none());
}

TEST_CASE("ai: identity counter protects self from an enemy trick")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.give("a", "wuxie", "w#0");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Lord},
               {"b", tkw::game::Role::Loyalist},
               {"c", tkw::game::Role::Rebel}};

    tkw::game::SimpleAI ai;
    const auto chosen = ai.play_counter(g.ctx, "a", "c", {"a"}, "");
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap() == "w#0");
}

TEST_CASE("ai: identity counter protects the first ally in window order")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.give("a", "wuxie", "w#a");
    g.give("b", "wuxie", "w#b");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Lord},
               {"b", tkw::game::Role::Loyalist},
               {"c", tkw::game::Role::Rebel}};

    tkw::game::SimpleAI ai;
    // 窗口序 c→a→b：a 是 b 的首位保护者，出无懈；目标本人 b 不重复出
    const auto first = ai.play_counter(g.ctx, "a", "c", {"b"}, "");
    REQUIRE(first.is_some());
    CHECK(first.unwrap() == "w#a");
    CHECK(ai.play_counter(g.ctx, "b", "c", {"b"}, "").is_none());
}

TEST_CASE("ai: identity counter declines an ally trick targeting self")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "wuxie", "w#0");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Lord},
               {"b", tkw::game::Role::Loyalist}};

    tkw::game::SimpleAI ai;
    // 友方对我用锦囊不自我抵消（旧口径锦囊冲自己就出）
    CHECK(ai.play_counter(g.ctx, "a", "b", {"a"}, "").is_none());
}

TEST_CASE("ai: identity counter handles the judgement window by camp")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.give("a", "wuxie", "w#a");
    g.give("b", "wuxie", "w#b");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Lord},
               {"b", tkw::game::Role::Loyalist},
               {"c", tkw::game::Role::Rebel}};

    tkw::game::SimpleAI ai;
    // 判定窗口（使用者空串哨兵）：被判定者 b 自己出；第三方 a 不代出
    const auto judged = ai.play_counter(g.ctx, "b", "", {"b"}, "shandian");
    REQUIRE(judged.is_some());
    CHECK(judged.unwrap() == "w#b");
    CHECK(ai.play_counter(g.ctx, "a", "", {"b"}, "shandian").is_none());
}

TEST_CASE("ai: identity counter nullifies an enemy self-benefit trick")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.give("a", "wuxie", "w#0");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Lord},
               {"b", tkw::game::Role::Loyalist},
               {"c", tkw::game::Role::Rebel}};

    tkw::game::SimpleAI ai;
    // 敌方自益锦囊窗口目标 = 使用者本人：主公视反贼为敌，本窗尚未被抵消
    // （已出 0 张）故补一张无懈
    const auto chosen = ai.play_counter(g.ctx, "a", "c", {"c"}, "wuzhong");
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap() == "w#0");
}

TEST_CASE("ai: identity counter stops once the self-benefit window is cancelled")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.give("a", "wuxie", "w#0");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Lord},
               {"b", tkw::game::Role::Loyalist},
               {"c", tkw::game::Role::Rebel}};

    tkw::game::SimpleAI ai;
    // 本窗已打出 1 张（奇数 = 已被抵消）：敌方不再补牌，避免偶数相抵
    CHECK(ai.play_counter(g.ctx, "a", "c", {"c"}, "wuzhong", 1).is_none());
}

TEST_CASE("ai: identity counter declines an ally self-benefit trick")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "wuxie", "w#0");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Lord},
               {"b", tkw::game::Role::Loyalist}};

    tkw::game::SimpleAI ai;
    // 友方 b 的自益锦囊（窗口目标 = b）：抵消等于拆自家台，故不出
    CHECK(ai.play_counter(g.ctx, "a", "b", {"b"}, "wuzhong").is_none());
}

TEST_CASE("ai: identity counter weighs a traitor self-benefit trick by camp")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.add_player("d", 3, 4);
    g.give("a", "wuxie", "w#a");
    g.give("d", "wuxie", "w#d");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Lord},
               {"b", tkw::game::Role::Loyalist},
               {"c", tkw::game::Role::Traitor},
               {"d", tkw::game::Role::Rebel}};

    tkw::game::SimpleAI ai;
    // 内奸自益窗：主公阵营视内奸为敌，补无懈；反贼视内奸为非敌，不出
    const auto lord = ai.play_counter(g.ctx, "a", "c", {"c"}, "wuzhong");
    REQUIRE(lord.is_some());
    CHECK(lord.unwrap() == "w#a");
    CHECK(ai.play_counter(g.ctx, "d", "c", {"c"}, "wuzhong").is_none());
}

TEST_CASE("ai: brawl counter stays legacy on a self-benefit trick")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.give("a", "wuxie", "w#0");

    tkw::game::SimpleAI ai;
    // 无角色回落乱斗旧口径：锦囊目标仅含使用者 c，a 不在目标内故不出
    CHECK(ai.play_counter(g.ctx, "a", "c", {"c"}, "wuzhong").is_none());
}

TEST_CASE("ai: identity counter declines an enemy beneficial group trick")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.give("a", "wuxie", "w#0");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Lord},
               {"b", tkw::game::Role::Loyalist},
               {"c", tkw::game::Role::Rebel}};

    tkw::game::SimpleAI ai;
    // 敌方桃园/五谷冲友方 b：抵消只会让己方少回血/少摸牌，故不出
    CHECK(ai.play_counter(g.ctx, "a", "c", {"b"}, "taoyuan").is_none());
    CHECK(ai.play_counter(g.ctx, "a", "c", {"b"}, "wugu").is_none());
}

TEST_CASE("ai: identity counter still nullifies an enemy aoe trick")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.give("a", "wuxie", "w#0");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Lord},
               {"b", tkw::game::Role::Loyalist},
               {"c", tkw::game::Role::Rebel}};

    tkw::game::SimpleAI ai;
    // 敌方万箭冲友方 b：伤害效果不受有益极性门影响，首位保护者仍出无懈
    const auto chosen = ai.play_counter(g.ctx, "a", "c", {"b"}, "wanjian");
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap() == "w#0");
}

TEST_CASE("ai: identity rescue saves only own camp")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "tao", "t#0");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Lord},
               {"b", tkw::game::Role::Rebel}};

    tkw::game::SimpleAI ai;
    CHECK(ai.play_peach(g.ctx, "a", "b").is_none());  // 不救反贼
    g.roles["b"] = tkw::game::Role::Loyalist;
    const auto saved = ai.play_peach(g.ctx, "a", "b");
    REQUIRE(saved.is_some());
    CHECK(saved.unwrap() == "t#0");
}

TEST_CASE("ai: identity rebel rescue spares the lord")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "tao", "t#0");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Rebel},
               {"b", tkw::game::Role::Lord}};

    tkw::game::SimpleAI ai;
    CHECK(ai.play_peach(g.ctx, "a", "b").is_none());  // 不救主公
    g.roles["b"] = tkw::game::Role::Rebel;
    CHECK(ai.play_peach(g.ctx, "a", "b").is_some());
}

TEST_CASE("ai: identity traitor saves the lord while rebels live")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.give("a", "tao", "t#0");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Traitor},
               {"b", tkw::game::Role::Lord},
               {"c", tkw::game::Role::Rebel}};

    tkw::game::SimpleAI ai;
    CHECK(ai.play_peach(g.ctx, "a", "b").is_some());  // 有反贼在：救主公制衡
    g.entities.remove("c");
    CHECK(ai.play_peach(g.ctx, "a", "b").is_none());  // 反贼尽灭：不救
}

TEST_CASE("ai: identity traitor saves the lord while any other faction lives")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.give("a", "tao", "t#0");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Traitor},
               {"b", tkw::game::Role::Lord},
               {"c", tkw::game::Role::Loyalist}};

    tkw::game::SimpleAI ai;
    CHECK(ai.play_peach(g.ctx, "a", "b").is_some());  // 忠臣尚存：救主公制衡
    g.entities.remove("c");
    CHECK(ai.play_peach(g.ctx, "a", "b").is_none());  // 主公已是最后非内奸：不救
}

TEST_CASE("ai: identity traitor rescues itself")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "tao", "t#0");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Traitor},
               {"b", tkw::game::Role::Lord}};

    tkw::game::SimpleAI ai;
    // 救自己不受落刀相位影响：任何相位内奸都应自救
    const auto saved = ai.play_peach(g.ctx, "a", "a");
    REQUIRE(saved.is_some());
    CHECK(saved.unwrap() == "t#0");
}

TEST_CASE("ai: identity traitor lets the lord die when solo")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "tao", "t#0");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Traitor},
               {"b", tkw::game::Role::Lord}};

    tkw::game::SimpleAI ai;
    // 主公是最后一名非内奸：见死不救，等他死后落刀收割
    CHECK(ai.play_peach(g.ctx, "a", "b").is_none());
}

TEST_CASE("ai: brawl rescue keeps first card")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "tao", "t#0");

    tkw::game::SimpleAI ai;
    const auto saved = ai.play_peach(g.ctx, "a", "b");
    REQUIRE(saved.is_some());
    CHECK(saved.unwrap() == "t#0");
}

TEST_CASE("ai: identity rebel passes when only allies are reachable")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Rebel},
               {"b", tkw::game::Role::Rebel}};

    tkw::game::SimpleAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    CHECK(ai.choose_play(g.ctx, turn).is_none());
}

TEST_CASE("ai: identity lord camp passes when only loyalists are reachable")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Lord},
               {"b", tkw::game::Role::Loyalist}};

    tkw::game::SimpleAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    CHECK(ai.choose_play(g.ctx, turn).is_none());
}

TEST_CASE("ai: identity traitor spares the lord while a rebel lives")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.add_player("c", 2, 4);
    g.give("a", "sha", "s#1");
    g.mode = tkw::game::GameMode::Identity;
    g.roles = {{"a", tkw::game::Role::Traitor},
               {"b", tkw::game::Role::Lord},
               {"c", tkw::game::Role::Rebel}};

    tkw::game::SimpleAI ai;
    const tkw::game::TurnContext turn{"a", 0, 1};
    const auto sparing = ai.choose_play(g.ctx, turn);
    REQUIRE(sparing.is_some());
    CHECK(sparing.unwrap().targets == std::vector<std::string>{"c"});

    g.entities.remove("c");
    const auto striking = ai.choose_play(g.ctx, turn);
    REQUIRE(striking.is_some());
    CHECK(striking.unwrap().targets == std::vector<std::string>{"b"});
}

TEST_CASE("ai: human decider counter window renders context and reads play")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "wuxie", "w#0");

    std::istringstream in("play 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto chosen = src.play_counter(g.ctx, "a", "b", {"a"}, "wugu");
    REQUIRE(chosen.is_some());
    CHECK(chosen.unwrap() == "w#0");
    CHECK(out.str().find("w#0") != std::string::npos);  // 无懈候选渲染
    CHECK(out.str().find("使用者: b") != std::string::npos);
    CHECK(out.str().find("五谷丰登") != std::string::npos);
    CHECK(out.str().find("目标: a") != std::string::npos);
}

TEST_CASE("ai: human decider counter window declines on pass or eof")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "wuxie", "w#0");

    {
        std::istringstream in("pass\n");
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK(src.play_counter(g.ctx, "a", "", {"a"}, "shandian").is_none());
        CHECK(out.str().find("延时锦囊判定") != std::string::npos);
        CHECK(out.str().find("闪电") != std::string::npos);
    }
    {
        std::istringstream in;  // 空流：首次读取即 EOF
        std::ostringstream out;
        HumanDecider dec(in, out);
        RequestDecisionSource src(dec);
        CHECK(src.play_counter(g.ctx, "a", "b", {"a"}, "").is_none());
    }
}

TEST_CASE("ai: human discard window help prints usage and continues")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");
    g.give("a", "shan", "j#2");
    g.give("a", "tao", "t#3");

    std::istringstream in("help\ndiscard 1 2\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto chosen = src.choose_discards(
        g.ctx, "a", 2, tkw::game::DiscardReason::TurnLimit);
    REQUIRE(chosen.size() == 2);
    CHECK(out.str().find("用法：") != std::string::npos);
    CHECK(out.str().find("discard <序号>") != std::string::npos);
}

TEST_CASE("ai: human discard pass names the requirement")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    std::istringstream in("pass\ndiscard 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto chosen = src.choose_discards(
        g.ctx, "a", 1, tkw::game::DiscardReason::TurnLimit);
    REQUIRE(chosen.size() == 1);
    CHECK(chosen[0] == "s#1");
    CHECK(out.str().find("不能 pass") != std::string::npos);
    CHECK(out.str().find("需弃 1 张") != std::string::npos);
}

TEST_CASE("ai: human discard eof announces give-up")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    std::istringstream in;
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto chosen = src.choose_discards(
        g.ctx, "a", 1, tkw::game::DiscardReason::TurnLimit);
    CHECK(chosen.empty());
    CHECK(out.str().find("输入已结束") != std::string::npos);
    CHECK(out.str().find("按放弃处理") != std::string::npos);
}

TEST_CASE("ai: human play window help prints usage")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    std::istringstream in("?\npass\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);
    const tkw::game::TurnContext turn{"a", 0, 1};

    CHECK(src.choose_play(g.ctx, turn).is_none());
    CHECK(out.str().find("用法：") != std::string::npos);
    CHECK(out.str().find("pass 结束出牌阶段") != std::string::npos);
}

TEST_CASE("ai: human trigger window help prints usage")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);

    std::istringstream in("help\ny\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    CHECK(src.trigger_effect(g.ctx, "a", tkw::card::Ability::NoShaLimit));
    CHECK(out.str().find("输入 y 发动") != std::string::npos);
}

TEST_CASE("ai: human response window shows source and consequence")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "shan", "j#1");

    std::istringstream in("pass\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    CHECK(src.play_response(
              g.ctx, "a", tkw::card::ResponseKind::Jink,
              tkw::game::ResponsePrompt{"wanjian", "b", 1})
              .is_none());
    CHECK(out.str().find("来源: b 的 万箭齐发") != std::string::npos);
    CHECK(out.str().find("不出将受到 1 点伤害") != std::string::npos);
}

TEST_CASE("ai: human response window falls back when source unknown")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "shan", "j#1");

    std::istringstream in("pass\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    CHECK(src.play_response(
              g.ctx, "a", tkw::card::ResponseKind::Jink,
              tkw::game::ResponsePrompt{})
              .is_none());
    CHECK(out.str().find("响应（需打出闪）") != std::string::npos);
    CHECK(out.str().find("来源") == std::string::npos);
}

TEST_CASE("ai: human response window reports no candidate")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");  // 无闪：响应窗口无候选

    std::istringstream in;
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    CHECK(src.play_response(
              g.ctx, "a", tkw::card::ResponseKind::Jink,
              tkw::game::ResponsePrompt{})
              .is_none());
    CHECK(out.str().find("无可用响应牌") != std::string::npos);
}

TEST_CASE("ai: human reveal window names wugu source")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.give("a", "sha", "s#1");

    std::istringstream in("pick 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto picked = src.pick_from_revealed(
        g.ctx, "a", g.ctx.cards->hand("a"), tkw::game::RevealSource::Wugu);
    REQUIRE(picked.is_some());
    CHECK(out.str().find("五谷丰登亮牌") != std::string::npos);
}

TEST_CASE("ai: human reveal window names qilin source")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("b", "qilin", "h#1");
    const auto horses = g.ctx.cards->equip("b");

    std::istringstream in("pick 1\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    const auto picked = src.pick_from_revealed(
        g.ctx, "a", horses, tkw::game::RevealSource::Qilin);
    REQUIRE(picked.is_some());
    CHECK(out.str().find("麒麟弓") != std::string::npos);
}

TEST_CASE("ai: human trigger window explains ability")
{
    TestGame g("deck");
    g.add_player("a", 0, 4);
    g.add_player("b", 1, 4);
    g.equip("a", "qilin", "e#0");

    std::istringstream in("y\n");
    std::ostringstream out;
    HumanDecider dec(in, out);
    RequestDecisionSource src(dec);

    CHECK(src.trigger_effect(
        g.ctx, "a", tkw::card::Ability::DiscardHorseOnDamage));
    CHECK(out.str().find("麒麟弓") != std::string::npos);
    CHECK(out.str().find("坐骑") != std::string::npos);
}

namespace
{
    /** @brief 可变实体上可扣血（红侧对照：证明该操作确实存在）。 */
    template <typename T>
    concept HasTakeDamage = requires(T *e) {
        e->take_damage("x", 1, false);
    };

    /** @brief 可变牌容器上可移除手牌（红侧对照）。 */
    template <typename T>
    concept CanRemoveFromHand = requires(T *c, const std::string &id) {
        c->remove_from_hand(id, id);
    };

    /** @brief const 管理器可直接迭代（仅当提供了 const begin/end 时为真）。 */
    template <typename T>
    concept ConstIterable = requires(const T &m) {
        m.begin();
        m.end();
    };
}

TEST_CASE("ai: decision context is read-only at compile time")
{
    using tkw::game::GameContext;
    using tkw::game::ReadOnlyContext;
    // 接缝只暴露 const 容器指针
    static_assert(std::is_same_v<decltype(ReadOnlyContext{}.cards),
                                 const tkw::card::CardManager *>);
    static_assert(std::is_same_v<decltype(ReadOnlyContext{}.entities),
                                 const tkw::EntityManager *>);
    static_assert(std::is_same_v<decltype(ReadOnlyContext{}.catalog),
                                 const tkw::card::CardDefCatalog *>);
    static_assert(std::is_same_v<decltype(ReadOnlyContext{}.rules),
                                 const tkw::game::RulesConfig *>);
    static_assert(std::is_same_v<decltype(ReadOnlyContext{}.mode),
                                 const tkw::game::GameMode *>);
    static_assert(std::is_same_v<decltype(ReadOnlyContext{}.roles),
                                 const tkw::game::RoleTable *>);
    // 同一改状态操作在可变容器成立、在接缝的 const 容器上不可达
    static_assert(HasTakeDamage<tkw::entity::Entity>);
    static_assert(!HasTakeDamage<const tkw::entity::Entity>);
    static_assert(CanRemoveFromHand<tkw::card::CardManager>);
    static_assert(!CanRemoveFromHand<const tkw::card::CardManager>);
    // const 实体管理器不可迭代：实体只读遍历唯一入口是 const_view()
    static_assert(!ConstIterable<tkw::EntityManager>);
    // 接缝无法还原出可变上下文（一旦进入只读，改状态路径被类型切断）
    static_assert(!std::is_convertible_v<ReadOnlyContext, GameContext>);
    CHECK(true);
}
