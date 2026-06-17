# BuildThirdParty.py — 一键编译 ThirdParty 库
# 用法:
#   python Tools/BuildThirdParty.py              # 编译全部
#   python Tools/BuildThirdParty.py --target openssl  # 只编 OpenSSL
#   python Tools/BuildThirdParty.py --clean --jobs 8   # 清理重建 + 8 线程
#
# 产物:
#   A 组 (外部编译) → ThirdParty/Bin/{openssl,protobuf,dasScript,libpq}/
#   B 组 (xmake 编译) → fmt, tracy 由 xmake 直接从 ThirdParty/ 源码编译
#   C 组 (纯头文件) → asio, entt, spdlog, concurrentqueue
#
#  中间文件 → ThirdParty/Build/{openssl,protobuf,dasScript}/
#
# 换机器可用: 所有工具路径通过 Toolchain 自动发现，无硬编码。

import argparse
import multiprocessing
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional

# 修复 Windows GBK 终端编码问题
if sys.platform == "win32":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# 项目根目录的 Tools/ 加入 Python path (确保 Toolchain 可导入)
sys.path.insert(0, str(Path(__file__).resolve().parent))
from Toolchain import Toolchain, ToolStatus


# =============================================================================
# 配置
# =============================================================================

ROOT = Path(__file__).resolve().parent.parent
THIRD_PARTY = ROOT / "ThirdParty"
BUILD = THIRD_PARTY / "Build"          # 中间编译文件
BIN = THIRD_PARTY / "Bin"              # 最终安装产物

_IS_WINDOWS = platform.system() == "Windows"


class Colors:
    """ANSI 终端颜色"""
    RED = "\033[31m"
    GREEN = "\033[32m"
    YELLOW = "\033[33m"
    CYAN = "\033[36m"
    RESET = "\033[0m"
    BOLD = "\033[1m"


# =============================================================================
# 工具函数
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


def Run(cmd: list[str], cwd: Optional[Path] = None, env: Optional[dict] = None) -> None:
    """运行命令，失败则退出"""
    mergedEnv = os.environ.copy()
    if env:
        mergedEnv.update(env)

    LogStep(f"  $ {' '.join(str(c) for c in cmd)}")
    result = subprocess.run(cmd, cwd=str(cwd) if cwd else None, env=mergedEnv)
    if result.returncode != 0:
        LogError(f"命令失败 (exit code {result.returncode}): {' '.join(cmd)}")
        sys.exit(1)


def GetCpuCount() -> int:
    return multiprocessing.cpu_count()


def RmTree(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path, ignore_errors=True)


# =============================================================================
# 构建器
# =============================================================================


# ---- OpenSSL ----

class OpenSSLBuilder:

    @staticmethod
    def SourceDir() -> Path:
        return THIRD_PARTY / "openssl"

    @staticmethod
    def BuildDir() -> Path:
        return BUILD / "openssl"

    @staticmethod
    def InstallDir() -> Path:
        return BIN / "openssl"

    @staticmethod
    def CheckPrerequisites() -> bool:
        ok = True

        perlPath = Toolchain.FindPerl()
        if perlPath:
            LogStep(f"  perl → {perlPath}")
        else:
            LogError("找不到 Perl")
            print(f"     {Toolchain._PerlHint()}")
            ok = False

        nasmPath = Toolchain.FindNasm()
        if nasmPath:
            LogStep(f"  nasm → {nasmPath}")
        else:
            LogError("找不到 NASM")
            print(f"     {Toolchain._NasmHint()}")
            ok = False

        return ok

    @staticmethod
    def Build(jobs: int, clean: bool) -> None:
        src = OpenSSLBuilder.SourceDir()
        buildDir = OpenSSLBuilder.BuildDir()
        install = OpenSSLBuilder.InstallDir()

        if clean:
            LogStep("清理 OpenSSL 构建产物...")
            RmTree(buildDir)

        buildDir.mkdir(parents=True, exist_ok=True)
        install.mkdir(parents=True, exist_ok=True)

        if _IS_WINDOWS:
            OpenSSLBuilder._BuildWindows(src, install, buildDir, jobs)
        else:
            OpenSSLBuilder._BuildLinux(src, install, jobs)

    @staticmethod
    def _BuildWindows(src: Path, install: Path, buildDir: Path, jobs: int) -> None:
        LogStep("检测 MSVC 环境...")

        vsPath = Toolchain.FindVS()
        if vsPath is None:
            LogError("未找到 Visual Studio，OpenSSL 在 Windows 上必须用 MSVC 编译")
            LogError("请安装 Visual Studio 2022+ 并勾选「使用 C++ 的桌面开发」")
            sys.exit(1)

        vcvars = vsPath / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
        if not vcvars.exists():
            LogError(f"找不到 vcvars64.bat: {vcvars}")
            sys.exit(1)

        LogStep(f"  找到 VS: {vsPath}")

        perlExe = Toolchain.FindPerl()
        nasmExe = Toolchain.FindNasm()

        if perlExe is None:
            LogError("找不到 Strawberry Perl，无法编译 OpenSSL")
            sys.exit(1)
        if nasmExe is None:
            LogError("找不到 NASM，无法编译 OpenSSL")
            sys.exit(1)

        LogStep(f"  Perl: {perlExe}")
        LogStep(f"  NASM: {nasmExe}")

        pathExtra = str(perlExe.parent) + ";" + str(nasmExe.parent) + ";"

        installNative = str(install).replace("\\", "/")
        srcNative = str(src).replace("\\", "/")

        # OpenSSL 要求 in-tree 构建（Configure + nmake 都在源码目录）。
        # buildDir 只放临时 .bat 脚本，不用于构建。
        # 安装完成后用 nmake clean + 清理残余文件来保持源码目录干净。
        batScript = buildDir / "_build.bat"
        batContent = (
            f"@echo off\r\n"
            f'call "{vcvars}"\r\n'
            f"if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%\r\n"
            f'set "PATH={pathExtra}%PATH%"\r\n'
            f"\r\n"
            f"rem === OpenSSL: in-tree configure + build + install ===\r\n"
            f"cd /d {srcNative}\r\n"
            f"\r\n"
            f"echo [Configure] VC-WIN64A...\r\n"
            f"perl Configure VC-WIN64A --prefix={installNative} --release no-tests no-docs\r\n"
            f"if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%\r\n"
            f"\r\n"
            f"echo [Build] nmake...\r\n"
            f"nmake\r\n"
            f"if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%\r\n"
            f"\r\n"
            f"echo [Install] nmake install_sw...\r\n"
            f"nmake install_sw\r\n"
            f"if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%\r\n"
            f"\r\n"
            f"echo [Clean] 清理源码目录构建产物...\r\n"
            f"nmake clean 2>nul\r\n"
            f"del /s /q *.dll *.lib *.exp *.def *.ilk *.pdb *.res *.manifest Makefile 2>nul\r\n"
            f"del /s /q makefile makefile.in 2>nul\r\n"
            f"for /r %%i in (*.obj *.o *.d) do if exist \"%%i\" del /q \"%%i\" 2>nul\r\n"
            f"\r\n"
            f"echo [OK] OpenSSL build complete\r\n"
        )
        batScript.write_text(batContent, encoding="utf-8")

        LogStep("  运行 OpenSSL 构建脚本...")
        result = subprocess.run(
            ["cmd", "/c", str(batScript)],
            cwd=str(src),
        )
        if result.returncode != 0:
            LogError("OpenSSL 构建失败")
            sys.exit(1)

    @staticmethod
    def _BuildLinux(src: Path, install: Path, jobs: int) -> None:
        Run(
            [
                "./Configure",
                f"--prefix={install}",
                "--release",
                "no-tests",
                "no-docs",
            ],
            cwd=src,
        )
        Run(["make", f"-j{jobs}"], cwd=src)
        Run(["make", "install_sw"], cwd=src)
        # 清理源码目录构建产物
        Run(["make", "clean"], cwd=src)


# ---- Protobuf ----

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
    def CheckPrerequisites() -> bool:
        cmakePath = Toolchain.FindCMake()
        if cmakePath:
            LogStep(f"  cmake → {cmakePath}")
            return True
        else:
            LogError("找不到 CMake")
            print(f"     {Toolchain._CMakeHint()}")
            return False

    @staticmethod
    def Build(jobs: int, clean: bool) -> None:
        src = ProtobufBuilder.SourceDir()
        buildDir = ProtobufBuilder.BuildDir()
        install = ProtobufBuilder.InstallDir()

        if clean:
            LogStep("清理 Protobuf 构建产物...")
            RmTree(buildDir)

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
        Run([
            "cmake", "--build", str(buildDir),
            "--config", "Release",
            f"--parallel", str(jobs),
        ])
        Run([
            "cmake", "--install", str(buildDir),
            "--config", "Release",
        ])


# ---- daScript ----

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
    def CheckPrerequisites() -> bool:
        cmakePath = Toolchain.FindCMake()
        if cmakePath:
            LogStep(f"  cmake → {cmakePath}")
            return True
        else:
            LogError("找不到 CMake")
            print(f"     {Toolchain._CMakeHint()}")
            return False

    @staticmethod
    def Build(jobs: int, clean: bool) -> None:
        src = DaScriptBuilder.SourceDir()
        buildDir = DaScriptBuilder.BuildDir()
        install = DaScriptBuilder.InstallDir()

        if clean:
            LogStep("清理 daScript 构建产物...")
            RmTree(buildDir)

        buildDir.mkdir(parents=True, exist_ok=True)
        install.mkdir(parents=True, exist_ok=True)

        cmakeArgs = [
            "cmake",
            "-B", str(buildDir),
            "-S", str(src),
            f"-DCMAKE_INSTALL_PREFIX={install}",
            "-DCMAKE_BUILD_TYPE=Release",
            # 关闭不需要的模块
            "-DDAS_CLANG_BIND_DISABLED=ON",
            "-DDAS_LLVM_DISABLED=ON",
            "-DDAS_SQLITE_DISABLED=ON",
            "-DDAS_TESTS_DISABLED=ON",
            "-DDAS_TUTORIAL_DISABLED=ON",
            "-DDAS_AOT_EXAMPLES_DISABLED=ON",
            "-DDAS_TOOLS_DISABLED=ON",
            "-DDAS_GLFW_DISABLED=ON",
            "-DDAS_AUDIO_DISABLED=ON",
            "-DDAS_STDDLG_DISABLED=ON",
            "-DDAS_STBIMAGE_DISABLED=ON",
            "-DDAS_HV_DISABLED=ON",
            # flex/bison 代码已预生成在 repo
            "-DDAS_FLEX_BISON_DISABLED=ON",
        ]

        if _IS_WINDOWS:
            cmakeArgs.append("-Dprotobuf_MSVC_STATIC_RUNTIME=OFF")

        Run(cmakeArgs, cwd=src)
        Run([
            "cmake", "--build", str(buildDir),
            "--config", "Release",
            f"--parallel", str(jobs),
        ])
        Run([
            "cmake", "--install", str(buildDir),
            "--config", "Release",
        ])


# ---- Libpq (从系统 PostgreSQL 拷贝到本地管理) ----

class LibpqBuilder:

    @staticmethod
    def InstallDir() -> Path:
        return BIN / "libpq"

    @staticmethod
    def CheckPrerequisites() -> bool:
        pgDir = Toolchain.FindLibpqInstallDir()
        if pgDir:
            LogStep(f"  PostgreSQL → {pgDir}")
            return True
        else:
            LogWarn("  未找到 PostgreSQL，libpq 将跳过")
            return False

    @staticmethod
    def Build(jobs: int, clean: bool) -> None:
        pgDir = Toolchain.FindLibpqInstallDir()
        if pgDir is None:
            LogWarn("跳过 libpq：未找到 PostgreSQL 安装。")
            return

        install = LibpqBuilder.InstallDir()
        includeDir = install / "include"

        if clean:
            RmTree(install)

        install.mkdir(parents=True, exist_ok=True)
        includeDir.mkdir(parents=True, exist_ok=True)

        pgInclude = pgDir / "include"

        # ---- 拷贝客户端头文件 ----
        LogStep("拷贝 libpq 客户端头文件...")
        clientHeaders = [
            "libpq-fe.h",
            "libpq-events.h",
            "postgres_ext.h",
            "pg_config.h",
            "pg_config_manual.h",
            "pg_config_os.h",
        ]
        for h in clientHeaders:
            src = pgInclude / h
            if src.exists():
                shutil.copy2(src, includeDir / h)

        # libpq/ 子目录（内部类型定义，libpq-fe.h 会 include）
        libpqSubdir = pgInclude / "libpq"
        if libpqSubdir.is_dir():
            dstSubdir = includeDir / "libpq"
            dstSubdir.mkdir(exist_ok=True)
            for f in libpqSubdir.iterdir():
                if f.is_file():
                    shutil.copy2(f, dstSubdir / f.name)

        # ---- 拷贝静态库 ----
        LogStep("拷贝 libpq 静态库...")
        libDir = install / "lib"
        libDir.mkdir(parents=True, exist_ok=True)

        pqLib = pgDir / "lib" / "libpq.lib"
        if pqLib.exists():
            shutil.copy2(pqLib, libDir / "libpq.lib")

        LogStep(f"  libpq → {install}")


# =============================================================================
# 主流程
# =============================================================================


def CheckPrerequisites(targets: list[str]) -> bool:
    """用 Toolchain.Diagnose() 统一检查所有工具链"""
    LogHeader("检查构建环境")

    statuses = Toolchain.Diagnose()

    for s in statuses:
        if s.found:
            LogStep(f"  {s.name} → {s.version or s.path}")
        else:
            LogWarn(f"  {s.name} → 未找到")
            if s.hint:
                for line in s.hint.split("\n"):
                    print(f"     {line}")

    requiredTools: set[str] = set()
    if "openssl" in targets:
        requiredTools.update(["perl", "nasm"])
        if _IS_WINDOWS:
            requiredTools.add("Visual Studio")
    if "protobuf" in targets or "dasScript" in targets:
        requiredTools.add("cmake")

    statusMap = {s.name: s for s in statuses}
    allOk = True
    for name in requiredTools:
        s = statusMap.get(name)
        if s and not s.found:
            LogError(f"  {name} 是必需的，请安装后重试")
            allOk = False

    return allOk


def BuildTargets(targets: list[str], jobs: int, clean: bool) -> None:
    builders: dict[str, object] = {
        "openssl": OpenSSLBuilder,
        "protobuf": ProtobufBuilder,
        "dasScript": DaScriptBuilder,
        "libpq": LibpqBuilder,
    }

    total = len(targets)
    for i, name in enumerate(targets, 1):
        LogHeader(f"[{i}/{total}] 构建 {name}")
        builder = builders[name]
        builder.Build(jobs, clean)

    LogHeader("构建完成")
    LogStep(f"产物路径: {BIN}")
    _PrintBinTree()


def _PrintBinTree() -> None:
    """打印 Bin/ 目录结构"""
    if not BIN.exists():
        return

    print()
    for entry in sorted(BIN.iterdir()):
        if entry.is_dir():
            hCount = 0
            if (entry / "include").exists():
                hCount = len(list((entry / "include").rglob("*.h")))
            libCount = 0
            if (entry / "lib").exists():
                libCount = sum(
                    1 for f in (entry / "lib").iterdir()
                    if f.suffix in (".lib", ".a")
                )
            print(f"  {entry.name}/")
            if hCount:
                print(f"    include/  ({hCount} headers)")
            if libCount:
                print(f"    lib/      ({libCount} libraries)")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="一键编译 ThirdParty 库 → ThirdParty/Bin/"
    )
    parser.add_argument(
        "--target",
        nargs="+",
        choices=["openssl", "protobuf", "dasScript", "libpq"],
        default=["openssl", "protobuf", "dasScript", "libpq"],
        help="要编译的目标 (默认: 全部)",
    )
    parser.add_argument(
        "--jobs", "-j",
        type=int,
        default=GetCpuCount(),
        help=f"并行编译线程数 (默认: {GetCpuCount()})",
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="清理中间构建目录后重新构建",
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
    print(f"  清理重建: {'是' if args.clean else '否'}")
    print()

    BIN.mkdir(parents=True, exist_ok=True)

    if not args.skip_check:
        if not CheckPrerequisites(args.target):
            LogError("前置检查失败——请安装缺失的工具后重试")
            sys.exit(1)

    BuildTargets(args.target, args.jobs, args.clean)


if __name__ == "__main__":
    main()
