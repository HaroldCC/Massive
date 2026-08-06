#!/usr/bin/env python3
"""
GenMsgBindings.py — Protobuf → C++ 脚本绑定代码生成器

职责（v2，2026-07 重构后）:
  - 扫描 Src/Proto/*.proto，识别 *Req 消息
  - 计算依赖闭包（*Req 所在文件 + 被引用的类型所在文件）
  - 每个闭包内的 .proto → <ProtoFileName>.gen.cpp
      · MAKE_TYPE_FACTORY + ManagedStructureAnnotation（消息类型绑定）
      · string/bytes/repeated 字段访问器
      · Dispatch<Msg>Req()——解析 protobuf + 转发进脚本
  - 产出 ProtoBindIndex.gen.{h,cpp}（汇总 + EMsgID 枚举绑定）

v2 相对 v1 的变化:
  1. 不再生成 Script/AutoGen/HandlerRegistry.das。
     [msg_handler] 注解改为手写在 Script/Handlers.das 里（见 Docs/ScriptLayer_01_MsgBinding.md）。
     msgID 由手写宏在编译期从已绑定的 EMsgID 枚举按命名约定推导，不再依赖生成的 das 查表。
  2. Dispatch 函数不再耦合 WorldServer——改走 DasLangEngine::GetIns()，
     使 .gen.cpp 成为“解析 protobuf + 转发进脚本”的服务无关代码。
  3. 解析到不支持的 proto 构造（oneof / 消息内嵌 message|enum）时报错退出，
     而非静默跳过（避免漏绑定字段）。
  4. 修复 v1 的重复 #include。

用法:
  python GenMsgBindings.py --proto-dir Src/Proto --cpp-out Src/World/AutoGen
  （--das-out 已废弃：仍可传入但被忽略，仅为兼容旧 xmake 调用；建议从 xmake 移除。）
"""

import argparse
import os
import re
import sys
from collections import defaultdict, deque
from dataclasses import dataclass, field
from pathlib import Path

# ── 修复 Windows 控制台编码 ──
if sys.platform == "win32":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

PROTO_NS = "MMO::Proto"  # 项目统一 package = MMO.Proto


# ═══════════════════════════════════════════════════════════════
# 数据结构
# ═══════════════════════════════════════════════════════════════

@dataclass
class FieldInfo:
    """protobuf 字段描述"""
    name: str            # 字段名（snake_case）
    type_name: str       # 类型名（去包前缀后的 Proto 类型名或标量）
    number: int
    label: str           # "", "repeated", "optional"
    is_scalar: bool = False
    is_string: bool = False
    is_bytes: bool = False
    is_map: bool = False


@dataclass
class MessageInfo:
    """protobuf message 描述"""
    name: str            # 消息名（PascalCase）
    fields: list[FieldInfo] = field(default_factory=list)


@dataclass
class ProtoFileInfo:
    """.proto 文件解析结果"""
    name: str            # 文件名（不含 .proto，如 "Move"）
    filename: str        # 完整文件名（Move.proto）
    package: str
    imports: list[str]
    messages: list[MessageInfo] = field(default_factory=list)
    enums: dict[str, list[tuple[str, int]]] = field(default_factory=dict)  # 顶层 enum 名 → [(值名, 值)]
    has_req: bool = False   # 是否包含 *Req 消息
    service: str = ""       # 归属服务（"world"/"social"/""=公共）


# ── Regex 集合 ──
_RE_PACKAGE = re.compile(r"^\s*package\s+([\w.]+)\s*;", re.MULTILINE)
_RE_IMPORT = re.compile(r'^\s*import\s+"([^"]+)"\s*;', re.MULTILINE)
_RE_MESSAGE_START = re.compile(r"^\s*message\s+(\w+)\s*\{", re.MULTILINE)
_RE_FIELD = re.compile(
    r"^\s*(repeated|optional)\s+([\w.]+)\s+(\w+)\s*=\s*(\d+)",
    re.MULTILINE,
)
_RE_FIELD_SCALAR = re.compile(
    r"^\s*((?!repeated|optional)\w+)\s+(\w+)\s*=\s*(\d+)",
    re.MULTILINE,
)
_RE_MAP_FIELD = re.compile(
    r"^\s*map<([^,]+),\s*([^>]+)>\s+(\w+)\s*=\s*(\d+)",
    re.MULTILINE,
)

# 顶层 enum 解析（GameEvent.proto 的 EGameEventType 等——值须从 proto 解析而非消息顺序硬编码）
_RE_ENUM_START = re.compile(r"^\s*enum\s+(\w+)\s*\{", re.MULTILINE)
_RE_ENUM_VALUE = re.compile(r"^\s*([A-Z_][A-Z0-9_]*)\s*=\s*(-?\d+)", re.MULTILINE)

# 消息体内不支持的构造——命中即报错退出（避免静默漏字段）
_RE_UNSUPPORTED = re.compile(r"^\s*(oneof|message|enum)\s+\w+", re.MULTILINE)

SCALAR_TYPES = {
    "double", "float", "int32", "int64", "uint32", "uint64",
    "sint32", "sint64", "fixed32", "fixed64", "sfixed32", "sfixed64",
    "bool", "enum",
}
STRING_TYPES = {"string"}
BYTES_TYPES = {"bytes"}

_CPP_SCALAR_MAP = {
    "double": "double", "float": "float",
    "int32": "int32_t", "int64": "int64_t",
    "uint32": "uint32_t", "uint64": "uint64_t",
    "sint32": "int32_t", "sint64": "int64_t",
    "fixed32": "uint32_t", "fixed64": "uint64_t",
    "sfixed32": "int32_t", "sfixed64": "int64_t",
    "bool": "bool",
}


# ═══════════════════════════════════════════════════════════════
# 命名约定（唯一真相源——宏侧同规则实现，见 Docs/ScriptLayer_01）
# ═══════════════════════════════════════════════════════════════

def camel_to_msg_id(msg_name: str) -> str:
    """MoveReq → MSG_MOVE_REQ；LoginEnterWorldReq → MSG_LOGIN_ENTER_WORLD_REQ"""
    snake = re.sub(r"(?<![A-Z])([A-Z])", r"_\1", msg_name)
    snake = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", snake)
    return "MSG" + snake.upper()


def cap_first(s: str) -> str:
    """username → Username（仅首字母大写，其余保持——用于函数名拼接）"""
    return s[0].upper() + s[1:] if s else s


# ═══════════════════════════════════════════════════════════════
# Proto 解析
# ═══════════════════════════════════════════════════════════════

def parse_msg_id_proto(filepath: Path) -> list[tuple[str, int]]:
    """解析 MsgID.proto，提取 EMsgID 枚举 [(name, value), ...]（过滤哨兵）"""
    text = filepath.read_text(encoding="utf-8")
    pattern = re.compile(r"^\s*([A-Z_][A-Z0-9_]*)\s*=\s*(\d+)\s*;", re.MULTILINE)
    entries: list[tuple[str, int]] = []
    for m in pattern.finditer(text):
        name = m.group(1)
        value = int(m.group(2))
        if any(kw in name for kw in ("SENTINEL", "_MIN_", "_MAX_")):
            continue
        entries.append((name, value))
    return entries


def _extract_message_body(text: str, start: int) -> tuple[str, int]:
    """从 message 体起始位置扫描配对大括号，返回 (body, 结束位置)"""
    brace_depth = 1
    pos = start
    while pos < len(text) and brace_depth > 0:
        if text[pos] == "{":
            brace_depth += 1
        elif text[pos] == "}":
            brace_depth -= 1
        pos += 1
    return text[start:pos], pos


def parse_proto_file(filepath: Path, service: str = "") -> ProtoFileInfo:
    """解析一个 .proto 文件；遇到不支持的构造报错退出"""
    text = filepath.read_text(encoding="utf-8")
    basename = filepath.name
    name = filepath.stem

    pkg_match = _RE_PACKAGE.search(text)
    package = pkg_match.group(1) if pkg_match else ""
    imports = _RE_IMPORT.findall(text)

    messages: list[MessageInfo] = []
    for msg_match in _RE_MESSAGE_START.finditer(text):
        msg_name = msg_match.group(1)
        body, _ = _extract_message_body(text, msg_match.end())

        # ── 健壮性：不支持的构造直接报错，绝不静默跳过 ──
        bad = _RE_UNSUPPORTED.search(body)
        if bad is not None:
            print(
                f"错误：{basename} 的 message {msg_name} 含暂不支持的构造 "
                f"'{bad.group(1)}'。GenMsgBindings.py 目前仅支持标量/string/bytes/"
                f"message 类型字段、repeated、proto3 optional。\n"
                f"→ 请拆分该构造，或扩展生成器（含 oneof/嵌套类型）后重跑。",
                file=sys.stderr,
            )
            sys.exit(1)

        fields: list[FieldInfo] = []

        # map 字段
        for m in _RE_MAP_FIELD.finditer(body):
            fields.append(FieldInfo(
                name=m.group(3),
                type_name=f"map<{m.group(1)},{m.group(2)}>",
                number=int(m.group(4)),
                label="",
                is_map=True,
            ))

        # repeated / optional
        for m in _RE_FIELD.finditer(body):
            raw_type = m.group(2)
            type_name = raw_type.split(".")[-1]
            fields.append(FieldInfo(
                name=m.group(3),
                type_name=type_name,
                number=int(m.group(4)),
                label=m.group(1),
                is_scalar=(raw_type in SCALAR_TYPES),
                is_string=(raw_type in STRING_TYPES),
                is_bytes=(raw_type in BYTES_TYPES),
            ))

        # 普通字段
        for m in _RE_FIELD_SCALAR.finditer(body):
            raw_type = m.group(1)
            if raw_type in ("option", "reserved", "oneof", "enum", "message"):
                continue
            type_name = raw_type.split(".")[-1]
            fields.append(FieldInfo(
                name=m.group(2),
                type_name=type_name,
                number=int(m.group(3)),
                label="",
                is_scalar=(raw_type in SCALAR_TYPES),
                is_string=(raw_type in STRING_TYPES),
                is_bytes=(raw_type in BYTES_TYPES),
            ))

        messages.append(MessageInfo(name=msg_name, fields=fields))

    has_req = any(m.name.endswith("Req") for m in messages)

    # 解析顶层 enum（值从 proto 文本解析——事件枚举值不得按消息顺序硬编码）
    enums: dict[str, list[tuple[str, int]]] = {}
    for enum_match in _RE_ENUM_START.finditer(text):
        enum_name = enum_match.group(1)
        enum_body, _ = _extract_message_body(text, enum_match.end())
        values = [
            (m.group(1), int(m.group(2)))
            for m in _RE_ENUM_VALUE.finditer(enum_body)
        ]
        enums[enum_name] = values

    return ProtoFileInfo(
        name=name, filename=basename, package=package,
        imports=imports, messages=messages, enums=enums, has_req=has_req,
        service=service,
    )


# ═══════════════════════════════════════════════════════════════
# 依赖闭包 + topo 排序
# ═══════════════════════════════════════════════════════════════

def _is_message_ref(fld: FieldInfo) -> bool:
    """字段是否为消息类型引用（非标量/string/bytes/map）"""
    return not (fld.is_scalar or fld.is_string or fld.is_bytes or fld.is_map)


def _topo_sort_messages(messages: list[MessageInfo],
                        by_name: dict[str, MessageInfo]) -> list[MessageInfo]:
    """按消息间字段依赖 topo 排序：被依赖者在前。

    依赖 = 某消息的字段（singular 或 repeated）类型是同文件内另一消息。
    循环依赖无法用 ctor 内 addProperty 满足任何顺序，检测到环时告警并退回原序
    （调用方需将环上字段改为注解外后置绑定）。
    """
    visited: dict[str, int] = {}   # 0=访问中, 1=完成
    ordered: list[MessageInfo] = []
    has_cycle = [False]

    def visit(m: MessageInfo) -> None:
        state = visited.get(m.name)
        if state == 1:
            return
        if state == 0:
            has_cycle[0] = True
            return
        visited[m.name] = 0
        for fld in m.fields:
            if not _is_message_ref(fld):
                continue
            dep = by_name.get(fld.type_name)
            if dep is not None and dep is not m:
                visit(dep)
        visited[m.name] = 1
        ordered.append(m)

    for m in messages:
        visit(m)

    if has_cycle[0]:
        print("警告：检测到消息循环引用，注解注册顺序可能触发 DAS_FATAL_ERROR；"
              "如确有循环字段，请改为注解外后置 addExternProperty 绑定该字段",
              file=sys.stderr)
        return messages
    return ordered


def compute_closure(proto_files: dict[str, ProtoFileInfo]) -> list[ProtoFileInfo]:
    """含 *Req 的文件 + 其（递归）引用的类型所在文件；按文件间依赖 topo 排序（被依赖者在前）。

    返回顺序即注册顺序：被依赖文件先注册其消息注解，保证 makeHandleType 前向解析成功。
    """
    req_files: set[str] = {f for f, i in proto_files.items() if i.has_req}

    type_to_file: dict[str, str] = {}
    for fname, info in proto_files.items():
        for msg in info.messages:
            type_to_file[msg.name] = fname

    # 收集闭包（BFS 沿消息字段引用）
    closure: set[str] = set(req_files)
    queue = deque(req_files)
    while queue:
        cur = queue.popleft()
        for msg in proto_files[cur].messages:
            for fld in msg.fields:
                if not _is_message_ref(fld):
                    continue
                ref_file = type_to_file.get(fld.type_name)
                if ref_file and ref_file not in closure:
                    closure.add(ref_file)
                    queue.append(ref_file)

    # 文件间依赖边：file X 依赖 file Y（X 的某消息字段类型定义在 Y，且 Y != X）
    def file_deps(fname: str) -> set[str]:
        deps: set[str] = set()
        for msg in proto_files[fname].messages:
            for fld in msg.fields:
                if not _is_message_ref(fld):
                    continue
                ref = type_to_file.get(fld.type_name)
                if ref and ref in closure and ref != fname:
                    deps.add(ref)
        return deps

    # DFS topo：被依赖文件先入 ordered（append 在递归返回后 → 依赖者后出）
    visited: dict[str, int] = {}   # 0=访问中, 1=完成
    ordered: list[str] = []

    def visit(fname: str) -> None:
        st = visited.get(fname)
        if st == 1:
            return
        if st == 0:
            return  # 文件级环：忽略（消息级 topo 已处理大部分）
        visited[fname] = 0
        for dep in sorted(file_deps(fname)):  # sorted 保证确定性
            visit(dep)
        visited[fname] = 1
        ordered.append(fname)

    for fname in sorted(closure):
        visit(fname)
    return [proto_files[f] for f in ordered]


# ═══════════════════════════════════════════════════════════════
# 字段分类辅助
# ═══════════════════════════════════════════════════════════════

def _string_accessor_name(msg: str, fld: FieldInfo) -> str:
    return f"{msg}_Get{cap_first(fld.name)}"


def _bytes_size_name(msg: str, fld: FieldInfo) -> str:
    return f"{msg}_{cap_first(fld.name)}_Size"


def _bytes_at_name(msg: str, fld: FieldInfo) -> str:
    return f"{msg}_{cap_first(fld.name)}_At"


def _repeated_size_name(msg: str, fld: FieldInfo) -> str:
    return f"{msg}_{cap_first(fld.name)}_Size"


def _repeated_index_name(msg: str, fld: FieldInfo) -> str:
    return f"{msg}_{cap_first(fld.name)}"


def _iter_plain_string_fields(msg: MessageInfo):
    """非 repeated 的 string 字段（真文本，可安全 c_str）"""
    for fld in msg.fields:
        if fld.is_map:
            continue
        if fld.label != "repeated" and fld.is_string:
            yield fld


def _iter_bytes_fields(msg: MessageInfo):
    """非 repeated 的 bytes 字段（二进制，须按长度逐字节暴露）"""
    for fld in msg.fields:
        if fld.is_map:
            continue
        if fld.label != "repeated" and fld.is_bytes:
            yield fld


def _iter_repeated_fields(msg: MessageInfo):
    for fld in msg.fields:
        if fld.label == "repeated" and not fld.is_map:
            yield fld


# ═══════════════════════════════════════════════════════════════
# C++ 发射器（Emitter）——每个只负责一小段，主函数编排
# ═══════════════════════════════════════════════════════════════

def emit_file_header(info: ProtoFileInfo) -> list[str]:
    req_names = [m.name for m in info.messages if m.name.endswith("Req")]
    brief = "消息类型注册 + 消息分发" if info.has_req else "被业务消息引用的辅助类型注册"
    out = [
        "/**",
        f" * @file {info.name}.gen.cpp",
        f" * @brief 自动生成文件——{info.filename} {brief}",
        " *",
        " * 生成工具: Tools/Script/GenMsgBindings.py",
        f" * 来源: Src/Proto/{info.filename}",
        " * @warning 不要手动编辑",
    ]
    if req_names:
        out.append(f" * @note {', '.join(req_names)} 覆写 canClone=false——req 句柄不能存进全局/结构体。")
        out.append(" *       ⚠ 但【从 req 抽出的 string】是指向 req 内部的临时指针，")
        out.append(" *       仅本次 dispatch 调用内有效；脚本如需保留必须 := 克隆，不能 = 存储。")
        out.append(" *       bytes 字段已按长度暴露（`xx_size + xx(index)`），无 NUL 截断问题。")
    out.append(" */")
    return out


def emit_includes(info: ProtoFileInfo, proto_files: dict[str, ProtoFileInfo],
                  referenced_types: set[str]) -> list[str]:
    """去重后的 include 列表"""
    pb_headers: list[str] = []           # 保持顺序 + 去重
    seen: set[str] = set()

    def add_pb(header_name: str):
        line = f'#include "Proto/AutoGen/{header_name}.pb.h"'
        if line not in seen:
            seen.add(line)
            pb_headers.append(line)

    add_pb(info.name)
    # 被引用类型所在文件的 .pb.h
    name_to_file = {}
    for other in proto_files.values():
        for msg in other.messages:
            name_to_file[msg.name] = other.name
    for ref in sorted(referenced_types):
        f = name_to_file.get(ref)
        if f and f != info.name:
            add_pb(f)

    out = list(pb_headers)
    if info.has_req:
        out.append("#include <MsgID.pb.h>")
    # dispatch 相关：引擎（服务无关，不再 include WorldServer.h）。
    # 注册表由 DasLangEngine 持有并经 DasEngine.h 传递可见，无需单独 include。
    if info.has_req:
        out.append('#include "ScriptEngine/DasEngine.h"')
        out.append('#include "Common/Log/Log.h"')
    out.append("")
    out.append("#include <daScript/simulate/simulate.h>")
    out.append("#include <daScript/ast/ast_interop.h>")
    out.append("#include <daScript/ast/ast_handle.h>")
    out.append("#include <daScript/daScriptModule.h>")
    out.append("#include <daScript/ast/ast_typefactory.h>")
    out.append("")
    return out


def emit_type_factories(info: ProtoFileInfo, referenced_types: set[str]) -> list[str]:
    out: list[str] = []
    own_names = {m.name for m in info.messages}
    for msg in info.messages:
        out.append(f"MAKE_TYPE_FACTORY({msg.name}, {PROTO_NS}::{msg.name})")
    for ref in sorted(referenced_types):
        if ref not in own_names:
            out.append(f"MAKE_TYPE_FACTORY({ref}, {PROTO_NS}::{ref})")
    return out


def emit_string_accessor(msg: MessageInfo, fld: FieldInfo) -> list[str]:
    func = _string_accessor_name(msg.name, fld)
    return [
        "    // ── string 字段访问器（普通 addExtern 函数，脚本 req |> GetXxx()）──",
        "    //    ⚠ 返回的是指向 req 内部的 const char*，仅本次 dispatch 调用内有效；",
        "    //    脚本如需保留必须 := 克隆，不能 = 存储（见 emit_file_header 注释）。",
        f"    const char *{func}(const {PROTO_NS}::{msg.name} &msg)",
        "    {",
        f"        return msg.{fld.name}().c_str();",
        "    }",
        "",
    ]


def emit_bytes_accessors(msg: MessageInfo, fld: FieldInfo) -> list[str]:
    """bytes 字段：_size 长度属性 + (msg,index)->uint8 逐字节访问，避免 NUL 截断。"""
    size_func = _bytes_size_name(msg.name, fld)
    at_func = _bytes_at_name(msg.name, fld)
    return [
        "    // ── bytes 字段：长度 + 逐字节 uint8，避免 NUL 截断（0x00 不丢）──",
        f"    int {size_func}(const {PROTO_NS}::{msg.name} &msg)",
        "    {",
        f"        return static_cast<int>(msg.{fld.name}().size());",
        "    }",
        "",
        f"    uint8_t {at_func}(const {PROTO_NS}::{msg.name} &msg, int index)",
        "    {",
        f"        const auto &b = msg.{fld.name}();",
        "        if (index < 0 || index >= static_cast<int>(b.size())) { return 0u; }",
        f"        return static_cast<uint8_t>(b[static_cast<size_t>(index)]);",
        "    }",
        "",
    ]


def emit_repeated_accessors(msg: MessageInfo, fld: FieldInfo) -> list[str]:
    size_func = _repeated_size_name(msg.name, fld)
    index_func = _repeated_index_name(msg.name, fld)
    if fld.is_string:
        ret_type, access, default = "const char *", f"msg.{fld.name}(index).c_str()", '""'
    elif fld.is_bytes:
        ret_type, access, default = "const char *", f"reinterpret_cast<const char *>(msg.{fld.name}(index).data())", "nullptr"
    elif fld.is_scalar:
        ret_type = _CPP_SCALAR_MAP.get(fld.type_name, fld.type_name)
        access, default = f"msg.{fld.name}(index)", f"static_cast<{ret_type}>(0)"
    else:
        # message 元素：越界返回该类型的静态默认实例（const& 安全）
        ret_type = f"const {PROTO_NS}::{fld.type_name} &"
        access = f"msg.{fld.name}(index)"
        default = f"{PROTO_NS}::{fld.type_name}::default_instance()"
    return [
        "    // ── repeated 大小访问器（普通 addExtern 函数）──",
        f"    int {size_func}(const {PROTO_NS}::{msg.name} &msg)",
        "    {",
        f"        return msg.{fld.name}_size();",
        "    }",
        "",
        "    // ── repeated 索引访问：带边界检查（本 build protobuf BoundsCheckMode=kNoEnforcement）──",
        f"    {ret_type}{index_func}(const {PROTO_NS}::{msg.name} &msg, int index)",
        "    {",
        f"        if (index < 0 || index >= msg.{fld.name}_size()) {{ return {default}; }}",
        f"        return {access};",
        "    }",
        "",
    ]


def emit_annotation(msg: MessageInfo, is_req: bool) -> list[str]:
    out = [
        "    /**",
        f"     * @brief {msg.name} 的 daScript 类型注解",
    ]
    if is_req:
        out.append("     * @note 引用类型（网络消息）——句柄不可 clone，防止逃逸本次调用")
    else:
        out.append("     * @note 值类型——可 clone(:=)/存储，不可 = 复制（protobuf 非平凡拷贝）")
    out += [
        "     */",
        f"    struct {msg.name}Annotation : ManagedStructureAnnotation<{PROTO_NS}::{msg.name}, false, false>",
        "    {",
        f"        {msg.name}Annotation(ModuleLibrary &ml)",
        f'            : ManagedStructureAnnotation("{msg.name}", ml, "{PROTO_NS}::{msg.name}")',
        "        {",
    ]
    # 标量 / 嵌套 message 直接 addProperty；string/bytes/repeated 走 Register 里的 addExtern（非属性）
    for fld in msg.fields:
        if fld.is_map:
            out.append(f"            // TODO(GenMsgBindings): {msg.name}.{fld.name} (map) 需手写绑定，暂未生成")
            continue
        if fld.is_string or fld.is_bytes:
            continue
        if fld.label == "repeated":
            continue
        out.append(f'            addProperty<DAS_BIND_MANAGED_PROP({fld.name})>("{fld.name}");')
    out.append("        }")
    if is_req:
        out += [
            "",
            "        // §2.4 生命周期防护——阻断脚本 clone req 存进逃逸本次调用的存储位置",
            "        // 模板参数 <Msg, false, false>：canNew=false（脚本不能 new 请求句柄），",
            "        // canDelete=false（脚本不能 delete）。canCopy/canMove 由基类按 protobuf 非平凡拷贝推导为 false。",
            "        virtual bool canClone() const override { return false; }",
        ]
    out += ["    };", ""]
    return out


def emit_dispatch_fn(info: ProtoFileInfo, msg: MessageInfo) -> list[str]:
    """解析 protobuf + 转发进脚本。服务无关：走 DasLangEngine::GetIns()。"""
    func = f"Dispatch{msg.name}"
    msg_id = f"{PROTO_NS}::{camel_to_msg_id(msg.name)}"
    return [
        f"    bool {func}(uint32 sessionID, const uint8 *body, size_t len)",
        "    {",
        f"        {PROTO_NS}::{msg.name} req;",
        "        if (!req.ParseFromArray(body, static_cast<int>(len)))",
        "        {",
        f'            MMO::Log::Error("{info.name}.gen: {msg.name} parse failed, session={{}}", sessionID);',
        "            return false;",
        "        }",
        "",
        "        das::Context     *ctx        = MMO::DasLangEngine::GetIns().GetScriptContext();",
        "        das::SimFunction *fnDispatch = MMO::DasLangEngine::GetIns().GetDispatchFunc();",
        "        if (nullptr == ctx || nullptr == fnDispatch)",
        "        {",
        "            return false;",
        "        }",
        "",
        "        // 脚本 DispatchMsg 返回 bool（handler 是否命中）——未命中返回 false，",
        "        // 让 OnMessage 的脚本优先路径落到 C++ handler（消除双 handler 遮蔽）。",
        "        // CallScriptFunctionInR 校验 arity（DispatchMsg 脚本 expects 3 参）——签名漂移立即报错。",
        f"        const uint32_t msgID = static_cast<uint32_t>({msg_id});",
        "        auto [handled, ok] = MMO::DasLangEngine::CallScriptFunctionInR<bool>(ctx, fnDispatch, \"Dispatch\", msgID, sessionID, &req);",
        "        if (!ok)",
        "        {",
        f'            MMO::Log::Error("{info.name}.gen: {msg.name} dispatch exception, session={{}}", sessionID);',
        "            return false;",
        "        }",
        "        return handled;",
        "    }",
        "",
    ]


def emit_register_bindings(info: ProtoFileInfo) -> list[str]:
    out = [
        f"    void Register{info.name}ProtoBindings(das::Module &mod, das::ModuleLibrary &lib)",
        "    {",
        "        (void)mod;",
    ]
    has_any = False
    for msg in info.messages:
        out.append(f"        mod.addAnnotation(new {msg.name}Annotation(lib));")
        has_any = True
        for fld in _iter_plain_string_fields(msg):
            func = _string_accessor_name(msg.name, fld)
            # ⚠ string 字段不能注册为 `.`xx 属性（addExternProperty）：AOT 发射器对
            # propertyFunction 生成 ((obj).cppName()) 成员调用形式，自由函数无法成员调用，
            # 脚本读 string 字段时 AOT 编译失败（error 见 aot_cpp.das propertyFunction 分支）。
            # 改为普通 addExtern 函数，脚本用 req |> GetUsername() 或 GetUsername(req) 访问。
            out.append(f'        das::addExtern<DAS_BIND_FUN({func})>(mod, lib, "{func}", das::SideEffects::none)')
            out.append('            ->args({"msg"});')
        for fld in _iter_bytes_fields(msg):
            size_func = _bytes_size_name(msg.name, fld)
            at_func = _bytes_at_name(msg.name, fld)
            # bytes 字段同 string——size 也注册为普通函数（非 `.`xx_size 属性），规避 AOT 成员展开。
            out.append(f'        das::addExtern<DAS_BIND_FUN({size_func})>(mod, lib, "{size_func}", das::SideEffects::none)')
            out.append('            ->args({"msg"});')
            out.append(f'        das::addExtern<DAS_BIND_FUN({at_func})>(mod, lib, "{fld.name}", das::SideEffects::none)')
            out.append('            ->args({"msg", "index"});')
        for fld in _iter_repeated_fields(msg):
            size_func = _repeated_size_name(msg.name, fld)
            index_func = _repeated_index_name(msg.name, fld)
            # repeated 字段同 string/bytes——size/索引都注册为普通函数，规避 AOT 成员展开。
            out.append(f'        das::addExtern<DAS_BIND_FUN({size_func})>(mod, lib, "{size_func}", das::SideEffects::none)')
            out.append('            ->args({"msg"});')
            out.append(f'        das::addExtern<DAS_BIND_FUN({index_func})>(mod, lib, "{fld.name}", das::SideEffects::none)')
            out.append('            ->args({"msg", "index"});')
    if not has_any:
        out.append("        // 本文件仅含被引用类型，无消息")
    out.append("    }")
    out.append("")
    return out


def emit_register_dispatch(info: ProtoFileInfo, req_msgs: list[MessageInfo]) -> list[str]:
    # 注册表由 DasLangEngine 持有（脚本层组件，World/Social 共用）。
    # 经引擎单例取实例注册——不再假设 ScriptDispatchRegistry 是静态类。
    out = [
        f"    void Register{info.name}MsgDispatch()",
        "    {",
        "        auto &registry = DasLangEngine::GetIns().DispatchRegistry();",
    ]
    for msg in req_msgs:
        msg_id = f"{PROTO_NS}::{camel_to_msg_id(msg.name)}"
        out.append(f"        registry.Register(static_cast<uint32_t>({msg_id}), &Dispatch{msg.name});")
    out.append("    }")
    return out


# ═══════════════════════════════════════════════════════════════
# 组装：单个 .gen.cpp
# ═══════════════════════════════════════════════════════════════

def generate_proto_gen_cpp(info: ProtoFileInfo,
                           proto_files: dict[str, ProtoFileInfo]) -> str:
    # ★ A1: 文件内按字段依赖 topo 排序，保证被依赖的消息注解先注册
    #   （注解 ctor 内 addProperty 即时解析 message 字段类型，前向引用会 DAS_FATAL_ERROR）
    by_name = {m.name: m for m in info.messages}
    info.messages = _topo_sort_messages(info.messages, by_name)

    # 收集跨文件引用类型（非 repeated 的 message 类型字段）
    referenced_types: set[str] = set()
    for msg in info.messages:
        for fld in msg.fields:
            if (not (fld.is_scalar or fld.is_string or fld.is_bytes or fld.is_map)
                    and fld.label != "repeated"):
                referenced_types.add(fld.type_name)

    req_names = {m.name for m in info.messages if m.name.endswith("Req")}
    req_msgs = [m for m in info.messages if m.name in req_names]

    lines: list[str] = []
    lines += emit_file_header(info)
    lines += emit_includes(info, proto_files, referenced_types)
    lines += emit_type_factories(info, referenced_types)

    # ── 匿名命名空间：访问器 + 注解 + dispatch ──
    lines.append("namespace")
    lines.append("{")
    lines.append("    using namespace das;")
    lines.append("")

    for msg in info.messages:
        for fld in _iter_plain_string_fields(msg):
            lines += emit_string_accessor(msg, fld)
        for fld in _iter_bytes_fields(msg):
            lines += emit_bytes_accessors(msg, fld)
        for fld in _iter_repeated_fields(msg):
            lines += emit_repeated_accessors(msg, fld)

    for msg in info.messages:
        lines += emit_annotation(msg, msg.name in req_names)

    for msg in req_msgs:
        lines += emit_dispatch_fn(info, msg)

    lines.append("} // namespace")
    lines.append("")

    # ── namespace MMO：注册函数 ──
    lines.append("namespace MMO")
    lines.append("{")
    lines.append("")
    lines += emit_register_bindings(info)
    if req_msgs:
        lines += emit_register_dispatch(info, req_msgs)
    lines.append("} // namespace MMO")
    lines.append("")
    return "\n".join(lines)


# ═══════════════════════════════════════════════════════════════
# ProtoBindIndex.gen.h / .cpp
# ═══════════════════════════════════════════════════════════════

def generate_emsgid_bind_h() -> str:
    """EMsgIDBind.gen.h —— EMsgID 枚举绑定声明（公共层，DasCommonModule 调用）"""
    return "\n".join([
        "/**",
        " * @file EMsgIDBind.gen.h",
        " * @brief 自动生成文件——EMsgID 枚举绑定声明（全局共享 ID 值空间）",
        " *",
        " * 生成工具: Tools/Script/GenMsgBindings.py",
        " * @warning 不要手动编辑",
        " */",
        "#pragma once",
        "",
        "namespace das",
        "{",
        "    class Module;",
        "} // namespace das",
        "",
        "namespace MMO",
        "{",
        "",
        "    /**",
        "     * @brief 注册 EMsgID 枚举到指定模块",
        "     * @note 由 DasCommonModule::Build 调用（绑定到 Common 模块），",
        "     *       使所有服务的脚本侧都能引用 EMsgID.MSG_XXX，宏侧查 Common 即可。",
        "     */",
        "    void RegisterEMsgIDEnumeration(das::Module &mod);",
        "",
        "} // namespace MMO",
        "",
    ])


def generate_emsgid_bind_cpp(msg_id_entries: list[tuple[str, int]]) -> str:
    """EMsgIDBind.gen.cpp —— EMsgID 枚举绑定实现"""
    # 官方宏 DAS_BASE_BIND_ENUM(enum_name, das_enum_name, ...) 生成：
    #   EnumerationEMsgID 类（当前命名空间，文件作用域）+ typeFactory<MMO::Proto::EMsgID>
    # 消除了手工 Enumeration 无 typeFactory 的缺陷（addExtern 参数/返回值用 EMsgID 类型时能解析）。
    enum_members = ", ".join(name for name, _ in msg_id_entries)
    lines = [
        "/**",
        " * @file EMsgIDBind.gen.cpp",
        " * @brief 自动生成文件——EMsgID 枚举绑定（全局共享 ID 值空间）",
        " *",
        " * 生成工具: Tools/Script/GenMsgBindings.py",
        " * @warning 不要手动编辑——新增/删除消息后重新生成",
        " */",
        '#include "Proto/AutoGen/EMsgIDBind.gen.h"',
        "",
        "#include <daScript/daScriptModule.h>",
        "#include <daScript/ast/ast.h>",
        "#include <daScript/simulate/simulate.h>",
        "#include <daScript/simulate/bind_enum.h>",
        "#include <MsgID.pb.h>",
        "",
        "// 官方范式（cpp_api.rst 枚举节）：DAS_BASE_BIND_ENUM 须放在 using namespace das 之前。",
        "// 生成 EnumerationEMsgID 类 + typeFactory<MMO::Proto::EMsgID>（addExtern 类型解析用）。",
        f"DAS_BASE_BIND_ENUM(MMO::Proto::EMsgID, EMsgID, {enum_members})",
        "",
        "DAS_BIND_ENUM_CAST(MMO::Proto::EMsgID)",
        "",
        "namespace MMO",
        "{",
        "",
        "    void RegisterEMsgIDEnumeration(das::Module &mod)",
        "    {",
        "        // EMsgID 是全局共享 ID 值空间，绑定到 Common 模块（DasCommonModule 构造注册），",
        "        // 使所有服务（World/Social）都能在脚本侧引用 EMsgID.MSG_XXX，宏侧查 Common 即可。",
        "        mod.addEnumeration(new EnumerationEMsgID());",
        "    }",
        "",
        "} // namespace MMO",
        "",
    ]
    return "\n".join(lines)


def generate_index_h(closure: list[ProtoFileInfo]) -> str:
    lines = [
        "/**",
        " * @file ProtoBindIndex.gen.h",
        " * @brief 自动生成文件——汇总声明，供宿主（WorldServer 等）手写代码 include",
        " *",
        " * 生成工具: Tools/Script/GenMsgBindings.py",
        " * @warning 不要手动编辑",
        " */",
        "#pragma once",
        "",
        "#include <MsgID.pb.h>",
    ]
    # include 闭包内所有消息的 .pb.h——AOT 生成的 .das.cpp 会引用 MMO::Proto::Xxx 类型，
    # 经 aotRequire 只 include 本头，故本头必须提供全部消息类型定义。
    seen_pb: set[str] = set()
    for info in closure:
        if info.name not in seen_pb:
            seen_pb.add(info.name)
            lines.append(f'#include "Proto/AutoGen/{info.name}.pb.h"')
    lines += [
        "",
        "namespace das",
        "{",
        "    class Module;",
        "    class ModuleLibrary;",
        "} // namespace das",
        "",
        "namespace MMO",
        "{",
        "",
        "    /**",
        "     * @brief 注册当前依赖闭包内全部消息的 daScript 类型（含 EMsgID 枚举）",
        "     * @note 在服务专用 Module 的 BindFunctions() 内调用一次",
        "     */",
        "    void RegisterAllProtoMessageTypes(das::Module &mod, das::ModuleLibrary &lib);",
        "",
        "    /**",
        "     * @brief 注册当前依赖闭包内全部 *Req 的消息分发函数到 ScriptDispatchRegistry",
        "     * @note 在脚本 Load 成功、dispatch_msg 就绪之后调用一次",
        "     */",
        "    void RegisterAllMsgDispatch();",
        "",
        "} // namespace MMO",
        "",
    ]
    return "\n".join(lines)


def generate_index_cpp(closure: list[ProtoFileInfo],
                       msg_id_entries: list[tuple[str, int]],
                       cpp_out_rel: str = "World/AutoGen") -> str:
    # 注意：EMsgID 枚举绑定已拆到 EMsgIDBind.gen.{h,cpp}（公共层，DasCommonModule 调用），
    # 这里只保留服务侧的消息类型注册 + 分发注册。
    lines = [
        "/**",
        " * @file ProtoBindIndex.gen.cpp",
        " * @brief 自动生成文件——汇总各 <ProtoFileName>.gen.cpp 的注册函数 + 消息分发注册",
        " *",
        " * 生成工具: Tools/Script/GenMsgBindings.py",
        " * @warning 不要手动编辑——新增/删除 .proto 后重新生成",
        " * @note 调用顺序已按依赖关系排好（被引用类型所在文件先注册）。",
        " * @note EMsgID 枚举绑定见 EMsgIDBind.gen.cpp（绑定到 Common 模块）。",
        " */",
        f'#include "{cpp_out_rel}/ProtoBindIndex.gen.h"',
        "",
        "#include <daScript/daScriptModule.h>",
        "#include <daScript/ast/ast.h>",
        "#include <daScript/simulate/simulate.h>",
        "#include <MsgID.pb.h>",
        "",
        "namespace MMO",
        "{",
    ]
    for info in closure:
        lines.append(f"    extern void Register{info.name}ProtoBindings(das::Module &mod, das::ModuleLibrary &lib);")
    lines.append("")
    for info in closure:
        if info.has_req:
            lines.append(f"    extern void Register{info.name}MsgDispatch();")
    lines.append("} // namespace MMO")
    lines.append("")
    lines += [
        "namespace MMO",
        "{",
        "",
        "    void RegisterAllProtoMessageTypes(das::Module &mod, das::ModuleLibrary &lib)",
        "    {",
    ]
    for info in closure:
        lines.append(f"        Register{info.name}ProtoBindings(mod, lib);")
    lines += [
        "    }",
        "",
        "    void RegisterAllMsgDispatch()",
        "    {",
    ]
    for info in closure:
        if info.has_req:
            lines.append(f"        Register{info.name}MsgDispatch();")
    lines += [
        "    }",
        "",
        "} // namespace MMO",
        "",
    ]
    return "\n".join(lines)


# ═══════════════════════════════════════════════════════════════
# GameEvent 绑定生成器（ECS_06 决策 3：类型化事件 + 自动绑定）
# ═══════════════════════════════════════════════════════════════

def event_msg_to_enum(event_msg_name: str) -> str:
    """EntityDamagedEvent → GAME_EVENT_ENTITY_DAMAGED"""
    # 去 Event 后缀 → EntityDamaged → 驼峰转下划线大写 → 加前缀
    if event_msg_name.endswith("Event"):
        event_msg_name = event_msg_name[: -len("Event")]
    snake = re.sub(r"(?<![A-Z])([A-Z])", r"_\1", event_msg_name)
    snake = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", snake)
    return "GAME_EVENT" + snake.upper()


# ═══════════════════════════════════════════════════════════════
# TypeNameRegistry 查表生成器（ScriptLayer_06 §3.1：命名约定单一真相源）
# ──
# 产出 MsgTypeToID(name)→EMsgID 值 / EventTypeToID(name)→EGameEventType 值。
# 分层（宏 apply 的 macroContext 可见性约束）：
#   · MsgTypeToID   → 公共层（Src/Proto/AutoGen，绑 Common）——[msg_handler] 宏
#                     （MsgHandlerRegistry.das）只 require Common，宏 apply 能看到。
#   · EventTypeToID → 服务层（cpp_out，绑 world）——[game_event] 宏
#                     （GameEventRegistry.das）require world，宏 apply 能看到。
# 宏 apply() 直接查表，das 侧零命名规则。未注册返回 -1（哨兵，ID 必须为正 int32）。
# ═══════════════════════════════════════════════════════════════

def generate_msg_type_registry_h() -> str:
    lines = [
        "/**",
        " * @file MsgTypeRegistry.gen.h",
        " * @brief 自动生成文件——消息宏命名查表（命名约定单一真相源，ScriptLayer_06 §3.1）",
        " *",
        " * 生成工具: Tools/Script/GenMsgBindings.py",
        " * @warning 不要手动编辑——新增 proto 消息后重新生成",
        " */",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace MMO::Script",
        "{",
        "    /**",
        "     * @brief 消息类型名 → EMsgID 值",
        "     * @param typeName 脚本侧 handle 类型名（如 \"MoveReq\"）",
        "     * @return EMsgID 值（正 int32）；未注册返回 -1（哨兵）",
        "     */",
        "    int64_t MsgTypeToID(const char *typeName);",
        "} // namespace MMO::Script",
        "",
    ]
    return "\n".join(lines)


def generate_msg_type_registry_cpp(req_msg_names: list[str],
                                   msg_id_entries: list[tuple[str, int]]) -> str:
    """生成 MsgTypeToID 实现（绑 Common，公共层）。

    req_msg_names  —— 全部 *Req 消息名（MoveReq 等）
    msg_id_entries —— EMsgID 枚举 [(名, 值), ...]
    """
    # 消息名 → 枚举值：按命名约定 camel_to_msg_id，查 EMsgID 枚举值
    msg_id_by_name: dict[str, int] = {}
    for name in req_msg_names:
        enum_name = camel_to_msg_id(name)  # MoveReq → MSG_MOVE_REQ
        for en, ev in msg_id_entries:
            if en == enum_name:
                msg_id_by_name[name] = ev
                break

    lines = [
        "/**",
        " * @file MsgTypeRegistry.gen.cpp",
        " * @brief 自动生成文件——消息宏命名查表（命名约定单一真相源，ScriptLayer_06 §3.1）",
        " *",
        " * 生成工具: Tools/Script/GenMsgBindings.py",
        " * @warning 不要手动编辑——新增 proto 消息后重新生成",
        " */",
        '#include "MsgTypeRegistry.gen.h"',
        "",
        "#include <MsgID.pb.h>",
        "",
        "#include <string>",
        "#include <unordered_map>",
        "",
        "namespace MMO::Script",
        "{",
        "    int64_t MsgTypeToID(const char *typeName)",
        "    {",
        "        static const std::unordered_map<std::string, int64_t> table = {",
    ]
    for name, value in sorted(msg_id_by_name.items()):
        lines.append(f'            {{ "{name}", static_cast<int64_t>({PROTO_NS}::{camel_to_msg_id(name)}) }},')
    lines += [
        "        };",
        "        auto it = table.find(nullptr != typeName ? typeName : \"\");",
        "        return it == table.end() ? -1 : it->second;",
        "    }",
        "} // namespace MMO::Script",
        "",
    ]
    return "\n".join(lines)


def generate_event_type_registry_h() -> str:
    lines = [
        "/**",
        " * @file EventTypeRegistry.gen.h",
        " * @brief 自动生成文件——事件宏命名查表（命名约定单一真相源，ScriptLayer_06 §3.1）",
        " *",
        " * 生成工具: Tools/Script/GenMsgBindings.py",
        " * @warning 不要手动编辑——新增 proto 事件后重新生成",
        " */",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace MMO::Script",
        "{",
        "    /**",
        "     * @brief 事件类型名 → EGameEventType 值",
        "     * @param typeName 脚本侧 handle 类型名（如 \"EntityDamagedEvent\"）",
        "     * @return EGameEventType 值（正 int32）；未注册返回 -1（哨兵）",
        "     */",
        "    int64_t EventTypeToID(const char *typeName);",
        "} // namespace MMO::Script",
        "",
    ]
    return "\n".join(lines)


def generate_event_type_registry_cpp(event_msg_names: list[str],
                                     event_enum_values: list[tuple[str, int]]) -> str:
    """生成 EventTypeToID 实现（绑 world，服务层）。

    event_msg_names  —— 全部 *Event 消息名（EntityDamagedEvent 等）
    event_enum_values—— EGameEventType 枚举 [(名, 值), ...]
    """
    # 事件名 → 枚举值：event_msg_to_enum，查 EGameEventType 枚举值
    ev_id_by_name: dict[str, int] = {}
    for name in event_msg_names:
        enum_name = event_msg_to_enum(name)  # EntityDamagedEvent → GAME_EVENT_ENTITY_DAMAGED
        for en, ev in event_enum_values:
            if en == enum_name:
                ev_id_by_name[name] = ev
                break

    lines = [
        "/**",
        " * @file EventTypeRegistry.gen.cpp",
        " * @brief 自动生成文件——事件宏命名查表（命名约定单一真相源，ScriptLayer_06 §3.1）",
        " *",
        " * 生成工具: Tools/Script/GenMsgBindings.py",
        " * @warning 不要手动编辑——新增 proto 事件后重新生成",
        " */",
        '#include "World/AutoGen/EventTypeRegistry.gen.h"',
        "",
        "#include <GameEvent.pb.h>",
        "",
        "#include <string>",
        "#include <unordered_map>",
        "",
        "namespace MMO::Script",
        "{",
        "    int64_t EventTypeToID(const char *typeName)",
        "    {",
        "        static const std::unordered_map<std::string, int64_t> table = {",
    ]
    for name, value in sorted(ev_id_by_name.items()):
        lines.append(f'            {{ "{name}", static_cast<int64_t>({PROTO_NS}::{event_msg_to_enum(name)}) }},')
    lines += [
        "        };",
        "        auto it = table.find(nullptr != typeName ? typeName : \"\");",
        "        return it == table.end() ? -1 : it->second;",
        "    }",
        "} // namespace MMO::Script",
        "",
    ]
    return "\n".join(lines)


def generate_game_event_bindings_h(event_msgs: list[MessageInfo]) -> str:
    lines = [
        "/**",
        " * @file GameEventBindings.gen.h",
        " * @brief 自动生成文件——游戏事件绑定声明（类型化事件，ECS_06 决策 3）",
        " *",
        " * 生成工具: Tools/Script/GenMsgBindings.py",
        " * @warning 不要手动编辑——新增/删除 GameEvent.proto 事件后重新生成",
        " */",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "// 事件消息类型（EntityDamagedEvent 等）——AOT 生成的 .das.cpp 引用这些类型",
        '#include "Proto/AutoGen/GameEvent.pb.h"',
        "",
        "namespace das",
        "{",
        "    class Module;",
        "    class ModuleLibrary;",
        "} // namespace das",
        "",
        "namespace MMO",
        "{",
        "",
        "    /**",
        "     * @brief 注册全部游戏事件类型 + EGameEventType 枚举到模块",
        "     * @note 由 WorldScriptModule::Build 调用（绑定到 world 模块），",
        "     *       脚本侧 require world 后可用 EGameEventType 枚举 + 各 *Event 类型",
        "     */",
        "    void RegisterAllGameEventBindings(das::Module &mod, das::ModuleLibrary &lib);",
        "",
        "} // namespace MMO",
        "",
    ]
    return "\n".join(lines)


def generate_game_event_bindings_cpp(event_msgs: list[MessageInfo],
                                     event_enum_values: list[tuple[str, int]] = None) -> str:
    lines = [
        "/**",
        " * @file GameEventBindings.gen.cpp",
        " * @brief 自动生成文件——游戏事件类型绑定 + Emit 工厂（类型化事件）",
        " *",
        " * 生成工具: Tools/Script/GenMsgBindings.py",
        " * @warning 不要手动编辑",
        " */",
        '#include "GameEventBindings.gen.h"',
        "",
        "#include <GameEvent.pb.h>",
        "",
        '#include "ScriptEngine/GameEventBus.h"',
        "",
        "#include <daScript/simulate/simulate.h>",
        "#include <daScript/ast/ast_interop.h>",
        "#include <daScript/ast/ast_handle.h>",
        "#include <daScript/daScriptModule.h>",
        "#include <daScript/ast/ast_typefactory.h>",
        "#include <daScript/simulate/bind_enum.h>",
        "",
    ]

    # MAKE_TYPE_FACTORY——每个事件类型必须（ManagedStructureAnnotation 需要 typeName 特化）
    for ev in event_msgs:
        lines.append(f"MAKE_TYPE_FACTORY({ev.name}, {PROTO_NS}::{ev.name})")
    # GameEventEnvelope 也需 typeName 特化（文件作用域，不能在 namespace 内）
    lines.append("MAKE_TYPE_FACTORY(GameEventEnvelope, MMO::GameEventEnvelope)")
    lines.append("")

    lines.append("namespace")
    lines.append("{")
    lines.append("    using namespace das;")
    lines.append("")

    # 每个事件的 ManagedStructureAnnotation（字段访问器）
    for ev in event_msgs:
        lines += [
            f"    // {ev.name} 类型注解（自动生成）",
            f"    struct {ev.name}Annotation : ManagedStructureAnnotation<{PROTO_NS}::{ev.name}, false, false>",
            "    {",
            f'        {ev.name}Annotation(ModuleLibrary &ml)',
            f'            : ManagedStructureAnnotation("{ev.name}", ml, "{PROTO_NS}::{ev.name}")',
            "        {",
        ]
        for fld in ev.fields:
            if fld.is_map or fld.is_string or fld.is_bytes or fld.label == "repeated":
                continue  # 事件字段均为标量（GameEvent.proto 约束）
            lines.append(f'            addProperty<DAS_BIND_MANAGED_PROP({fld.name})>("{fld.name}");')
        lines += [
            "        }",
            "        // 事件类型是 protobuf 消息——canNew/canDelete 均 false（模板参），",
            "        // canCopy/canMove 由基类按非平凡拷贝推导为 false；阻断脚本 clone 逃逸。",
            "        virtual bool canClone() const override { return false; }",
            "    };",
            "",
        ]

    # GameEventEnvelope 类型注解（脚本侧声明 array<GameEventEnvelope> 用）
    # 注意：字段名 event_type（type 是 das 保留字）
    # ⚠ event_type/data 不注册为 `.`xx 属性：AOT 发射器对 propertyFunction 生成
    #   ((obj).cppName()) 成员调用，自由函数无法成员调用 → AOT 编译失败。
    #   改为普通 addExtern 函数（脚本侧实际不访问这些字段，经 DispatchGameEvent 函数接收事件）。
    lines += [
        "    // GameEventEnvelope 类型注解——事件总线负载（脚本侧遍历用）",
        "    struct GameEventEnvelopeAnnotation : ManagedStructureAnnotation<MMO::GameEventEnvelope>",
        "    {",
        '        GameEventEnvelopeAnnotation(ModuleLibrary &ml)',
        '            : ManagedStructureAnnotation("GameEventEnvelope", ml, "MMO::GameEventEnvelope")',
        "        {",
        "            // 事件经 DispatchGameEvent(evType, payload) 接收，此处无需绑定字段属性",
        "        }",
        "    };",
        "",
        "    // event_type 值访问器（普通 addExtern 函数，非属性——规避 AOT 成员展开）",
        "    uint16_t GameEventEnvelope_GetEventType(const MMO::GameEventEnvelope &env)",
        "    {",
        "        return env.event_type;",
        "    }",
        "",
        "    // data 指针访问器（普通 addExtern 函数，非属性）。",
        "    // 返回 const uint8_t*——脚本侧只读 payload（铁律：脚本不得写总线内部缓冲）。",
        "    const uint8_t *GameEventEnvelope_GetData(const MMO::GameEventEnvelope &env)",
        "    {",
        "        return env.data;",
        "    }",
        "",
    ]

    # EGameEventType 枚举绑定
    lines += [
        "    // EGameEventType 枚举绑定——脚本侧写 EGameEventType.GAME_EVENT_XXX",
        "    struct EnumerationEGameEventType : das::Enumeration",
        "    {",
        '        EnumerationEGameEventType() : das::Enumeration("EGameEventType")',
        "        {",
        "            external = true;",
        '            cppName  = "MMO::Proto::EGameEventType";',
        "            baseType = das::Type::tInt;",
    ]
    if event_enum_values:
        # 从 GameEvent.proto 的 enum EGameEventType 解析真实值（含 GAME_EVENT_NONE=0）——
        # 不再按消息声明顺序硬编码 1..N（中间插入事件会错位导致分派错乱）。
        for name, value in event_enum_values:
            lines.append(f'            addI("{name}", {value}, das::LineInfo());')
    else:
        # 兜底：proto enum 未解析到时退化为消息顺序（不应发生）
        for i, ev in enumerate(event_msgs, start=1):
            enum_name = event_msg_to_enum(ev.name)
            lines.append(f'            addI("{enum_name}", {i}, das::LineInfo());')
    lines += [
        "        }",
        "    };",
        "",
        "} // namespace",
        "",
        "namespace MMO",
        "{",
        "",
        "    void RegisterAllGameEventBindings(das::Module &mod, das::ModuleLibrary &lib)",
        "    {",
    ]
    for ev in event_msgs:
        lines.append(f"        mod.addAnnotation(new {ev.name}Annotation(lib));")
    lines.append("        mod.addAnnotation(new GameEventEnvelopeAnnotation(lib));")
    lines.append("        mod.addEnumeration(new EnumerationEGameEventType());")
    # GameEventEnvelope 访问器注册为普通 addExtern 函数（非 `.`xx 属性——规避 AOT 成员展开）。
    # 脚本侧实际不直接访问这些字段，事件经 DispatchGameEvent(evType, payload) 接收。
    lines.append(
        '        das::addExtern<DAS_BIND_FUN(GameEventEnvelope_GetEventType)>'
        '(mod, lib, "GameEventEnvelope_GetEventType", das::SideEffects::none)'
    )
    lines.append('            ->args({"env"});')
    lines.append(
        '        das::addExtern<DAS_BIND_FUN(GameEventEnvelope_GetData)>'
        '(mod, lib, "GameEventEnvelope_GetData", das::SideEffects::none)'
    )
    lines.append('            ->args({"env"});')
    lines += [
        "    }",
        "",
    ]

    lines += [
        "} // namespace MMO",
        "",
        f"DAS_BIND_ENUM_CAST({PROTO_NS}::EGameEventType)",
        "",
    ]
    return "\n".join(lines)


def collect_game_events(proto_files: dict[str, ProtoFileInfo]) -> list[MessageInfo]:
    """从 GameEvent.proto 收集 *Event 消息（按枚举顺序）"""
    info = proto_files.get("GameEvent.proto")
    if not info:
        return []
    # 按消息声明顺序（枚举值即顺序，见 generate_game_event_bindings_cpp）
    return [m for m in info.messages if m.name.endswith("Event")]


# ═══════════════════════════════════════════════════════════════
# 主入口
# ═══════════════════════════════════════════════════════════════

def collect_service_protos(proto_dir: Path, service: str) -> dict[str, ProtoFileInfo]:
    """扫描 公共（顶层）+ <Service>/ 子目录 的 .proto，返回 {filename: ProtoFileInfo}。

    - 顶层 *.proto 视为公共（Common.proto 等），所有服务共享；
    - <Service>/ 子目录（如 World/、Social/）为该服务专用；
    - 若同名文件同时出现在公共与服务子目录，服务子目录优先（覆盖）。
    """
    proto_files: dict[str, ProtoFileInfo] = {}
    exclude = {"MsgID.proto"}
    # 公共
    for p in sorted(proto_dir.glob("*.proto")):
        if p.name in exclude:
            continue
        info = parse_proto_file(p)
        proto_files[info.filename] = info
    # 服务专用
    svc_dir = proto_dir / service.capitalize()
    if svc_dir.is_dir():
        for p in sorted(svc_dir.glob("*.proto")):
            if p.name in exclude:
                continue
            info = parse_proto_file(p, service=service)
            proto_files[info.filename] = info
    return proto_files


def main():
    parser = argparse.ArgumentParser(description="Protobuf → C++ 脚本绑定代码生成器")
    parser.add_argument("--service", required=True,
                        help="服务名（world / social），决定扫描的 proto 子目录")
    parser.add_argument("--proto-dir", required=True, help="Src/Proto 目录路径")
    parser.add_argument("--cpp-out", required=True, help="C++ 生成文件输出路径（如 Src/World/AutoGen）")
    parser.add_argument("--emsgid-out", required=False, default=None,
                        help="EMsgID 枚举绑定输出路径（默认 Src/Proto/AutoGen——公共层，"
                             "由 DasCommonModule 依赖）")
    parser.add_argument("--only-emsgid", action="store_true",
                        help="仅生成 EMsgID 枚举绑定（公共层），不生成服务消息绑定。"
                             "供 Proto target 的 proto_msgid rule 调用")
    parser.add_argument("--das-out", required=False, default=None,
                        help="[已废弃] 旧 das 输出路径——保留以兼容旧 xmake 调用，本生成器忽略之")
    parser.add_argument("--purge-legacy-das", action="store_true",
                        help="迁移到手写 [msg_handler] 后，删除旧生成产物 "
                             "Script/AutoGen/HandlerRegistry.das 与 MsgIDConstants.das。"
                             "默认不删——避免尚未迁移的构建丢失依赖文件。")
    args = parser.parse_args()

    proto_dir = Path(args.proto_dir)
    cpp_out = Path(args.cpp_out)
    cpp_out.mkdir(parents=True, exist_ok=True)

    if args.das_out:
        print(f"⚠️  --das-out 已废弃并被忽略（{args.das_out}）——请从 xmake 调用中移除该参数")

    # 0. EMsgID 枚举绑定（公共层，DasCommonModule 依赖）——先于消息绑定生成
    msg_id_path0 = proto_dir / "MsgID.proto"
    if msg_id_path0.is_file():
        msg_id_entries0 = parse_msg_id_proto(msg_id_path0)
        emsgid_out = Path(args.emsgid_out) if args.emsgid_out else proto_dir / "AutoGen"
        emsgid_out.mkdir(parents=True, exist_ok=True)
        (emsgid_out / "EMsgIDBind.gen.h").write_text(generate_emsgid_bind_h(), encoding="utf-8")
        print(f"  生成: {emsgid_out / 'EMsgIDBind.gen.h'}")
        (emsgid_out / "EMsgIDBind.gen.cpp").write_text(
            generate_emsgid_bind_cpp(msg_id_entries0), encoding="utf-8")
        print(f"  生成: {emsgid_out / 'EMsgIDBind.gen.cpp'}")

        # 消息宏命名查表（ScriptLayer_06 §3.1）——MsgTypeToID 绑 Common，需在 ScriptEngine 前就绪。
        # 与 EMsgIDBind 同源：扫描公共层 proto 的 *Req 消息名 → EMsgID 值。
        # 只扫公共层（MsgID.proto 排外）；服务专属消息由服务层完整流程追加（见下）。
        req_names0: list[str] = []
        for pf in sorted(proto_dir.glob("*.proto")):
            if pf.name == "MsgID.proto":
                continue
            info = parse_proto_file(pf)
            req_names0 += [m.name for m in info.messages if m.name.endswith("Req")]
        if req_names0:
            (emsgid_out / "MsgTypeRegistry.gen.h").write_text(
                generate_msg_type_registry_h(), encoding="utf-8")
            print(f"  生成: {emsgid_out / 'MsgTypeRegistry.gen.h'}")
            (emsgid_out / "MsgTypeRegistry.gen.cpp").write_text(
                generate_msg_type_registry_cpp(req_names0, msg_id_entries0), encoding="utf-8")
            print(f"  生成: {emsgid_out / 'MsgTypeRegistry.gen.cpp'}")
    if args.only_emsgid:
        print("\n✅ EMsgID 绑定生成完成（--only-emsgid）")
        return

    # 1. 解析 .proto（公共 + <Service>/ 子目录；排除 MsgID.proto 单独处理）
    proto_files = collect_service_protos(proto_dir, args.service)

    if not proto_files:
        print("错误：未找到 .proto 文件", file=sys.stderr)
        sys.exit(1)

    for info in proto_files.values():
        status = "有 *Req" if info.has_req else "仅辅助类型"
        print(f"  解析: {info.filename} ({status}, {len(info.messages)} 个消息)")

    # 2. 依赖闭包
    closure = compute_closure(proto_files)
    print(f"\n依赖闭包 ({len(closure)} 个文件):")
    for info in closure:
        print(f"  - {info.filename}")

    # 2.5 MsgID.proto → EMsgID 枚举
    msg_id_path = proto_dir / "MsgID.proto"
    if not msg_id_path.is_file():
        print(f"错误：缺少 {msg_id_path}", file=sys.stderr)
        sys.exit(1)
    msg_id_entries = parse_msg_id_proto(msg_id_path)
    print(f"\n解析: {msg_id_path.name} ({len(msg_id_entries)} 个枚举值)")

    # 3. 每文件 .gen.cpp
    for info in closure:
        content = generate_proto_gen_cpp(info, proto_files)
        out_path = cpp_out / f"{info.name}.gen.cpp"
        out_path.write_text(content, encoding="utf-8")
        print(f"  生成: {out_path}")

    # 4. 汇总
    (cpp_out / "ProtoBindIndex.gen.h").write_text(generate_index_h(closure), encoding="utf-8")
    print(f"  生成: {cpp_out / 'ProtoBindIndex.gen.h'}")
    # include 路径：从 project root 到 cpp_out 的相对路径（如 World/AutoGen）。
    # 约定 cpp_out 位于 <project>/Src/<Service>/AutoGen，取 "Src" 之后的片段。
    cpp_out_rel = "World/AutoGen"
    try:
        src_idx = str(cpp_out).replace("\\", "/").find("Src/")
        if src_idx >= 0:
            cpp_out_rel = str(cpp_out).replace("\\", "/")[src_idx + len("Src/"):]
    except Exception:
        pass
    (cpp_out / "ProtoBindIndex.gen.cpp").write_text(
        generate_index_cpp(closure, msg_id_entries, cpp_out_rel), encoding="utf-8")
    print(f"  生成: {cpp_out / 'ProtoBindIndex.gen.cpp'}")

    # 4.5 GameEvent 绑定（ECS_06 决策 3：类型化事件自动绑定）
    #     生成 GameEventBindings.gen.{h,cpp} 到 cpp_out（World/AutoGen）
    event_msgs = collect_game_events(proto_files)
    # 从 GameEvent.proto 的 enum EGameEventType 解析真实值（含 GAME_EVENT_NONE=0），
    # 传给生成器——避免按消息顺序硬编码导致中间插值错位。
    game_event_proto = proto_files.get("GameEvent.proto")
    event_enum_values = (game_event_proto.enums.get("EGameEventType")
                         if game_event_proto else None) or []
    if event_msgs:
        (cpp_out / "GameEventBindings.gen.h").write_text(
            generate_game_event_bindings_h(event_msgs), encoding="utf-8")
        print(f"  生成: {cpp_out / 'GameEventBindings.gen.h'} ({len(event_msgs)} 个事件)")
        (cpp_out / "GameEventBindings.gen.cpp").write_text(
            generate_game_event_bindings_cpp(event_msgs, event_enum_values), encoding="utf-8")
        print(f"  生成: {cpp_out / 'GameEventBindings.gen.cpp'}")
    else:
        print("  ⚠️  未找到 GameEvent.proto 的 *Event 消息，跳过事件绑定生成")

    # 4.6 宏命名查表（ScriptLayer_06 §3.1：命名约定单一真相源）
    #     宏 apply() 查「类型名→枚举值」，das 侧零命名规则，-1 = 未注册。
    #     分层：MsgTypeToID（消息，绑 Common）→ 公共层 emsgid_out；
    #           EventTypeToID（事件，绑 world）→ 服务层 cpp_out。
    req_msg_names = [m.name for info in closure for m in info.messages if m.name.endswith("Req")]
    if req_msg_names:
        # MsgTypeToID —— 公共层（[msg_handler] 宏只 require Common，宏 apply 可见）
        msg_reg_out = Path(args.emsgid_out) if args.emsgid_out else proto_dir / "AutoGen"
        msg_reg_out.mkdir(parents=True, exist_ok=True)
        (msg_reg_out / "MsgTypeRegistry.gen.h").write_text(
            generate_msg_type_registry_h(), encoding="utf-8")
        print(f"  生成: {msg_reg_out / 'MsgTypeRegistry.gen.h'}")
        (msg_reg_out / "MsgTypeRegistry.gen.cpp").write_text(
            generate_msg_type_registry_cpp(req_msg_names, msg_id_entries), encoding="utf-8")
        print(f"  生成: {msg_reg_out / 'MsgTypeRegistry.gen.cpp'}")
    else:
        print("  ⚠️  无 *Req 消息，跳过 MsgTypeRegistry 生成")

    if event_msgs:
        # EventTypeToID —— 服务层（[game_event] 宏 require world，宏 apply 可见）
        event_msg_names = [m.name for m in event_msgs]
        (cpp_out / "EventTypeRegistry.gen.h").write_text(
            generate_event_type_registry_h(), encoding="utf-8")
        print(f"  生成: {cpp_out / 'EventTypeRegistry.gen.h'}")
        (cpp_out / "EventTypeRegistry.gen.cpp").write_text(
            generate_event_type_registry_cpp(event_msg_names, event_enum_values),
            encoding="utf-8")
        print(f"  生成: {cpp_out / 'EventTypeRegistry.gen.cpp'}")
    else:
        print("  ⚠️  无 *Event 消息，跳过 EventTypeRegistry 生成")

    # 5. 清理旧 das 产物——仅当显式传 --purge-legacy-das（已完成手写迁移）时执行。
    #    默认不删：否则一次普通构建/测试就会删掉尚在使用的 HandlerRegistry.das。
    if args.purge_legacy_das:
        das_autogen = proto_dir.parent.parent / "Script" / "AutoGen"
        for stale in ("HandlerRegistry.das", "MsgIDConstants.das"):
            p = das_autogen / stale
            if p.exists():
                p.unlink()
                print(f"  删除旧产物: {p}（--purge-legacy-das）")

    print("\n✅ 全部生成完成")
    print("⚠️  新增/删除含 *Req 的 .proto 后，需执行一次 xmake f -c 让通配符重新扫描")


if __name__ == "__main__":
    main()
