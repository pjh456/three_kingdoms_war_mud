/**
 * @file   catalog.hpp
 * @brief  武将目录：`heroes.json` + `heroes/<id>.json` 的加载与语义校验。
 * @details 语义校验归本域（`config` 只管「文件 → `Document`」）：
 *          - `heroes.json` 引用一名武将 → 按 `<root>/heroes/<id>.json` 加载单武将
 *            文件；
 *          - 未知技能名 / 未知性别在加载时立即 `InvalidValue` 失败（detail 为
 *            字段路径）；
 *          - 文件内 `id` 必须等于文件名（引用方），不一致即数据事故。
 * @note   加载完成后不持有 `Document`：全部解析成 `HeroDef` 值类型，`Document` 即弃。
 * @note   武将目录独立于牌表文件，不参与 `deck_hash`；缺 `heroes.json` 时经
 *         `load_optional` 回落空目录，使无武将数据的自定义牌表照常可玩。
 * @ingroup tkw_hero
 */

#ifndef INCLUDE_TKW_HERO_CATALOG_HPP
#define INCLUDE_TKW_HERO_CATALOG_HPP

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "config/error.hpp"
#include "config/fields.hpp"
#include "config/resource.hpp"
#include "hero/def.hpp"
#include "io/file.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace hero
    {
        namespace json = pjh::json;
        namespace cfg = tkw::config;

        namespace detail
        {
            /** @brief skills 封闭名表：严格解析与展示名查询共用的单一表源。 */
            inline constexpr
                std::initializer_list<std::pair<std::string_view, HeroSkill>>
                    skill_table{{"paoxiao", HeroSkill::PaoXiao},
                                {"wusheng", HeroSkill::WuSheng},
                                {"yingzi", HeroSkill::YingZi},
                                {"fankui", HeroSkill::FanKui},
                                {"mashu", HeroSkill::MaShu},
                                {"qicai", HeroSkill::QiCai},
                                {"longdan", HeroSkill::LongDan},
                                {"qingguo", HeroSkill::QingGuo}};

            /**
             * @brief  字符串 → 技能枚举。
             * @param[in] s    待查表文本。
             * @param[in] path 字段路径，用于失败定位。
             * @return 解析结果。
             * @retval Ok 命中 `skill_table` 中的键。
             * @retval Err(InvalidValue) 未命中，detail = `path`。
             */
            inline cfg::ConfigResult<HeroSkill> skill_value(
                std::string_view s, std::string_view path)
            {
                for (const auto &[key, val] : skill_table)
                    if (key == s)
                        return cfg::ConfigResult<HeroSkill>::Ok(val);
                return cfg::fail<HeroSkill>(
                    cfg::ConfigErrorKind::InvalidValue, std::string(path));
            }

            /**
             * @brief  解析 `skills` 数组。
             * @param[in] root 单武将顶层对象。
             * @param[in] path 容器路径。
             * @return 技能列表；字段缺失时为空列表。
             * @retval Ok 字段缺失或逐项均命中技能名表。
             * @retval Err(TypeMismatch) `skills` 非数组或元素非字符串。
             * @retval Err(InvalidValue) 某个技能名未登记。
             */
            inline cfg::ConfigResult<std::vector<HeroSkill>> parse_skills(
                const json::Json &root, std::string_view path)
            {
                std::vector<HeroSkill> out;
                const auto *o = root.try_as_object();
                if (!o)
                    return cfg::fail<std::vector<HeroSkill>>(
                        cfg::ConfigErrorKind::TypeMismatch,
                        cfg::container_path(path));
                if (!o->contains("skills"))
                    return cfg::ConfigResult<std::vector<HeroSkill>>::Ok(
                        std::move(out));

                const json::Json &v = (*o)["skills"];
                const auto *arr = v.try_as_array();
                if (!arr)
                    return cfg::fail<std::vector<HeroSkill>>(
                        cfg::ConfigErrorKind::TypeMismatch,
                        cfg::field_path(path, "skills"));

                const auto prefix = cfg::field_path(path, "skills");
                for (std::size_t i = 0; i < arr->size(); ++i)
                {
                    const std::string ip =
                        prefix + "[" + std::to_string(i) + "]";
                    auto s = (*arr)[i].try_as_string();
                    if (!s)
                        return cfg::fail<std::vector<HeroSkill>>(
                            cfg::ConfigErrorKind::TypeMismatch, ip);
                    auto skill = skill_value(*s, ip);
                    if (skill.is_err())
                        return cfg::ConfigResult<std::vector<HeroSkill>>::Err(
                            skill.unwrap_err());
                    out.push_back(skill.unwrap());
                }
                return cfg::ConfigResult<std::vector<HeroSkill>>::Ok(std::move(out));
            }

            /**
             * @brief  解析可选性别。
             * @param[in] root 单武将顶层对象。
             * @param[in] path 容器路径。
             * @return 性别；字段缺失时为 `None`。
             * @retval Ok 字段缺失（`None`）或成功解析（`Some`）。
             * @retval Err(TypeMismatch) 容器非对象或字段非字符串。
             * @retval Err(InvalidValue) 文本非 `"male"`/`"female"`。
             */
            inline cfg::ConfigResult<Option<entity::Gender>> parse_gender(
                const json::Json &root, std::string_view path)
            {
                const auto *o = root.try_as_object();
                if (!o)
                    return cfg::fail<Option<entity::Gender>>(
                        cfg::ConfigErrorKind::TypeMismatch,
                        cfg::container_path(path));
                if (!o->contains("gender"))
                    return cfg::ConfigResult<Option<entity::Gender>>::Ok(
                        Option<entity::Gender>::None());

                auto s = (*o)["gender"].try_as_string();
                if (!s)
                    return cfg::fail<Option<entity::Gender>>(
                        cfg::ConfigErrorKind::TypeMismatch,
                        cfg::field_path(path, "gender"));

                entity::Gender g = entity::Gender::Male;
                if (*s == "male")
                    g = entity::Gender::Male;
                else if (*s == "female")
                    g = entity::Gender::Female;
                else
                    return cfg::fail<Option<entity::Gender>>(
                        cfg::ConfigErrorKind::InvalidValue,
                        cfg::field_path(path, "gender"));
                return cfg::ConfigResult<Option<entity::Gender>>::Ok(
                    Option<entity::Gender>::Some(g));
            }

            /**
             * @brief  解析单武将文件。
             * @param[in] root 文件顶层对象。
             * @param[in] path 容器路径（如 `"heroes/zhangfei.json"`），用于拼错误
             *             字段路径。
             * @return 武将定义。
             * @retval Ok 必填字段齐全、可选字段合法、文件内 `id` 与引用名一致。
             * @retval Err(MissingField/TypeMismatch/InvalidValue) 按字段定位失败。
             */
            inline cfg::ConfigResult<HeroDef> parse_hero_def(
                const json::Json &root, std::string_view path)
            {
                HeroDef def;

                auto id = cfg::require_string(root, "id", path);
                if (id.is_err())
                    return cfg::ConfigResult<HeroDef>::Err(id.unwrap_err());
                def.id = id.unwrap();

                auto name = cfg::require_string(root, "name", path);
                if (name.is_err())
                    return cfg::ConfigResult<HeroDef>::Err(name.unwrap_err());
                def.name = name.unwrap();

                auto gender = parse_gender(root, path);
                if (gender.is_err())
                    return cfg::ConfigResult<HeroDef>::Err(gender.unwrap_err());
                def.gender = gender.unwrap();

                auto hp = cfg::opt_int_range(root, "hp", 0, path);
                if (hp.is_err())
                    return cfg::ConfigResult<HeroDef>::Err(hp.unwrap_err());
                if (hp.unwrap() < 0)
                    return cfg::fail<HeroDef>(
                        cfg::ConfigErrorKind::InvalidValue,
                        cfg::field_path(path, "hp"));
                def.hp = hp.unwrap();

                auto skills = parse_skills(root, path);
                if (skills.is_err())
                    return cfg::ConfigResult<HeroDef>::Err(skills.unwrap_err());
                def.skills = std::move(skills).unwrap();

                auto text = cfg::opt_string(root, "text", "", path);
                if (text.is_err())
                    return cfg::ConfigResult<HeroDef>::Err(text.unwrap_err());
                def.text = text.unwrap();

                return cfg::ConfigResult<HeroDef>::Ok(std::move(def));
            }
        }  // namespace detail

        /**
         * @brief  技能中文展示名（技能表/警告文案用）。
         * @param[in] skill 技能枚举。
         * @return 静态中文名；未覆盖的枚举值返回空串。
         */
        inline constexpr const char *display_skill_name(HeroSkill skill)
        {
            switch (skill)
            {
            case HeroSkill::PaoXiao:
                return "咆哮";
            case HeroSkill::WuSheng:
                return "武圣";
            case HeroSkill::YingZi:
                return "英姿";
            case HeroSkill::FanKui:
                return "反馈";
            case HeroSkill::MaShu:
                return "马术";
            case HeroSkill::QiCai:
                return "奇才";
            case HeroSkill::LongDan:
                return "龙胆";
            case HeroSkill::QingGuo:
                return "倾国";
            }
            return "";
        }

        /**
         * @class HeroCatalog
         * @brief 对局作用域的武将定义容器：heroes.json 引用的有序定义 + id 索引。
         * @note 一次加载后不可变；迭代顺序 = heroes.json 引用顺序，id 查询走内部
         *       索引（O(1)）。
         */
        class HeroCatalog
        {
        public:
            /**
             * @brief 显式声明移动/拷贝：vector 与 unordered_map 的移动只窃取内部
             *        指针、实际不抛，但标准未把容器移动标为 noexcept。显式 noexcept
             *        移动使目录可放入 Result（其存储要求 T 移动构造为 noexcept）。
             * @note 自定义移动会抑制隐式拷贝，故拷贝一并 = default 保留。
             */
            HeroCatalog() = default;
            HeroCatalog(const HeroCatalog &) = default; /**< 拷贝构造。 */
            HeroCatalog &operator=(const HeroCatalog &) = default; /**< 拷贝赋值；@return 自身。 */
            /**
             * @brief  移动构造。
             * @param[in] other 被移动的目录；之后仅可析构或重新赋值。
             */
            HeroCatalog(HeroCatalog &&other) noexcept
                : defs(std::move(other.defs)), index(std::move(other.index))
            {
            }
            /**
             * @brief  移动赋值。
             * @param[in] other 被移动的目录。
             * @return 自身引用。
             */
            HeroCatalog &operator=(HeroCatalog &&other) noexcept
            {
                defs = std::move(other.defs);
                index = std::move(other.index);
                return *this;
            }

            /**
             * @brief  从 `ResourceStore` 加载：先读 `<hero_name>.json`（武将构成），
             *         再逐个加载 `heroes/<id>.json`。
             * @param[in] store     资源目录句柄。
             * @param[in] hero_name 武将资源名（通常 `"heroes"`）。
             * @return 加载完成的目录。
             * @retval Ok 定义按 `heroes.json` 引用序全部解析并建立 id 索引。
             * @retval Err(FileNotFound/IoFailed/ParseError) 清单或某名武将文件
             *         缺失、I/O 失败或 JSON 非法。
             * @retval Err(MissingField/TypeMismatch/InvalidValue) 字段缺失、类型
             *         不符、枚举未登记、`id` 与引用名不一致或重复引用。
             * @pre   `store` 生命周期覆盖本次调用，且其根目录可读。
             * @post  成功时全部 `Document` 即弃，仅保留 `HeroDef` 值类型；
             *         失败时不产出部分目录，`store` 不被修改。
             */
            static cfg::ConfigResult<HeroCatalog> load(
                const cfg::ResourceStore &store, std::string_view hero_name)
            {
                auto doc = store.load(hero_name);
                if (doc.is_err())
                    return cfg::ConfigResult<HeroCatalog>::Err(doc.unwrap_err());
                const json::Json &root = doc.unwrap().root();

                HeroCatalog catalog;
                auto er = cfg::each(root, "heroes", {},
                                    [&catalog, &store](const json::Json &item,
                                                       std::string_view ip)
                                        -> cfg::ConfigResult<void>
                {
                    auto id_s = item.try_as_string();
                    if (!id_s)
                        return cfg::fail<void>(
                            cfg::ConfigErrorKind::TypeMismatch, std::string(ip));
                    const std::string hid(*id_s);

                    if (catalog.index.find(hid) != catalog.index.end())
                        return cfg::fail<void>(
                            cfg::ConfigErrorKind::InvalidValue,
                            std::string(ip) + " 重复引用武将 " + hid);

                    const std::string file = "heroes/" + hid + ".json";
                    auto hero_doc = store.load("heroes/" + hid);
                    if (hero_doc.is_err())
                        return cfg::ConfigResult<void>::Err(hero_doc.unwrap_err());

                    auto def = detail::parse_hero_def(hero_doc.unwrap().root(), file);
                    if (def.is_err())
                        return cfg::ConfigResult<void>::Err(def.unwrap_err());

                    if (def.unwrap().id != hid)
                        return cfg::fail<void>(
                            cfg::ConfigErrorKind::InvalidValue, file + ".id");

                    catalog.index.emplace(hid, catalog.defs.size());
                    catalog.defs.push_back(std::move(def).unwrap());
                    return cfg::ConfigResult<void>::Ok();
                });
                if (er.is_err())
                    return cfg::ConfigResult<HeroCatalog>::Err(er.unwrap_err());
                return cfg::ConfigResult<HeroCatalog>::Ok(std::move(catalog));
            }

            /**
             * @brief  可选加载：`<hero_name>.json` 不存在时回落空目录；其余错误透传。
             * @param[in] store     资源目录句柄。
             * @param[in] hero_name 武将资源名（通常 `"heroes"`）。
             * @return 加载结果。
             * @retval Ok 目录（缺清单文件时为空目录）。
             * @retval Err 解析/类型/枚举等硬错误，语义同 `load`。
             * @note   建局与 `tkw heroes` 共用此入口：无武将数据的自定义牌表不新增
             *         失败面，坏数据仍在加载期硬失败。
             */
            static cfg::ConfigResult<HeroCatalog> load_optional(
                const cfg::ResourceStore &store, std::string_view hero_name)
            {
                if (!tkw::io::exists(store.root() /
                                     (std::string(hero_name) + ".json")))
                    return cfg::ConfigResult<HeroCatalog>::Ok(HeroCatalog{});
                return load(store, hero_name);
            }

            /**
             * @brief  O(1) 按武将 id 查询（经内部索引）。
             * @param[in] id 武将 id。
             * @return 定义指针；`None` = 未收录。
             * @retval Some 指针指向内部 `defs`，生命周期同本目录。
             * @retval None 目录中无此 id。
             */
            Option<const HeroDef *> find(const std::string &id) const
            {
                auto it = index.find(id);
                if (it == index.end())
                    return Option<const HeroDef *>::None();
                return Option<const HeroDef *>::Some(&defs[it->second]);
            }

            /**
             * @brief  收录的定义数。
             * @return `defs` 中的定义条数。
             */
            std::size_t size() const noexcept { return defs.size(); }

            /**
             * @brief  是否为空。
             * @return `true` = 目录未收录任何武将。
             */
            bool empty() const noexcept { return defs.empty(); }

            /**
             * @brief  按 `heroes.json` 引用顺序迭代。
             * @return 指向首元素的迭代器。
             */
            auto begin() const noexcept { return defs.begin(); }

            /**
             * @brief  迭代尾标。
             * @return 尾后迭代器。
             */
            auto end() const noexcept { return defs.end(); }

        private:
            std::vector<HeroDef> defs; /**< heroes.json 引用顺序 */
            std::unordered_map<std::string, std::size_t> index; /**< id → defs 下标 */
        };

        /**
         * @brief  按 id 从目录取展示名。
         * @param[in] catalog 武将目录。
         * @param[in] hero_id 武将 id。
         * @return 目录收录且 `name` 非空 → `name`；否则回落 `hero_id`（拷贝）。
         * @note   结果按值返回：目录未收录时返回入参的拷贝，不暴露调用方引用。
         */
        inline std::string display_hero_name(
            const HeroCatalog &catalog, const std::string &hero_id)
        {
            const auto def = catalog.find(hero_id);
            return def.is_some() ? display_hero_name(*def.unwrap()) : hero_id;
        }

        /**
         * @brief  目录指针可空（无武将数据）的展示名。
         * @param[in] catalog 武将目录；可为 `nullptr`。
         * @param[in] hero_id 武将 id。
         * @return `catalog` 为空或未收录 → `hero_id`；否则同目录重载。
         */
        inline std::string display_hero_name(
            const HeroCatalog *catalog, const std::string &hero_id)
        {
            return catalog == nullptr ? hero_id
                                      : display_hero_name(*catalog, hero_id);
        }
    }
}

#endif  // INCLUDE_TKW_HERO_CATALOG_HPP
