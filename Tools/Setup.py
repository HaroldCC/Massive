# Setup.py — 新机器环境诊断 & 安装指引
# 用法:
#   python Tools/Setup.py          # 全面诊断
#   python Tools/Setup.py --check  # 仅检查 (exit code 反映结果)
#   python Tools/Setup.py --json   # JSON 输出 (CI 集成)
#
# 这是新机器拉取代码后的第一步——不编译任何东西，只告诉你缺什么。

import argparse
import json
import os
import platform
import sys
from dataclasses import asdict
from pathlib import Path

# 修复 Windows GBK 终端编码问题
if sys.platform == "win32":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# 项目根目录的 Tools/ 加入 Python path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from Toolchain import Toolchain, ToolStatus


class Colors:
    RED = "\033[31m"
    GREEN = "\033[32m"
    YELLOW = "\033[33m"
    CYAN = "\033[36m"
    MAGENTA = "\033[35m"
    RESET = "\033[0m"
    BOLD = "\033[1m"


def Diagnose() -> list[ToolStatus]:
    """全面诊断"""
    print(f"\n{Colors.BOLD}{Colors.CYAN}{'=' * 60}{Colors.RESET}")
    print(f"{Colors.BOLD}{Colors.CYAN}  Massive 环境诊断{Colors.RESET}")
    print(f"{Colors.BOLD}{Colors.CYAN}{'=' * 60}{Colors.RESET}\n")
    print(f"  操作系统: {platform.system()} {platform.release()}")
    print(f"  Python:   {sys.version.split()[0]}")
    print(f"  工作目录: {Path.cwd()}")
    print()

    statuses = Toolchain.Diagnose()

    okCount = sum(1 for s in statuses if s.found)
    failCount = len(statuses) - okCount

    for s in statuses:
        _PrintStatus(s)

    print(f"\n{Colors.BOLD}{'─' * 60}{Colors.RESET}")
    if failCount == 0:
        print(f"{Colors.GREEN}{Colors.BOLD}  ✓ 所有工具就绪 ({okCount}/{len(statuses)}){Colors.RESET}")
        print(f"  下一步: python Tools/BuildThirdParty.py")
    else:
        print(f"{Colors.YELLOW}{Colors.BOLD}  ⚠ {failCount} 个工具缺失，请按上述指引安装{Colors.RESET}")

    print()

    return statuses


def _PrintStatus(s: ToolStatus) -> None:
    """打印单个工具状态"""
    icon = f"{Colors.GREEN}✓{Colors.RESET}" if s.found else f"{Colors.RED}✗{Colors.RESET}"
    pathStr = str(s.path) if s.path else ""
    versionStr = f" → {s.version}" if s.version else ""

    print(f"  {icon} {s.name:<18} {Colors.CYAN}{pathStr}{Colors.RESET}{versionStr}")

    if not s.found and s.hint:
        for line in s.hint.split("\n"):
            print(f"     {Colors.YELLOW}{line}{Colors.RESET}")
        print()


def DiagnoseJSON() -> str:
    """JSON 格式输出 (CI 集成用)"""
    statuses = Toolchain.Diagnose()
    result = {
        "platform": platform.system(),
        "release": platform.release(),
        "tools": [
            {
                "name": s.name,
                "found": s.found,
                "path": str(s.path) if s.path else None,
                "version": s.version,
                "hint": s.hint,
            }
            for s in statuses
        ],
        "allPassed": all(s.found for s in statuses),
    }
    return json.dumps(result, indent=2, ensure_ascii=False)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Massive 新机器环境诊断 — 检查工具链是否就绪"
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="静默检查模式: 通过则 exit 0，缺失工具则 exit 1",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="以 JSON 格式输出 (CI 集成)",
    )
    args = parser.parse_args()

    if args.json:
        print(DiagnoseJSON())
        sys.exit(0)
    else:
        statuses = Diagnose()

    if args.check:
        allOk = all(s.found for s in statuses)
        sys.exit(0 if allOk else 1)


if __name__ == "__main__":
    main()
