#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <string>

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

    REQUIRE(catalog.size() == 2);
    const auto zhangfei = catalog.find("zhangfei");
    REQUIRE(zhangfei.is_some());
    const hero::HeroDef &zf = *zhangfei.unwrap();
    CHECK(zf.name == "张飞");
    REQUIRE(zf.gender.is_some());
    CHECK(zf.gender.unwrap() == entity::Gender::Male);
    CHECK(zf.hp == 4);
    CHECK(has_skill(zf, hero::HeroSkill::PaoXiao));
    CHECK(hero::display_hero_name(zf) == "张飞");
    CHECK(hero::display_skill_name(hero::HeroSkill::PaoXiao) == "咆哮");

    // 咆哮已实现，武圣未实现：审计面据此暴露未实现技能
    CHECK_FALSE(game::is_unimplemented_skill(hero::HeroSkill::PaoXiao));
    CHECK(game::is_unimplemented_skill(hero::HeroSkill::WuSheng));

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
