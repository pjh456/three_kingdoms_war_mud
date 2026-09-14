#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

#include "config/resource.hpp"
#include "game/core/effect.hpp"
#include "hero/catalog.hpp"
#include "hero/def.hpp"
#include "io/file.hpp"
#include "util/types.hpp"

namespace
{
    using namespace tkw;

    /** 在独立临时目录写一份最小武将目录（heroes.json + 单武将文件）。 */
    bool write_hero_dir(
        const std::filesystem::path &dir, const std::string &hero_id,
        const std::string &hero_json)
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        if (!std::filesystem::create_directories(dir / "heroes"))
            return false;
        if (io::write_text(dir / "heroes.json",
                           std::string("{\"name\":\"t\",\"heroes\":[\"") +
                               hero_id + "\"]}")
                .is_err())
            return false;
        return io::write_text(dir / "heroes" / (hero_id + ".json"), hero_json)
            .is_ok();
    }

    bool has_skill(const hero::HeroDef &def, hero::HeroSkill skill)
    {
        return std::find(def.skills.begin(), def.skills.end(), skill) !=
               def.skills.end();
    }
}

TEST_CASE("hero: standard catalog loads heroes and skill metadata")
{
    config::ResourceStore store(TKW_TEST_RESOURCE_DIR);
    auto cat = hero::HeroCatalog::load_optional(store, "heroes");
    REQUIRE(cat.is_ok());
    const auto &catalog = cat.unwrap();

    REQUIRE(catalog.size() == 8);
    const auto zhangfei = catalog.find("zhangfei");
    REQUIRE(zhangfei.is_some());
    const hero::HeroDef &zf = *zhangfei.unwrap();
    CHECK(zf.name == "张飞");
    REQUIRE(zf.gender.is_some());
    CHECK(zf.gender.unwrap() == entity::Gender::Male);
    CHECK(zf.hp == 4);
    CHECK(has_skill(zf, hero::HeroSkill::PaoXiao));
    CHECK(hero::display_hero_name(zf) == "张飞");
    CHECK(std::string_view(hero::display_skill_name(hero::HeroSkill::PaoXiao)) == "咆哮");

    const auto zhouyu = catalog.find("zhouyu");
    REQUIRE(zhouyu.is_some());
    const hero::HeroDef &zy = *zhouyu.unwrap();
    CHECK(zy.name == "周瑜");
    REQUIRE(zy.gender.is_some());
    CHECK(zy.gender.unwrap() == entity::Gender::Male);
    CHECK(zy.hp == 3);
    CHECK(has_skill(zy, hero::HeroSkill::YingZi));
    CHECK(std::string_view(hero::display_skill_name(hero::HeroSkill::YingZi)) == "英姿");

    const auto simayi = catalog.find("simayi");
    REQUIRE(simayi.is_some());
    const hero::HeroDef &sm = *simayi.unwrap();
    CHECK(sm.name == "司马懿");
    REQUIRE(sm.gender.is_some());
    CHECK(sm.gender.unwrap() == entity::Gender::Male);
    CHECK(sm.hp == 3);
    CHECK(has_skill(sm, hero::HeroSkill::FanKui));
    CHECK(std::string_view(hero::display_skill_name(hero::HeroSkill::FanKui)) == "反馈");

    const auto machao = catalog.find("machao");
    REQUIRE(machao.is_some());
    const hero::HeroDef &mc = *machao.unwrap();
    CHECK(mc.name == "马超");
    REQUIRE(mc.gender.is_some());
    CHECK(mc.gender.unwrap() == entity::Gender::Male);
    CHECK(mc.hp == 4);
    CHECK(has_skill(mc, hero::HeroSkill::MaShu));
    CHECK(std::string_view(hero::display_skill_name(hero::HeroSkill::MaShu)) == "马术");

    const auto huangyueying = catalog.find("huangyueying");
    REQUIRE(huangyueying.is_some());
    const hero::HeroDef &hy = *huangyueying.unwrap();
    CHECK(hy.name == "黄月英");
    REQUIRE(hy.gender.is_some());
    CHECK(hy.gender.unwrap() == entity::Gender::Female);
    CHECK(hy.hp == 3);
    CHECK(has_skill(hy, hero::HeroSkill::QiCai));
    CHECK(std::string_view(hero::display_skill_name(hero::HeroSkill::QiCai)) == "奇才");

    const auto zhaoyun = catalog.find("zhaoyun");
    REQUIRE(zhaoyun.is_some());
    const hero::HeroDef &zd = *zhaoyun.unwrap();
    CHECK(zd.name == "赵云");
    REQUIRE(zd.gender.is_some());
    CHECK(zd.gender.unwrap() == entity::Gender::Male);
    CHECK(zd.hp == 4);
    CHECK(has_skill(zd, hero::HeroSkill::LongDan));
    CHECK(std::string_view(hero::display_skill_name(hero::HeroSkill::LongDan)) == "龙胆");

    const auto zhenji = catalog.find("zhenji");
    REQUIRE(zhenji.is_some());
    const hero::HeroDef &zj = *zhenji.unwrap();
    CHECK(zj.name == "甄姬");
    REQUIRE(zj.gender.is_some());
    CHECK(zj.gender.unwrap() == entity::Gender::Female);
    CHECK(zj.hp == 3);
    CHECK(has_skill(zj, hero::HeroSkill::QingGuo));
    CHECK(std::string_view(hero::display_skill_name(hero::HeroSkill::QingGuo)) == "倾国");

    // 标准目录内全部技能均已实现：审计面不再标记为未实现
    CHECK_FALSE(game::is_unimplemented_skill(hero::HeroSkill::PaoXiao));
    CHECK_FALSE(game::is_unimplemented_skill(hero::HeroSkill::WuSheng));
    CHECK_FALSE(game::is_unimplemented_skill(hero::HeroSkill::YingZi));
    CHECK_FALSE(game::is_unimplemented_skill(hero::HeroSkill::FanKui));
    CHECK_FALSE(game::is_unimplemented_skill(hero::HeroSkill::MaShu));
    CHECK_FALSE(game::is_unimplemented_skill(hero::HeroSkill::QiCai));
    CHECK_FALSE(game::is_unimplemented_skill(hero::HeroSkill::LongDan));
    CHECK_FALSE(game::is_unimplemented_skill(hero::HeroSkill::QingGuo));

    // 目录未命中回落 id
    CHECK(hero::display_hero_name(catalog, "nobody") == "nobody");
    CHECK(hero::display_hero_name(static_cast<const hero::HeroCatalog *>(nullptr),
                                  "zhangfei") == "zhangfei");
}

TEST_CASE("hero: missing heroes file falls back to an empty optional catalog")
{
    const auto dir =
        std::filesystem::temp_directory_path() / "tkw_hero_missing";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    REQUIRE(std::filesystem::create_directories(dir));

    config::ResourceStore store(dir);
    auto optional = hero::HeroCatalog::load_optional(store, "heroes");
    REQUIRE(optional.is_ok());
    CHECK(optional.unwrap().empty());

    // 严格加载仍报文件不存在，只有 optional 回落
    auto strict = hero::HeroCatalog::load(store, "heroes");
    REQUIRE(strict.is_err());
    CHECK(strict.unwrap_err().kind == config::ConfigErrorKind::FileNotFound);

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("hero: unknown skill name is rejected at load")
{
    const auto dir = std::filesystem::temp_directory_path() / "tkw_hero_skill";
    REQUIRE(write_hero_dir(dir, "x",
                           R"({"id":"x","name":"测试","skills":["nope"]})"));

    config::ResourceStore store(dir);
    auto cat = hero::HeroCatalog::load(store, "heroes");
    REQUIRE(cat.is_err());
    CHECK(cat.unwrap_err().kind == config::ConfigErrorKind::InvalidValue);
    CHECK(cat.unwrap_err().detail.find("skills[0]") != std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("hero: hero id must match the file name")
{
    const auto dir = std::filesystem::temp_directory_path() / "tkw_hero_id";
    REQUIRE(write_hero_dir(dir, "x", R"({"id":"y","name":"测试"})"));

    config::ResourceStore store(dir);
    auto cat = hero::HeroCatalog::load(store, "heroes");
    REQUIRE(cat.is_err());
    CHECK(cat.unwrap_err().kind == config::ConfigErrorKind::InvalidValue);
    CHECK(cat.unwrap_err().detail.find("heroes/x.json.id") != std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("hero: unknown gender and negative hp are rejected")
{
    const auto dir = std::filesystem::temp_directory_path() / "tkw_hero_gender";
    REQUIRE(write_hero_dir(
        dir, "x", R"({"id":"x","name":"测试","gender":"other"})"));

    config::ResourceStore store(dir);
    auto cat = hero::HeroCatalog::load(store, "heroes");
    REQUIRE(cat.is_err());
    CHECK(cat.unwrap_err().detail.find("gender") != std::string::npos);

    REQUIRE(write_hero_dir(dir, "x", R"({"id":"x","name":"测试","hp":-1})"));
    auto cat2 = hero::HeroCatalog::load(store, "heroes");
    REQUIRE(cat2.is_err());
    CHECK(cat2.unwrap_err().detail.find("hp") != std::string::npos);

    std::filesystem::remove_all(dir);
}
