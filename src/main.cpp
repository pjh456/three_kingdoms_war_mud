/**
 * @file main.cpp
 * @brief CLI 入口（pjh_cli）：构建命令树后一次性解析并分派 help/version/action。
 * @note 命令树与执行体在 cli/commands.hpp，便于测试复用同一棵树。
 *       入口自行渲染：解析错误经项目中文前缀，动作错误保留原消息。
 * @ingroup tkw_cli
 */

#include <iostream>

#include <pjh_cli.hpp>

#include "cli/commands.hpp"
#include "cli/error_zh.hpp"

int main(int argc, char **argv)
{
    pjh::cli::App app("tkw", "0.1.0", "三国杀式卡牌对局引擎");
    tkw::cli::Session session;  // 跨命令持有的对局会话
    tkw::cli::build_app(app, session);

    // 批量入口也启用模糊匹配：唯一近距匹配自动纠错，多候选报歧义。
    // 非打印执行：帮助/版本与错误均交回入口按项目文案输出，退出码走框架契约。
    auto result = app.run_fuzzy_quiet(argc, argv);
    if (result.kind == pjh::cli::AppRunResult::Kind::Help ||
        result.kind == pjh::cli::AppRunResult::Kind::Version)
    {
        std::cout << result.text;
        return 0;
    }
    if (result.error.is_some())
    {
        const auto &e = result.error.unwrap();
        if (e.kind() == pjh::cli::ErrorKind::Runtime)
            std::cerr << e.what() << "\n";
        else
            std::cerr << tkw::cli::render_error_zh(e) << "\n";
    }
    return result.exit_code();
}
