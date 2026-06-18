# BuildThirdParty.py — 一键编译 ThirdParty 库
#
# 用法:
#   python Tools/ThirdParty/BuildThirdParty.py               # 编译全部
#   python Tools/ThirdParty/BuildThirdParty.py --target protobuf  # 只编 Protobuf
#
# 说明:
#   OpenSSL 和 libpq 的 Windows 二进制是手动管理的 vendored 文件，
#   已直接提交到 ThirdParty/Bin/{openssl,libpq}/ 中，无需编译。
#   本脚本只负责需要 CMake 编译的 Protobuf 和 daScript。
#
# 增量编译:
#   构建完成后 ThirdParty/Build/<target>/.done 标记写入
#   下次同一目标直接跳过 (--force 强制重建)
#
# 产物:
#   ThirdParty/Bin/{protobuf,dasScript}/
#   ThirdParty/Build/{protobuf,dasScript}/  (中间文件)
#   ThirdParty/Build/{protobuf,dasScript}/.done  (增量标记)

import argparse
import multiprocessing
import platform
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional

# 修复 Windows GBK 终端编码问题
if sys.platform == "win32":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# =============================================================================
# 路径常量
# =============================================================================

ROOT = Path(__file__).resolve().parent.parent.parent
THIRD_PARTY = ROOT / "ThirdParty"
BUILD = THIRD_PARTY / "Build"          # 中间编译文件
BIN = THIRD_PARTY / "Bin"              # 最终安装产物

_IS_WINDOWS = platform.system() == "Windows"


# =============================================================================
# 终端颜色
# =============================================================================

class Colors:
    RED = "\033[31m"
    GREEN = "\033[32m"
    YELLOW = "\033[33m"
    CYAN = "\033[36m"
    RESET = "\033[0m"
    BOLD = "\033[1m"


# =============================================================================
# 日志
# =============================================================================

def LogHeader(msg: str) -> None:
    print(f"\n{Colors.BOLD}{Colors.CYAN}{'=' * 60}{Colors.RESET}")
    print(f"{Colors.BOLD}{Colors.CYAN}  {msg}{Colors.RESET}")
    print(f"{Colors.BOLD}{Colors.CYAN}{'=' * 60}{Colors.RESET}\n")


def LogStep(msg: str) -> None:
    print(f"{Colors.GREEN}[+] {msg}{Colors.RESET}")


def LogWarn(msg: str) -> None:
    print(f"{Colors.YELLOW}[!] {msg}{Colors.RESET}")


def LogError(msg: str) -> None:
    print(f"{Colors.RED}[X] {msg}{Colors.RESET}")


# =============================================================================
# 工具函数
# =============================================================================

def GetCpuCount() -> int:
    return multiprocessing.cpu_count()


def RmTree(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path, ignore_errors=True)


def Run(cmd: list[str], cwd: Optional[Path] = None) -> None:
    """运行命令，失败则退出"""
    LogStep(f"  $ {' '.join(str(c) for c in cmd)}")
    result = subprocess.run(cmd, cwd=str(cwd) if cwd else None)
    if result.returncode != 0:
        LogError(f"命令失败 (exit code {result.returncode}): {' '.join(cmd)}")
        sys.exit(1)


# =============================================================================
# 增量编译标记
# =============================================================================

def IsBuilt(buildDir: Path) -> bool:
    return (buildDir / ".done").exists()


def MarkBuilt(buildDir: Path) -> None:
    buildDir.mkdir(parents=True, exist_ok=True)
    (buildDir / ".done").write_text("", encoding="utf-8")


def SkipOrRebuild(name: str, buildDir: Path, force: bool) -> bool:
    """检查是否需要跳过构建。返回 True = 跳过。"""
    if force:
        LogStep(f"  强制重建 {name}，清理中间目录...")
        RmTree(buildDir)
        return False

    if IsBuilt(buildDir):
        LogStep(f"  {name} 已构建完成 (--force 可强制重建)，跳过...")
        return True

    return False


# =============================================================================
# 工具发现
# =============================================================================

def FindCMake() -> Optional[Path]:
    """查找 CMake（系统 PATH）"""
    cmake = shutil.which("cmake")
    return Path(cmake) if cmake else None


# =============================================================================
# 检查环境
# =============================================================================

def CheckPrerequisites(targets: list[str]) -> bool:
    """检查 CMake 是否就绪"""
    LogHeader("检查构建环境")
    allOk = True

    cmakePath = FindCMake()
    if cmakePath:
        LogStep(f"  CMake → {cmakePath}")
    else:
        LogError("找不到 CMake（Protobuf/daScript 必需）")
        if _IS_WINDOWS:
            print("    https://cmake.org/download/  → Windows x64 Installer")
            print("    安装时勾选 Add CMake to system PATH")
        else:
            print("    sudo apt install cmake")
        allOk = False

    return allOk


# =============================================================================
# 构建器: Protobuf
# =============================================================================

class ProtobufBuilder:

    @staticmethod
    def SourceDir() -> Path:
        return THIRD_PARTY / "protobuf"

    @staticmethod
    def BuildDir() -> Path:
        return BUILD / "protobuf"

    @staticmethod
    def InstallDir() -> Path:
        return BIN / "protobuf"

    @staticmethod
    def Build(jobs: int, force: bool) -> None:
        buildDir = ProtobufBuilder.BuildDir()
        if SkipOrRebuild("Protobuf", buildDir, force):
            return

        src = ProtobufBuilder.SourceDir()
        install = ProtobufBuilder.InstallDir()
        buildDir.mkdir(parents=True, exist_ok=True)
        install.mkdir(parents=True, exist_ok=True)

        cmakeArgs = [
            "cmake",
            "-B", str(buildDir),
            "-S", str(src),
            f"-DCMAKE_INSTALL_PREFIX={install}",
            "-DCMAKE_BUILD_TYPE=Release",
            "-Dprotobuf_BUILD_TESTS=OFF",
            "-Dprotobuf_BUILD_EXAMPLES=OFF",
            "-Dprotobuf_BUILD_PROTOC_BINARIES=ON",
            "-Dprotobuf_BUILD_LIBPROTOC=ON",
            "-Dprotobuf_WITH_ZLIB=OFF",
            "-Dprotobuf_ABSL_PROVIDER=module",
        ]

        if _IS_WINDOWS:
            cmakeArgs.append("-Dprotobuf_MSVC_STATIC_RUNTIME=OFF")

        Run(cmakeArgs, cwd=src)
        Run(["cmake", "--build", str(buildDir), "--config", "Release", "--parallel", str(jobs)])
        Run(["cmake", "--install", str(buildDir), "--config", "Release"])

        MarkBuilt(buildDir)


# =============================================================================
# 构建器: daScript
# =============================================================================

class DaScriptBuilder:

    @staticmethod
    def SourceDir() -> Path:
        return THIRD_PARTY / "daScript"

    @staticmethod
    def BuildDir() -> Path:
        return BUILD / "dasScript"

    @staticmethod
    def InstallDir() -> Path:
        return BIN / "dasScript"

    @staticmethod
    def Build(jobs: int, force: bool) -> None:
        buildDir = DaScriptBuilder.BuildDir()
        if SkipOrRebuild("daScript", buildDir, force):
            return

        src = DaScriptBuilder.SourceDir()
        install = DaScriptBuilder.InstallDir()
        buildDir.mkdir(parents=True, exist_ok=True)
        install.mkdir(parents=True, exist_ok=True)

        cmakeArgs = [
            "cmake",
            "-B", str(buildDir),
            "-S", str(src),
            f"-DCMAKE_INSTALL_PREFIX={install}",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DDAS_CLANG_BIND_DISABLED=ON",
            "-DDAS_LLVM_DISABLED=ON",
            "-DDAS_SQLITE_DISABLED=ON",
            "-DDAS_TESTS_DISABLED=ON",
            "-DDAS_TUTORIAL_DISABLED=ON",
            "-DDAS_AOT_EXAMPLES_DISABLED=ON",
            # daslang 编译器保留（DAS_TOOLS=ON 默认），跳过 example 运行
            # DAS_TOOLS_DISABLED 不能设为 ON，否则 run_examples target 引用的
            # $<TARGET_FILE:daslang> 在 configure 阶段会报错
            "-DDAS_GLFW_DISABLED=ON",
            "-DDAS_AUDIO_DISABLED=ON",
            "-DDAS_STDDLG_DISABLED=ON",
            "-DDAS_STBIMAGE_DISABLED=ON",
            "-DDAS_HV_DISABLED=ON",
            "-DDAS_FLEX_BISON_DISABLED=ON",
        ]

        Run(cmakeArgs, cwd=src)
        Run(["cmake", "--build", str(buildDir), "--config", "Release", "--parallel", str(jobs)])
        Run(["cmake", "--install", str(buildDir), "--config", "Release"])

        MarkBuilt(buildDir)


# =============================================================================
# 产物结构展示
# =============================================================================

def _PrintBinTree() -> None:
    """打印 Bin/ 目录结构"""
    if not BIN.exists():
        return

    print()
    for entry in sorted(BIN.iterdir()):
        if entry.is_dir() and entry.name not in ("openssl", "libpq"):
            hCount = 0
            if (entry / "include").exists():
                hCount = len(list((entry / "include").rglob("*.h")))
            libCount = 0
            libDir = entry / "lib"
            if libDir.exists():
                libCount = sum(1 for f in libDir.iterdir() if f.suffix in (".lib", ".a"))
            print(f"  {entry.name}/")
            if hCount:
                print(f"    include/  ({hCount} headers)")
            if libCount:
                print(f"    lib/      ({libCount} libraries)")


# =============================================================================
# 主流程
# =============================================================================

def BuildTargets(targets: list[str], jobs: int, force: bool) -> None:
    builders: dict[str, object] = {
        "protobuf": ProtobufBuilder,
        "dasScript": DaScriptBuilder,
    }

    total = len(targets)
    for i, name in enumerate(targets, 1):
        LogHeader(f"[{i}/{total}] 构建 {name}")
        builder = builders[name]
        builder.Build(jobs, force)

    LogHeader("构建完成")
    LogStep(f"产物路径: {BIN}")
    _PrintBinTree()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="一键编译 ThirdParty 库 → ThirdParty/Bin/",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "说明: OpenSSL 和 libpq 的 Windows 二进制为 vendored 文件无需编译。\n"
            "本脚本仅编译 Protobuf 和 daScript。\n"
            "\n"
            "示例:\n"
            "  %(prog)s                          # 编译全部\n"
            "  %(prog)s --target protobuf        # 只编 Protobuf\n"
            "  %(prog)s --target dasScript --force  # 强制重建 daScript\n"
        ),
    )
    parser.add_argument(
        "--target",
        nargs="+",
        choices=["protobuf", "dasScript"],
        default=["protobuf", "dasScript"],
        help="要编译的目标 (默认: 全部)",
    )
    parser.add_argument(
        "--jobs", "-j",
        type=int,
        default=GetCpuCount(),
        help=f"并行编译线程数 (默认: {GetCpuCount()})",
    )
    parser.add_argument(
        "--force", "-f",
        action="store_true",
        help="强制清理中间目录后重新构建",
    )
    parser.add_argument(
        "--skip-check",
        action="store_true",
        help="跳过前置工具检查",
    )

    args = parser.parse_args()

    print(f"{Colors.BOLD}Massive ThirdParty Builder{Colors.RESET}")
    print(f"  目标:     {', '.join(args.target)}")
    print(f"  并行数:   {args.jobs}")
    print(f"  强制重建: {'是' if args.force else '否'}")
    print()

    BIN.mkdir(parents=True, exist_ok=True)

    if not args.skip_check:
        if not CheckPrerequisites(args.target):
            LogError("前置检查失败——请安装 CMake 后重试")
            sys.exit(1)

    BuildTargets(args.target, args.jobs, args.force)


if __name__ == "__main__":
    main()
