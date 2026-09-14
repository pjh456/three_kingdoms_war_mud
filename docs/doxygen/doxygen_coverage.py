#!/usr/bin/env python3
"""Doxygen 注释静态预检（启发式，非权威）。

权威门禁是 `doxygen Doxyfile`（WARN_AS_ERROR=YES）。本脚本只做三类可靠度较高的
结构检查，便于在并行注释任务中快速定位遗漏，不能替代 Doxygen：

  1. 头文件缺少 `@file`。
  2. `/** ... */` 文档块缺少 `@brief`（允许 @copydoc/@defgroup/@file 例外）。
  3. 文档块 `@param` 数量与紧随其后的函数声明参数数量不一致。

用法：
    python3 docs/doxygen/doxygen_coverage.py [路径 ...] [--quiet]
退出码：发现任一问题返回 1，否则 0。
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

CONTROL = re.compile(
    r"^(if|for|while|switch|catch|else|return|do|case|default)\b"
)
DECL_START = re.compile(
    r"\b(class|struct|enum|namespace|using|typedef)\b"
)
# 文档块必须以这些之一开头才算"有 @brief 语义"
BRIEF_ALTERNATIVES = ("@brief", "@copydoc", "@defgroup", "@file", "@name")


def iter_doc_blocks(lines: list[str]):
    """产出 (kind, start, end, content)。kind: 'block' | 'inline'."""
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        stripped = line.lstrip()
        if stripped.startswith("/**<") or stripped.startswith("///<"):
            start = i
            while i < n and "*/" not in lines[i]:
                i += 1
            yield "inline", start, i, "\n".join(lines[start : i + 1])
        elif stripped.startswith("/**") or stripped.startswith("/*!"):
            start = i
            while i < n and "*/" not in lines[i]:
                i += 1
            yield "block", start, i, "\n".join(lines[start : i + 1])
        i += 1


def split_top_level(text: str) -> list[str]:
    """按顶层逗号切分（跟踪 <> () [] {} 深度）。"""
    parts, buf = [], []
    depth_round = depth_angle = depth_brace = depth_square = 0
    for ch in text:
        if ch == "," and not (depth_round or depth_angle or depth_brace or depth_square):
            parts.append("".join(buf))
            buf = []
            continue
        buf.append(ch)
        if ch == "(":
            depth_round += 1
        elif ch == ")":
            depth_round = max(0, depth_round - 1)
        elif ch == "{":
            depth_brace += 1
        elif ch == "}":
            depth_brace = max(0, depth_brace - 1)
        elif ch == "[":
            depth_square += 1
        elif ch == "]":
            depth_square = max(0, depth_square - 1)
        elif ch == "<":
            depth_angle += 1
        elif ch == ">":
            depth_angle = max(0, depth_angle - 1)
    parts.append("".join(buf))
    return parts


def extract_params(decl: str) -> list[str] | None:
    """从声明文本抽取顶层参数；无法解析返回 None。"""
    try:
        open_paren = decl.index("(")
    except ValueError:
        return None
    depth = 0
    close_paren = -1
    for idx in range(open_paren, len(decl)):
        ch = decl[idx]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                close_paren = idx
                break
    if close_paren < 0:
        return None
    body = decl[open_paren + 1 : close_paren].strip()
    if not body or body == "void":
        return []
    params = [p.strip() for p in split_top_level(body)]
    # 去掉可变参数占位
    return [p for p in params if p and p not in ("...",)]


def following_declaration(lines: list[str], start_after: int, limit: int = 24) -> str | None:
    """文档块之后、首个函数/类型声明文本。"""
    buf: list[str] = []
    for i in range(start_after, min(len(lines), start_after + limit)):
        line = lines[i]
        stripped = line.strip()
        if not stripped or stripped.startswith("//"):
            continue
        if stripped.startswith("#"):
            return None
        if stripped.startswith("/**") or stripped.startswith("/*"):
            return None
        buf.append(stripped)
        joined = " ".join(buf)
        if "(" in joined and (";" in joined or "{" in joined or joined.endswith(")")):
            return joined
    return " ".join(buf) if buf else None


def check_file(path: Path) -> list[str]:
    problems: list[str] = []
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return [f"{path}: 非 UTF-8，跳过"]

    lines = text.splitlines()
    if "@file" not in text:
        problems.append(f"{path}: 缺少 @file 文件头")

    for kind, start, end, content in iter_doc_blocks(lines):
        if kind == "inline":
            continue
        if not any(tok in content for tok in BRIEF_ALTERNATIVES):
            problems.append(f"{path}:{start + 1}: 文档块缺少 @brief")
            continue
        # @param 数量核对（仅当紧跟的是函数声明）
        decl = following_declaration(lines, end + 1)
        if not decl or DECL_START.search(decl):
            continue
        params = extract_params(decl)
        if params is None:
            continue
        doc_params = len(re.findall(r"@param\b", content))
        if len(params) != doc_params:
            problems.append(
                f"{path}:{start + 1}: @param 数量不符 "
                f"(声明 {len(params)} 个参数 / 文档 {doc_params} 个) -> {decl[:60]}"
            )
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description="Doxygen 注释静态预检")
    parser.add_argument("paths", nargs="*", default=["src"], help="要扫描的目录/文件")
    parser.add_argument("--quiet", action="store_true", help="仅打印汇总")
    args = parser.parse_args()

    files: list[Path] = []
    for raw in args.paths:
        p = Path(raw)
        if p.is_dir():
            files.extend(sorted(p.rglob("*.hpp")))
        elif p.is_file() and p.suffix == ".hpp":
            files.append(p)

    if not files:
        print("没有找到 .hpp 文件", file=sys.stderr)
        return 1

    all_problems: list[str] = []
    for f in files:
        all_problems.extend(check_file(f))

    if not args.quiet:
        for p in all_problems:
            print(p)

    print(f"\n扫描 {len(files)} 个头文件，发现 {len(all_problems)} 处问题。")
    if all_problems:
        print("提示：本脚本为启发式预检；权威门禁请运行 `doxygen Doxyfile`。")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
