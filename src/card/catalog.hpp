/**
 * @file   catalog.hpp
 * @brief  卡牌目录：`deck.json` + `cards/<id>.json` 的加载与语义校验。
 * @details 本域负责「数据 → 值类型」的语义校验；底层 `config` 只把文件读成
 *          `Document`，不解释任何卡牌语义。校验规则：
 *          - `deck.json` 引用一张卡 → 按 `<root>/cards/<id>.json` 加载单卡文件；
 *          - 未知 `effect.kind` / `scope` / `suit` / `equip` 等在加载时立即
 *            `InvalidValue` 失败（detail 为字段路径，如
 *            `"cards/sha.json.effect.kind"`）；
 *          - 文件内 `id` 必须等于文件名（deck 引用方），不一致即数据事故。
 * @note   加载完成后不持有 `Document`：全部解析成 `CardDef` 值类型，`Document` 即弃。
 * @ingroup tkw_card
 */

#ifndef INCLUDE_TKW_CARD_CATALOG_HPP
#define INCLUDE_TKW_CARD_CATALOG_HPP

#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "card/def.hpp"
#include "config/error.hpp"
#include "config/fields.hpp"
#include "config/resource.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace card
    {
        namespace json = pjh::json;
        namespace cfg = tkw::config;

        namespace detail
        {
            /**
             * @brief  判断 `subtype` 是否属于封闭集合。
             * @param[in] s 待校验的 subtype 文本。
             * @return `true` = 空串或已登记的 subtype。
             */
            bool is_valid_subtype(std::string_view s);

            /** @brief scope 字段封闭六值集：effect/judge 解析共用的单一表源。 */
            inline constexpr std::initializer_list<std::pair<std::string_view, Scope>>
                scope_table{{"self", Scope::Self}, {"one_other", Scope::OneOther},
                             {"all_others", Scope::AllOthers}, {"all", Scope::All},
                             {"one_or_two", Scope::OneOrTwo},
                             {"any_one", Scope::AnyOne}};

            /** @brief damage_type 字段封闭三值集：effect/judge 解析共用的单一表源。 */
            inline constexpr
                std::initializer_list<std::pair<std::string_view, DamageType>>
                    damage_type_table{{"normal", DamageType::Normal},
                                      {"fire", DamageType::Fire},
                                      {"thunder", DamageType::Thunder}};

            /** @brief effect.kind 封闭名表：严格解析与名称查询共用的单一表源。 */
            inline constexpr
                std::initializer_list<std::pair<std::string_view, CardEffectKind>>
                    effect_kind_table{{"damage", CardEffectKind::Damage},
                                      {"jink", CardEffectKind::Jink},
                                      {"heal", CardEffectKind::Heal},
                                      {"draw", CardEffectKind::Draw},
                                      {"discard_target", CardEffectKind::DiscardTarget},
                                      {"steal", CardEffectKind::Steal},
                                      {"aoe_damage", CardEffectKind::AoeDamage},
                                      {"duel", CardEffectKind::Duel},
                                      {"reveal_pick", CardEffectKind::RevealPick},
                                      {"borrowed_sword", CardEffectKind::BorrowedSword},
                                      {"analeptic", CardEffectKind::Analeptic},
                                      {"chain", CardEffectKind::Chain},
                                      {"fire_attack", CardEffectKind::FireAttack}};

            /** @brief abilities 封闭名表：严格解析与名称查询共用的单一表源。 */
            inline constexpr
                std::initializer_list<std::pair<std::string_view, Ability>>
                    ability_table{{"no_sha_limit", Ability::NoShaLimit},
                                  {"ignore_armor", Ability::IgnoreArmor},
                                  {"cixiong", Ability::Cixiong},
                                  {"extra_sha_after_jink", Ability::ExtraShaAfterJink},
                                  {"two_cards_as_sha", Ability::TwoCardsAsSha},
                                  {"discard_two_force_damage", Ability::DiscardTwoForceDamage},
                                  {"multi_target_sha", Ability::MultiTargetSha},
                                  {"discard_horse_on_damage", Ability::DiscardHorseOnDamage},
                                  {"damage_as_discard", Ability::DamageAsDiscard},
                                  {"judgement_jink", Ability::JudgementJink},
                                  {"black_sha_immune", Ability::BlackShaImmune},
                                  {"vine_armor", Ability::VineArmor},
                                  {"guding_blade", Ability::GudingBlade},
                                  {"silver_lion", Ability::SilverLion},
                                  {"fire_sha_convert", Ability::FireShaConvert}};

            /**
             * @brief  字符串 → 封闭枚举。
             * @tparam E 目标枚举类型。
             * @param[in] s     待查表文本。
             * @param[in] path  字段路径，用于失败定位。
             * @param[in] table 名表；与严格解析共用。
             * @return 解析结果。
             * @retval Ok 命中 `table` 中的键。
             * @retval Err(InvalidValue) 未命中，detail = `path`。
             */
            template <typename E>
            cfg::ConfigResult<E> enum_value(
                std::string_view s, std::string_view path,
                std::initializer_list<std::pair<std::string_view, E>> table)
            {
                for (const auto &[key, val] : table)
                    if (key == s)
                        return cfg::ConfigResult<E>::Ok(val);
                return cfg::fail<E>(cfg::ConfigErrorKind::InvalidValue, std::string(path));
            }

            /**
             * @brief  必填字符串字段 → 枚举。
             * @tparam E 目标枚举类型。
             * @param[in] obj   所属 JSON 对象。
             * @param[in] key   字段名。
             * @param[in] path  容器路径。
             * @param[in] table 名表。
             * @return 解析结果。
             * @retval Ok 字段存在、为字符串且命中名表。
             * @retval Err(MissingField) 字段缺失。
             * @retval Err(TypeMismatch) 字段非字符串。
             * @retval Err(InvalidValue) 字符串未命中名表。
             */
            template <typename E>
            cfg::ConfigResult<E> require_enum(
                const json::Json &obj, std::string_view key, std::string_view path,
                std::initializer_list<std::pair<std::string_view, E>> table)
            {
                auto s = cfg::require_string(obj, key, path);
                if (s.is_err())
                    return cfg::ConfigResult<E>::Err(s.unwrap_err());
                return enum_value<E>(s.unwrap(), cfg::field_path(path, key), table);
            }

            /**
             * @brief  可选字符串字段 → 枚举。
             * @tparam E 目标枚举类型。
             * @param[in] obj   所属 JSON 对象。
             * @param[in] key   字段名。
             * @param[in] path  容器路径。
             * @param[in] table 名表。
             * @return 枚举包装；字段缺失时为 `None`。
             * @retval Ok 字段缺失或成功解析（`Some`）。
             * @retval Err(TypeMismatch) 容器非对象或字段非字符串。
             * @retval Err(InvalidValue) 字符串未命中名表。
             */
            template <typename E>
            cfg::ConfigResult<Option<E>> opt_enum(
                const json::Json &obj, std::string_view key, std::string_view path,
                std::initializer_list<std::pair<std::string_view, E>> table)
            {
                const auto *o = obj.try_as_object();
                if (!o)
                    return cfg::fail<Option<E>>(
                        cfg::ConfigErrorKind::TypeMismatch,
                        cfg::container_path(path));
                if (!o->contains(key))
                    return cfg::ConfigResult<Option<E>>::Ok(Option<E>::None());
                const json::Json &v = (*o)[key];
                auto s = v.try_as_string();
                if (!s)
                    return cfg::fail<Option<E>>(
                        cfg::ConfigErrorKind::TypeMismatch, cfg::field_path(path, key));
                auto r = enum_value<E>(*s, cfg::field_path(path, key), table);
                if (r.is_err())
                    return cfg::ConfigResult<Option<E>>::Err(r.unwrap_err());
                return cfg::ConfigResult<Option<E>>::Ok(Option<E>::Some(r.unwrap()));
            }

            /**
             * @brief  可选对象字段：缺失回落 `None`。
             * @param[in] obj  所属 JSON 对象。
             * @param[in] key  字段名。
             * @param[in] path 容器路径。
             * @return 指向子对象的只读指针；字段缺失时为 `None`。
             * @retval Ok 字段缺失（`None`）或为对象（`Some`）。
             * @retval Err(TypeMismatch) 容器非对象或字段非对象。
             * @warning 返回指针指向 `obj` 内部，生命周期不得超过 `obj`。
             */
            cfg::ConfigResult<Option<const json::Json *>> opt_object(
                const json::Json &obj, std::string_view key, std::string_view path);

            /**
             * @brief  解析单个副本：`suit` + `number`。
             * @param[in] item 副本对象。
             * @param[in] ip   该副本的容器路径。
             * @return 花色点数副本。
             * @retval Ok 字段齐全且 `number` 在 1~13。
             * @retval Err(MissingField/TypeMismatch/InvalidValue) 按字段定位失败。
             */
            cfg::ConfigResult<CardCopy> parse_card_copy(
                const json::Json &item, std::string_view ip);

            /**
             * @brief  解析 `effect` 对象。
             * @param[in] obj  effect 对象。
             * @param[in] path 容器路径。
             * @return 卡牌主动效果。
             * @retval Ok `kind` 及该 kind 所需字段均合法。
             * @retval Err(MissingField/TypeMismatch/InvalidValue) 按字段定位失败。
             * @note   校验 kind 专属不变量：`Damage` 类 amount > 0，
             *         `Draw`/`DiscardTarget` count > 0，`Steal` count/range > 0，
             *         `FireAttack` 须显式火焰伤，`Chain` 须显式 `scope`。
             */
            cfg::ConfigResult<CardEffect> parse_card_effect(
                const json::Json &obj, std::string_view path);

            /**
             * @brief  解析 `equip` 对象。
             * @param[in] obj  equip 对象。
             * @param[in] path 容器路径。
             * @return 装备参数。
             * @retval Ok `slot` 合法且可选 `range` 合法。
             * @retval Err(MissingField/TypeMismatch/InvalidValue) 按字段定位失败。
             */
            cfg::ConfigResult<CardEquip> parse_card_equip(
                const json::Json &obj, std::string_view path);

            /**
             * @brief  解析 `judge` 对象（延时锦囊/防具判定）。
             * @param[in] obj  judge 对象。
             * @param[in] path 容器路径。
             * @return 判定描述（条件 + 成功/失败动作）。
             * @retval Ok 必填 `trigger`/`success` 合法，可选字段合法。
             * @retval Err(MissingField/TypeMismatch/InvalidValue) 按字段定位失败；
             *         `Damage` 动作要求 amount > 0。
             */
            cfg::ConfigResult<JudgeEffect> parse_judge(
                const json::Json &obj, std::string_view path);

            /**
             * @brief  解析 `abilities` 数组。
             * @param[in] root 单卡顶层对象。
             * @param[in] path 容器路径。
             * @return 能力列表；字段缺失时为空列表。
             * @retval Ok 字段缺失或逐项均命中能力名表。
             * @retval Err(TypeMismatch) `abilities` 非数组或元素非字符串。
             * @retval Err(InvalidValue) 某个能力名未登记。
             */
            cfg::ConfigResult<std::vector<Ability>> parse_abilities(
                const json::Json &root, std::string_view path);

            /**
             * @brief  解析单卡文件。
             * @param[in] root 文件顶层对象。
             * @param[in] path 容器路径（如 `"cards/sha.json"`），用于拼错误字段路径。
             * @return 卡牌定义。
             * @retval Ok 必填字段齐全、可选字段合法、文件内 `id` 与引用名一致。
             * @retval Err(MissingField/TypeMismatch/InvalidValue) 按字段定位失败。
             */
            cfg::ConfigResult<CardDef> parse_card_def(
                const json::Json &root, std::string_view path);
        }

        /**
         * @brief  `effect.kind` 原文 → 效果类别。
         * @param[in] name `effect.kind` 原文。
         * @return 效果类别；`None` 表示名表无此机制（未知/未实现）。
         * @retval Some 命中 `detail::effect_kind_table`。
         * @retval None 未登记。
         * @note   与严格解析共用同一名表，避免名↔枚举映射漂移。
         */
        Option<CardEffectKind> effect_kind_from_name(std::string_view name);

        /**
         * @brief  `abilities` 原文 → 装备能力。
         * @param[in] name 能力原文。
         * @return 装备能力；`None` 表示名表无此能力（未知/未实现）。
         * @retval Some 命中 `detail::ability_table`。
         * @retval None 未登记。
         * @note   与严格解析共用同一名表，避免名↔枚举映射漂移。
         */
        Option<Ability> ability_from_name(std::string_view name);

        /** @brief 单卡机制名原文（不做枚举校验，仅供机制审计）。 */
        struct RawMechanisms
        {
            std::string id;   /**< `deck.json` 引用的卡 id（= `cards/<id>.json` 文件名）。 */
            std::string name; /**< 卡中文名；JSON 缺失时回落 id */
            Option<std::string> effect_kind =
                Option<std::string>::None(); /**< effect.kind 原文；无 effect/无 kind 为 None */
            std::vector<std::string> abilities; /**< abilities 逐项原文；无则空 */
        };

        /**
         * @brief 容错扫描牌堆的机制名原文：未知机制名不报错，交给调用方判定。
         * @param[in] store     资源目录句柄。
         * @param[in] deck_name 牌堆资源名（通常 "deck"）。
         * @return Ok 为按 deck 序的逐卡机制原文；Err 为文件缺失/非法 JSON/
         *         deck 结构错误/字段类型不符（detail 带路径），与严格加载同级。
         * @note 只读 id/name/effect.kind/abilities 的字符串原文，不校验枚举值，
         *       也不校验 scope/copies 等其余字段；用途是审计「机制名是否被引擎
         *       认识」，对局建局仍走严格加载（未知机制直接失败）。
         */
        cfg::ConfigResult<std::vector<RawMechanisms>> scan_mechanisms(
            const cfg::ResourceStore &store, std::string_view deck_name);

        /**
         * @class CardDefCatalog
         * @brief 对局作用域的卡牌定义容器：deck.json 引用的有序定义 + id 索引。
         * @note 一次加载后不可变：实体牌（Card）引用其 def_id 或拷贝副本，
         *       目录本身不参与对局状态变化。
         * @note 迭代顺序 = deck.json 引用顺序（**不是无序**）：build_deck 等
         *       消费者依赖该顺序，保证同 seed 下牌堆初始序确定；id 查询
         *       走内部索引（O(1)），与迭代序无关。
         */
        class CardDefCatalog
        {
        public:
            /**
             * @brief 显式声明移动/拷贝：vector 与 unordered_map 的移动只窃取内部
             *        指针、实际不抛，但标准未把容器移动标为 noexcept。显式 noexcept
             *        移动使目录可放入 Result（其存储要求 T 移动构造为 noexcept）。
             * @note 自定义移动会抑制隐式拷贝，故拷贝一并 = default 保留。
             */
            CardDefCatalog() = default;
            CardDefCatalog(const CardDefCatalog &) = default; /**< 拷贝构造。 */
            CardDefCatalog &operator=(const CardDefCatalog &) = default; /**< 拷贝赋值；@return 自身。 */
            /**
             * @brief  移动构造。
             * @param[in] other 被移动的目录；之后仅可析构或重新赋值。
             */
            CardDefCatalog(CardDefCatalog &&other) noexcept;
            /**
             * @brief  移动赋值。
             * @param[in] other 被移动的目录。
             * @return 自身引用。
             */
            CardDefCatalog &operator=(CardDefCatalog &&other) noexcept;

            /**
             * @brief  从 `ResourceStore` 加载：先读 `<deck_name>.json`（牌堆构成），
             *         再逐个加载 `cards/<id>.json`。
             * @param[in] store     资源目录句柄。
             * @param[in] deck_name 牌堆资源名（通常 `"deck"`）。
             * @return 加载完成的目录。
             * @retval Ok 定义按 `deck.json` 引用序全部解析并建立 id 索引。
             * @retval Err(FileNotFound/IoFailed/ParseError) 牌堆或某张单卡文件
             *         缺失、I/O 失败或 JSON 非法。
             * @retval Err(MissingField/TypeMismatch/InvalidValue) 字段缺失、类型
             *         不符、枚举未登记、`id` 与引用名不一致、重复引用或牌堆
             *         总张数为 0。
             * @pre   `store` 生命周期覆盖本次调用，且其根目录可读。
             * @post  成功时全部 `Document` 即弃，仅保留 `CardDef` 值类型；
             *         失败时不产出部分目录，`store` 不被修改。
             */
            static cfg::ConfigResult<CardDefCatalog> load(
                const cfg::ResourceStore &store, std::string_view deck_name);

            /**
             * @brief  O(1) 按卡牌 id 查询（经内部索引）。
             * @param[in] id 卡牌定义 id。
             * @return 定义指针；`None` = 未收录。
             * @retval Some 指针指向内部 `m_defs`，生命周期同本目录。
             * @retval None 目录中无此 id。
             */
            Option<const CardDef *> find(const std::string &id) const;

            /**
             * @brief  收录的定义数。
             * @return `m_defs` 中的定义条数。
             */
            std::size_t size() const noexcept { return m_defs.size(); }

            /**
             * @brief  牌堆物理张数（所有定义副本数之和）。
             * @return 全部 `CardDef.copies` 的张数合计。
             */
            std::size_t total_copies() const noexcept;

            /**
             * @brief  按 `deck.json` 引用顺序迭代。
             * @return 指向首元素的迭代器。
             */
            auto begin() const noexcept { return m_defs.begin(); }

            /**
             * @brief  迭代尾标。
             * @return 尾后迭代器。
             */
            auto end() const noexcept { return m_defs.end(); }

        private:
            std::vector<CardDef> m_defs; /**< deck.json 引用顺序（build_deck 等依赖此序） */
            std::unordered_map<std::string, std::size_t> m_index; /**< id → m_defs 下标 */
        };

        /**
         * @brief  卡定义展示名：`name` 非空取 `name`，否则回落 `id`。
         * @param[in] def 卡牌定义。
         * @return 引用指向 `def` 自身字段（`name` 或 `id`），生命周期同 `def`。
         */
        const std::string &display_name(const CardDef &def) noexcept;

        /**
         * @brief  按 id 从目录取展示名。
         * @param[in] catalog 卡牌目录。
         * @param[in] def_id  卡牌定义 id。
         * @return 目录收录且 `name` 非空 → `name`；否则回落 `def_id`（拷贝）。
         * @note   结果按值返回：目录未收录时返回入参的拷贝，不暴露调用方引用。
         */
        std::string display_name(
            const CardDefCatalog &catalog, const std::string &def_id);

        /**
         * @brief  目录指针可空（决策请求的目录可选）的展示名。
         * @param[in] catalog 卡牌目录；可为 `nullptr`。
         * @param[in] def_id  卡牌定义 id。
         * @return `catalog` 为空或未收录 → `def_id`；否则同目录重载。
         */
        std::string display_name(
            const CardDefCatalog *catalog, const std::string &def_id);
    }
}

#endif  // INCLUDE_TKW_CARD_CATALOG_HPP