# Toolchain.py — 跨平台工具链发现
# 用法:
#   from Toolchain import Toolchain
#   perl = Toolchain.FindPerl()     # → Path or None
#   nasm = Toolchain.FindNasm()
#   vs   = Toolchain.FindVS()
#
# 查找策略: PATH → 常见安装目录 → None (+ 安装指引)

import os
import platform
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

# 修复 Windows GBK 终端编码问题
if sys.platform == "win32":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")


@dataclass
class ToolStatus:
    """单个工具的发现结果"""
    name: str            # 工具名 (perl, nasm, cmake, ...)
    found: bool
    path: Optional[Path]
    version: str         # --version 第一行
    hint: str            # 未找到时的安装指引


class Toolchain:
    """跨平台工具链发现——不依赖任何硬编码路径"""

    _IS_WINDOWS = platform.system() == "Windows"
    _IS_LINUX = platform.system() == "Linux"

    # =========================================================================
    # 公共 API
    # =========================================================================

    @classmethod
    def FindPerl(cls) -> Optional[Path]:
        """查找 Perl 解释器。
        Windows: 优先 Strawberry Perl (MSVC 兼容)，排除 Git MSYS2 Perl。
        Linux: 系统 Perl 即可。
        """
        if cls._IS_WINDOWS:
            return cls._FindWindowsPerl()
        else:
            return cls._FindTool("perl", [])

    @classmethod
    def FindNasm(cls) -> Optional[Path]:
        """查找 NASM 汇编器"""
        hints = [
            "C:\\Program Files\\NASM\\nasm.exe",
            "C:\\Program Files (x86)\\NASM\\nasm.exe",
        ] if cls._IS_WINDOWS else []
        return cls._FindTool("nasm", hints)

    @classmethod
    def FindVS(cls) -> Optional[Path]:
        """查找 Visual Studio 安装路径 (Windows only)"""
        if not cls._IS_WINDOWS:
            return None
        return cls._FindVisualStudio()

    @classmethod
    def FindCMake(cls) -> Optional[Path]:
        """查找 CMake"""
        hints = [
            "C:\\Program Files\\CMake\\bin\\cmake.exe",
            "C:\\Program Files (x86)\\CMake\\bin\\cmake.exe",
        ] if cls._IS_WINDOWS else []
        return cls._FindTool("cmake", hints)

    @classmethod
    def FindLibpq(cls) -> Optional[dict[str, str]]:
        """查找 PostgreSQL libpq (头文件 + 库)。
        返回 dict: {"include": "/path/to/include", "lib": "/path/to/lib"} 或 None
        """
        if cls._IS_WINDOWS:
            return cls._FindWindowsLibpq()
        else:
            return cls._FindLinuxLibpq()

    @classmethod
    def Diagnose(cls) -> list[ToolStatus]:
        """全面诊断：返回所有工具的 ToolStatus 列表"""
        results: list[ToolStatus] = []

        # Perl
        perlPath = cls.FindPerl()
        results.append(ToolStatus(
            name="perl",
            found=perlPath is not None,
            path=perlPath,
            version=cls._GetVersion(perlPath, ["--version"]) if perlPath else "",
            hint=cls._PerlHint(),
        ))

        # NASM
        nasmPath = cls.FindNasm()
        results.append(ToolStatus(
            name="nasm",
            found=nasmPath is not None,
            path=nasmPath,
            version=cls._GetVersion(nasmPath, ["--version"]) if nasmPath else "",
            hint=cls._NasmHint(),
        ))

        # CMake
        cmakePath = cls.FindCMake()
        results.append(ToolStatus(
            name="cmake",
            found=cmakePath is not None,
            path=cmakePath,
            version=cls._GetVersion(cmakePath, ["--version"]) if cmakePath else "",
            hint=cls._CMakeHint(),
        ))

        # Visual Studio (Windows only)
        if cls._IS_WINDOWS:
            vsPath = cls.FindVS()
            results.append(ToolStatus(
                name="Visual Studio",
                found=vsPath is not None,
                path=vsPath,
                version=cls._GetVSVersion(vsPath) if vsPath else "",
                hint="安装 Visual Studio 2022+ 并勾选「使用 C++ 的桌面开发」\n    https://visualstudio.microsoft.com/downloads/",
            ))

        # libpq
        pqInfo = cls.FindLibpq()
        results.append(ToolStatus(
            name="libpq",
            found=pqInfo is not None,
            path=None,
            version=str(pqInfo) if pqInfo else "",
            hint=cls._LibpqHint(),
        ))

        return results

    # =========================================================================
    # 内部: 通用工具发现
    # =========================================================================

    @classmethod
    def _FindTool(cls, name: str, extraHints: list[str]) -> Optional[Path]:
        """查找可执行文件: PATH → 提示路径 → None"""
        # 1. PATH
        found = shutil.which(name)
        if found:
            return Path(found)

        # 2. 常见安装目录
        for hint in extraHints:
            p = Path(hint)
            if p.exists():
                return p

        return None

    @classmethod
    def _GetVersion(cls, exe: Optional[Path], args: list[str]) -> str:
        """获取工具版本号 (第一行输出)"""
        if exe is None:
            return ""
        try:
            result = subprocess.run(
                [str(exe)] + args,
                capture_output=True, text=True, timeout=10,
            )
            lines = result.stdout.strip().split("\n")
            return lines[0] if lines else ""
        except Exception:
            return ""

    # =========================================================================
    # Windows: Perl
    # =========================================================================

    @classmethod
    def _FindWindowsPerl(cls) -> Optional[Path]:
        """在 Windows 上查找 Perl，必须排除 Git MSYS2 Perl。
        MSYS2 Perl 缺少 Locale::Maketext::Simple 等模块，
        OpenSSL Configure 无法运行。
        """
        # Strawberry Perl 常见路径
        strawberryHints = [
            "C:\\Strawberry\\perl\\bin\\perl.exe",
            "C:\\Strawberry\\c\\bin\\perl.exe",   # 旧版
        ]

        # 额外扫描所有盘符根目录 (D:\Strawberry\, E:\... 等)
        for drive in cls._WindowsDrives():
            d = f"{drive}:\\"
            for sub in ["Strawberry\\perl\\bin\\perl.exe",
                        "Applications\\Perl\\perl\\bin\\perl.exe",
                        "Perl64\\bin\\perl.exe"]:
                p = Path(d + sub)
                if p.exists():
                    strawberryHints.append(str(p))

        # 1. PATH 中找 perl，但检查是否为 MSYS2 版本
        pathPerl = shutil.which("perl")
        if pathPerl and cls._IsStrawberryPerl(Path(pathPerl)):
            return Path(pathPerl)

        # 2. 扫描 Strawberry Perl 路径
        for hint in strawberryHints:
            p = Path(hint)
            if p.exists() and cls._IsStrawberryPerl(p):
                return p

        # 3. 如果 PATH 中的 perl 不是 Strawberry 但也没别的选择，检测并警告
        if pathPerl:
            return Path(pathPerl)  # 回退，让调用方决定

        return None

    @staticmethod
    def _IsStrawberryPerl(perlExe: Path) -> bool:
        """判断是否为 Strawberry Perl（检查是否有 MSVC 兼容性）"""
        try:
            result = subprocess.run(
                [str(perlExe), "-e", "use Locale::Maketext::Simple; print 'OK'"],
                capture_output=True, text=True, timeout=10,
            )
            return result.stdout.strip() == "OK"
        except Exception:
            return False

    # =========================================================================
    # Windows: Visual Studio
    # =========================================================================

    @classmethod
    def _FindVisualStudio(cls) -> Optional[Path]:
        """通过 vswhere 查找 Visual Studio"""
        vswherePaths = [
            "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe",
            "C:/Program Files/Microsoft Visual Studio/Installer/vswhere.exe",
        ]
        vswhere = None
        for p in vswherePaths:
            if Path(p).exists():
                vswhere = p
                break

        if vswhere is None:
            return None

        try:
            result = subprocess.run(
                [vswhere, "-latest", "-property", "installationPath"],
                capture_output=True, text=True, timeout=10,
            )
            vsPath = result.stdout.strip()
            if vsPath:
                return Path(vsPath)
        except Exception:
            pass

        return None

    @classmethod
    def _GetVSVersion(cls, vsPath: Optional[Path]) -> str:
        """从 VS 路径获取版本号"""
        if vsPath is None:
            return ""
        # 尝试从 vcvars 或版本文件获取
        versionFile = vsPath / "VC" / "Auxiliary" / "Build" / "Microsoft.VCToolsVersion.default.txt"
        if versionFile.exists():
            try:
                return versionFile.read_text().strip()
            except Exception:
                pass
        return vsPath.name

    # =========================================================================
    # libpq 发现
    # =========================================================================

    @classmethod
    def FindLibpqInstallDir(cls) -> Optional[Path]:
        """查找 PostgreSQL 安装根目录。
        返回 PostgreSQL 安装目录（如 D:/Applications/PostgreSQL/18），
        调用方可自行组合 include/ lib/ 子路径。
        """
        if cls._IS_WINDOWS:
            return cls._FindWindowsPostgresqlDir()
        else:
            return cls._FindLinuxPostgresqlDir()

    @classmethod
    def _FindWindowsPostgresqlDir(cls) -> Optional[Path]:
        """在 Windows 上查找 PostgreSQL 安装目录"""
        # 已知安装目录（优先匹配）
        knownDirs = [
            "D:\\Applications\\PostgreSQL\\18",
            "D:\\Applications\\PostgreSQL\\17",
            "D:\\Applications\\PostgreSQL\\16",
            "C:\\Program Files\\PostgreSQL\\18",
            "C:\\Program Files\\PostgreSQL\\17",
            "C:\\Program Files\\PostgreSQL\\16",
            "C:\\Program Files (x86)\\PostgreSQL\\18",
            "C:\\Program Files (x86)\\PostgreSQL\\17",
            "C:\\Program Files (x86)\\PostgreSQL\\16",
        ]
        for d in knownDirs:
            p = Path(d)
            if p.exists() and (p / "lib" / "libpq.lib").exists():
                return p

        # 环境变量
        envDir = os.environ.get("PostgreSQL_ROOT") or os.environ.get("PGROOT")
        if envDir:
            p = Path(envDir)
            if p.exists() and (p / "lib" / "libpq.lib").exists():
                return p

        # C:/Program Files/PostgreSQL/ 下扫描版本号目录
        for base in ["C:\\Program Files\\PostgreSQL", "C:\\Program Files (x86)\\PostgreSQL"]:
            bp = Path(base)
            if bp.exists():
                versions = sorted([d for d in bp.iterdir() if d.is_dir()], reverse=True)
                for v in versions:
                    if (v / "lib" / "libpq.lib").exists():
                        return v

        return None

    @classmethod
    def _FindLinuxPostgresqlDir(cls) -> Optional[Path]:
        """在 Linux 上查找 PostgreSQL 安装目录"""
        pgConfig = shutil.which("pg_config")
        if pgConfig:
            try:
                bindir = subprocess.run(
                    [pgConfig, "--bindir"], capture_output=True, text=True,
                ).stdout.strip()
                if bindir:
                    return Path(bindir).parent
            except Exception:
                pass
        return None

    @classmethod
    def _FindWindowsLibpq(cls) -> Optional[dict[str, str]]:
        """在 Windows 上查找 PostgreSQL libpq"""
        pgDir = cls.FindLibpqInstallDir()
        if pgDir:
            include = pgDir / "include"
            lib = pgDir / "lib"
            if include.exists() and lib.exists():
                return {"include": str(include), "lib": str(lib)}

        # 环境变量
        pqLibDir = os.environ.get("PQ_LIB_DIR")
        pqIncludeDir = os.environ.get("PQ_INCLUDE_DIR")
        if pqLibDir and pqIncludeDir:
            return {"include": pqIncludeDir, "lib": pqLibDir}

        return None

    @classmethod
    def _FindLinuxLibpq(cls) -> Optional[dict[str, str]]:
        """在 Linux 上查找 libpq"""
        # pg_config 是最可靠的方式
        pgConfig = shutil.which("pg_config")
        if pgConfig:
            try:
                include = subprocess.run(
                    [pgConfig, "--includedir"], capture_output=True, text=True,
                ).stdout.strip()
                lib = subprocess.run(
                    [pgConfig, "--libdir"], capture_output=True, text=True,
                ).stdout.strip()
                if include and lib:
                    return {"include": include, "lib": lib}
            except Exception:
                pass

        # 常见系统路径
        for includeDir in ["/usr/include/postgresql", "/usr/include", "/usr/local/include"]:
            p = Path(includeDir) / "libpq-fe.h"
            if p.exists():
                for libDir in ["/usr/lib/x86_64-linux-gnu", "/usr/lib", "/usr/local/lib"]:
                    libFile = Path(libDir) / "libpq.so"
                    if libFile.exists():
                        return {"include": includeDir, "lib": libDir}

        # 环境变量
        pqLibDir = os.environ.get("PQ_LIB_DIR")
        pqIncludeDir = os.environ.get("PQ_INCLUDE_DIR")
        if pqLibDir and pqIncludeDir:
            return {"include": pqIncludeDir, "lib": pqLibDir}

        return None

    # =========================================================================
    # 安装指引
    # =========================================================================

    @classmethod
    def _PerlHint(cls) -> str:
        if cls._IS_WINDOWS:
            return (
                "Windows: 安装 Strawberry Perl (不要用 Git MSYS2 自带的 Perl)\n"
                "    https://strawberryperl.com/  → 下载 64-bit 版本安装"
            )
        else:
            return "sudo apt install perl  (或 yum install perl)"

    @classmethod
    def _NasmHint(cls) -> str:
        if cls._IS_WINDOWS:
            return (
                "Windows: 下载并安装 NASM\n"
                "    https://www.nasm.us/pub/nasm/releasebuilds/\n"
                "    安装到 C:\\Program Files\\NASM\\ 或加入 PATH"
            )
        else:
            return "sudo apt install nasm  (或 yum install nasm)"

    @classmethod
    def _CMakeHint(cls) -> str:
        if cls._IS_WINDOWS:
            return (
                "Windows: 下载 CMake 安装并加入 PATH\n"
                "    https://cmake.org/download/"
            )
        else:
            return "sudo apt install cmake  (或 yum install cmake)"

    @classmethod
    def _LibpqHint(cls) -> str:
        if cls._IS_WINDOWS:
            return (
                "Windows: 安装 PostgreSQL 17+\n"
                "    https://www.postgresql.org/download/windows/\n"
                "    或设置环境变量: PQ_LIB_DIR / PQ_INCLUDE_DIR"
            )
        else:
            return (
                "Linux: sudo apt install libpq-dev\n"
                "    或设置环境变量: PQ_LIB_DIR / PQ_INCLUDE_DIR"
            )

    # =========================================================================
    # 工具函数
    # =========================================================================

    @staticmethod
    def _WindowsDrives() -> list[str]:
        """返回 Windows 所有逻辑盘符 (C, D, E, ...)"""
        drives = []
        for letter in "CDEFGHIJKLMNOPQRSTUVWXYZ":
            if Path(f"{letter}:\\").exists():
                drives.append(letter)
        return drives
