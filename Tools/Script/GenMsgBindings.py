#!/usr/bin/env python3
"""
GenMsgBindings.py — Protobuf → daScript 消息绑定代码生成器

按文档 23_ProtoScriptBinding.md 实现：
  - 扫描 Src/Proto/*.proto，识别 *Req 消息
  - 计算依赖闭包（*Req 所在文件 + 被引用的类型所在文件）
  - 每个闭包内的 .proto → <ProtoFileName>.gen.cpp
  - 产出 ProtoBindIndex.gen.{h,cpp}（汇总）
  - 产出 Script/AutoGen/HandlerRegistry.das

用法:
  python GenMsgBindings.py --proto-dir Src/Proto --cpp-out Src/World/AutoGen --das-out Script/AutoGen
"""

import argparse
import os
import re
import sys
from collections import defaultdict, deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

# ── 修复 Windows 编码 ──
if sys.platform == "win32":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# ═══════════════════════════════════════════════════════════════
# Proto 解析（轻量——仅提取本生成器所需的信息）
# ═══════════════════════════════════════════════════════════════

@dataclass
class FieldInfo:
    """protobuf 字段描述"""
    name: str            # 字段名（snake_case）
    type_name: str       # 类型名（Proto 类型名或标量）
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
    package: str         # package 名
    imports: list[str]   # import 的文件名列表
    messages: list[MessageInfo] = field(default_factory=list)
    has_req: bool = False   # 是否包含 *Req 消息

# ── Regex 集合 ──
_RE_PACKAGE = re.compile(r"^\s*package\s+([\w.]+)\s*;", re.MULTILINE)
_RE_IMPORT  = re.compile(r'^\s*import\s+"([^"]+)"\s*;', re.MULTILINE)
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

# 标量类型集合
SCALAR_TYPES = {
    "double", "float", "int32", "int64", "uint32", "uint64",
    "sint32", "sint64", "fixed32", "fixed64", "sfixed32", "sfixed64",
    "bool", "enum",
}
STRING_TYPES = {"string"}
BYTES_TYPES = {"bytes"}

def parse_msg_id_proto(filepath: Path) -> list[tuple[str, int]]:
    """
    解析 MsgID.proto，提取 EMsgID 枚举值。
    返回 [(name, value), ...] 列表，过滤掉名字含 SENTINEL / _MIN_ / _MAX_ 的项。
    """
    text = filepath.read_text(encoding="utf-8")
    # 匹配 MSG_XXX = N; 格式
    pattern = re.compile(r"^\s*([A-Z_][A-Z0-9_]*)\s*=\s*(\d+)\s*;", re.MULTILINE)
    entries: list[tuple[str, int]] = []
    for m in pattern.finditer(text):
        name = m.group(1)
        value = int(m.group(2))
        if any(kw in name for kw in ("SENTINEL", "_MIN_", "_MAX_")):
            continue
        entries.append((name, value))
    return entries


def parse_proto_file(filepath: Path) -> ProtoFileInfo:
    """解析一个 .proto 文件，返回结构化信息"""
    text = filepath.read_text(encoding="utf-8")
    basename = filepath.name
    name = filepath.stem  # 不带扩展名

    # Package
    pkg_match = _RE_PACKAGE.search(text)
    package = pkg_match.group(1) if pkg_match else ""

    # Imports
    imports = _RE_IMPORT.findall(text)

    # Messages
    messages = []
    for msg_match in _RE_MESSAGE_START.finditer(text):
        msg_name = msg_match.group(1)
        fields = []

        # 找到消息体的结束位置——简单方案：扫描大括号
        start = msg_match.end()
        brace_depth = 1
        pos = start
        while pos < len(text) and brace_depth > 0:
            if text[pos] == '{':
                brace_depth += 1
            elif text[pos] == '}':
                brace_depth -= 1
            pos += 1
        body = text[start:pos]

        # 解析 map 字段
        for m in _RE_MAP_FIELD.finditer(body):
            fields.append(FieldInfo(
                name=m.group(3),
                type_name=f"map<{m.group(1)},{m.group(2)}>",
                number=int(m.group(4)),
                label="",
                is_map=True,
            ))

        # 解析 repeated/optional 字段
        for m in _RE_FIELD.finditer(body):
            raw_type = m.group(2)
            type_name = raw_type.split(".")[-1]  # 去掉包前缀
            fields.append(FieldInfo(
                name=m.group(3),
                type_name=type_name,
                number=int(m.group(4)),
                label=m.group(1),
                is_scalar=(raw_type in SCALAR_TYPES),
                is_string=(raw_type in STRING_TYPES),
                is_bytes=(raw_type in BYTES_TYPES),
            ))

        # 解析普通（非 repeated/optional）字段
        for m in _RE_FIELD_SCALAR.finditer(body):
            raw_type = m.group(1)
            # 跳过 map（已处理）、跳过 enum 内联定义、跳过 oneof
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

    return ProtoFileInfo(
        name=name,
        filename=basename,
        package=package,
        imports=imports,
        messages=messages,
        has_req=has_req,
    )


# ═══════════════════════════════════════════════════════════════
# 依赖闭包计算
# ═══════════════════════════════════════════════════════════════

def compute_closure(
    proto_files: dict[str, ProtoFileInfo],
) -> list[ProtoFileInfo]:
    """
    计算依赖闭包：
      1. 所有含 *Req 的 .proto 文件
      2. 这些 *Req 消息（递归）引用的其它 message 所在的 .proto 文件
    返回按依赖顺序排列的列表（被引用类型所在文件在前）。
    """
    # 首先收集所有含 *Req 的文件
    req_files: set[str] = set()
    for fname, info in proto_files.items():
        if info.has_req:
            req_files.add(fname)

    # 构建类型→文件映射
    type_to_file: dict[str, str] = {}
    for fname, info in proto_files.items():
        for msg in info.messages:
            type_to_file[msg.name] = fname

    # BFS 计算闭包
    closure: set[str] = set(req_files)
    queue = deque(req_files)

    while queue:
        cur = queue.popleft()
        info = proto_files[cur]
        for msg in info.messages:
            for field in msg.fields:
                if field.is_scalar or field.is_string or field.is_bytes or field.is_map:
                    continue
                # 嵌套 message 类型
                ref_file = type_to_file.get(field.type_name)
                if ref_file and ref_file not in closure:
                    closure.add(ref_file)
                    queue.append(ref_file)

    # 按依赖关系排序（被引用多的在前）
    # 简单实现：按被引用次数降序
    ref_count: dict[str, int] = defaultdict(int)
    for fname in closure:
        info = proto_files[fname]
        for msg in info.messages:
            for field in msg.fields:
                if not (field.is_scalar or field.is_string or field.is_bytes or field.is_map):
                    ref_file = type_to_file.get(field.type_name)
                    if ref_file and ref_file in closure:
                        ref_count[ref_file] += 1

    # 排序：被引用多的在前；同引用数按文件名排
    sorted_files = sorted(closure, key=lambda f: (-ref_count.get(f, 0), f))

    return [proto_files[f] for f in sorted_files]


# ═══════════════════════════════════════════════════════════════
# C++ 生成：单个 .gen.cpp
# ═══════════════════════════════════════════════════════════════

def cpp_field_type(raw_type: str) -> str:
    """protobuf 类型 → C++ 函数签名中的返回类型"""
    type_map = {
        "double": "double", "float": "float",
        "int32": "int32_t", "int64": "int64_t",
        "uint32": "uint32_t", "uint64": "uint64_t",
        "sint32": "int32_t", "sint64": "int64_t",
        "fixed32": "uint32_t", "fixed64": "uint64_t",
        "sfixed32": "int32_t", "sfixed64": "int64_t",
        "bool": "bool",
    }
    return type_map.get(raw_type, raw_type)


def camel_to_msg_id(msg_name: str) -> str:
    """MoveReq → MSG_MOVE_REQ"""
    snake = re.sub(r"(?<![A-Z])([A-Z])", r"_\1", msg_name)
    snake = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", snake)
    return "MSG" + snake.upper()


def generate_proto_gen_cpp(info: ProtoFileInfo, proto_files: dict[str, ProtoFileInfo]) -> str:
    """为单个 .proto 文件生成 .gen.cpp"""
    lines = []
    ns = "MMO::Proto"  # 项目使用 MMO.Proto 包

    # ── 文件头 ──
    has_dispatch = info.has_req
    brief = f"消息类型注册 + 消息分发" if has_dispatch else f"被业务消息引用的辅助类型注册"
    lines.append(f"/**")
    lines.append(f" * @file {info.name}.gen.cpp")
    lines.append(f" * @brief 自动生成文件——{info.filename} {brief}")
    lines.append(f" *")
    lines.append(f" * 生成工具: Tools/Script/GenMsgBindings.py")
    lines.append(f" * 来源: Src/Proto/{info.filename}")
    lines.append(f" * @warning 不要手动编辑")
    if has_dispatch:
        req_names = [m.name for m in info.messages if m.name.endswith("Req")]
        lines.append(f" * @note {', '.join(req_names)} 覆写了 canCopy/canClone 为 false——脚本不能把 req 存进")
        lines.append(f" *       全局变量或结构体字段，只能在本次调用内读取字段/按引用传递。")
    lines.append(f" */")
    # ── Include：自己的 .pb.h 加上依赖类型所在的 .pb.h ──
    lines.append(f"#include \"Proto/AutoGen/{info.name}.pb.h\"")
    # 收集本文件引用到的所有嵌套类型（跨文件依赖）
    referenced_types: set[str] = set()
    for msg in info.messages:
        for field in msg.fields:
            if not (field.is_scalar or field.is_string or field.is_bytes or field.is_map) and field.label != "repeated":
                referenced_types.add(field.type_name)
    # 对每个引用类型，include 其所在 .proto 的 .pb.h
    for ref_type in referenced_types:
        for other_info in proto_files.values():
            if other_info.name == info.name:
                continue
            for other_msg in other_info.messages:
                if other_msg.name == ref_type:
                    lines.append(f"#include \"Proto/AutoGen/{other_info.name}.pb.h\"")

    if info.has_req:
        lines.append(f"#include <MsgID.pb.h>")
    lines.append(f"#include \"World/ScriptDispatchRegistry.h\"")
    lines.append(f"#include \"World/WorldServer.h\"")
    lines.append(f"")
    lines.append(f"#include <daScript/simulate/simulate.h>")
    lines.append(f"#include <daScript/ast/ast_interop.h>")
    lines.append(f"#include <daScript/ast/ast_handle.h>")
    lines.append(f"#include <daScript/daScriptModule.h>")
    lines.append(f"#include <daScript/ast/ast_typefactory.h>")
    lines.append(f"")

    # MAKE_TYPE_FACTORY：为本文件定义的消息类型 + 所有被引用的跨文件类型提供 typeName
    for msg in info.messages:
        lines.append(f"MAKE_TYPE_FACTORY({msg.name}, {ns}::{msg.name})")
    for ref_type in referenced_types:
        # 找出引用类型的完整命名空间路径
        for proto_file in proto_files.values():
            for other_msg in proto_file.messages:
                if other_msg.name == ref_type and other_msg.name not in {m.name for m in info.messages}:
                    lines.append(f"MAKE_TYPE_FACTORY({ref_type}, {ns}::{ref_type})")

    # ── 匿名命名空间 ──
    lines.append(f"namespace")
    lines.append(f"{{")
    lines.append(f"    using namespace das;")
    lines.append(f"")

    # ── 为每个 message 生成 Annotation ──
    req_names = {m.name for m in info.messages if m.name.endswith("Req")}
    value_type_names = set()
    # 确定值类型：被 *Req 引用但不是 *Req 本身的类型
    for msg in info.messages:
        if msg.name not in req_names:
            value_type_names.add(msg.name)

    # 辅助函数生成——排除 repeated 字段（repeated 单独处理）
    string_field_funcs = []
    repeated_size_funcs = []
    repeated_index_funcs = []
    for msg in info.messages:
        for field in msg.fields:
            if field.is_map:
                continue
            if field.label == "repeated":
                # repeated 字段：生成 _size + 索引访问
                size_func = f"{msg.name}_{field.name[0].upper()}{field.name[1:]}_Size"
                repeated_size_funcs.append((size_func, msg.name, field))
                index_func = f"{msg.name}_{field.name[0].upper()}{field.name[1:]}"
                repeated_index_funcs.append((index_func, msg.name, field))
            elif field.is_string or field.is_bytes:
                # 非 repeated 的 string/bytes：生成 getter 辅助函数
                func_name = f"{msg.name}_Get{field.name[0].upper()}{field.name[1:]}"
                string_field_funcs.append((func_name, msg.name, field))

    # 产出 string/bytes 辅助函数
    for func_name, msg_name, field in string_field_funcs:
        lines.append(f"    // ── string/bytes 字段：用 addExternProperty 注册，脚本侧写 req.{field.name}（无括号）──")
        lines.append(f"    const char *{func_name}(const {ns}::{msg_name} &msg)")
        lines.append(f"    {{")
        if field.is_string:
            lines.append(f"        return msg.{field.name}().c_str();")
        else:
            lines.append(f"        return reinterpret_cast<const char *>(msg.{field.name}().data());")
        lines.append(f"    }}")
        lines.append(f"")

    # 产出 repeated _size 函数
    for func_name, msg_name, field in repeated_size_funcs:
        lines.append(f"    // ── repeated _size：用 addExternProperty，无括号 ──")
        lines.append(f"    int {func_name}(const {ns}::{msg_name} &msg)")
        lines.append(f"    {{")
        lines.append(f"        return msg.{field.name}_size();")
        lines.append(f"    }}")
        lines.append(f"")

    # 产出 repeated 索引访问函数
    for func_name, msg_name, field in repeated_index_funcs:
        if field.is_string:
            ret_type = "const char *"
            access_expr = f"msg.{field.name}(index).c_str()"
        elif field.is_bytes:
            ret_type = "const char *"
            access_expr = f"reinterpret_cast<const char *>(msg.{field.name}(index).data())"
        elif field.is_scalar:
            ret_type = cpp_field_type(field.type_name)
            access_expr = f"msg.{field.name}(index)"
        else:
            ret_type = f"const {ns}::{field.type_name} &"
            access_expr = f"msg.{field.name}(index)"
        lines.append(f"    // ── repeated 索引访问：需要 index 参数，必须带括号 ──")
        lines.append(f"    {ret_type}{func_name}(const {ns}::{msg_name} &msg, int index)")
        lines.append(f"    {{")
        lines.append(f"        return {access_expr};")
        lines.append(f"    }}")
        lines.append(f"")

    # ── Annotation 类 ──
    for msg in info.messages:
        is_req = msg.name in req_names
        is_value = msg.name in value_type_names
        can_copy_str = "" if is_value else """
        // §2.4 生命周期防护——阻断脚本把 req 赋值/克隆进任何逃逸本次调用的存储位置
        virtual bool canCopy() const override { return false; }
        virtual bool canClone() const override { return false; }"""

        lines.append(f"    /**")
        lines.append(f"     * @brief {msg.name} 的 daScript 类型注解")
        if is_value:
            lines.append(f"     * @note 值类型——脚本可安全复制/克隆保存")
        else:
            lines.append(f"     * @note 引用类型（网络消息）——canCopy/canClone = false")
        lines.append(f"     */")
        lines.append(f"    struct {msg.name}Annotation : ManagedStructureAnnotation<{ns}::{msg.name}, false, false>")
        lines.append(f"    {{")
        lines.append(f"        {msg.name}Annotation(ModuleLibrary &ml)")
        lines.append(f"            : ManagedStructureAnnotation(\"{msg.name}\", ml, \"{ns}::{msg.name}\")")
        lines.append(f"        {{")

        # addProperty 绑定（标量、嵌套 message 直接用 addProperty）
        for field in msg.fields:
            if field.is_map:
                lines.append(f"            // TODO(GenMsgBindings): {msg.name}.{field.name} (map) 需手写绑定，暂未生成")
                continue
            if field.is_string or field.is_bytes:
                continue  # 用 addExternProperty，在 Register 函数里
            if field.label == "repeated" and not field.is_map:
                continue  # 用 addExtern/addExternProperty，在 Register 函数里
            # 标量、嵌套 message 直接用 addProperty 绑定
            lines.append(f"            addProperty<DAS_BIND_MANAGED_PROP({field.name})>(\"{field.name}\");")

        lines.append(f"        }}")
        lines.append(f"{can_copy_str}")
        lines.append(f"    }};")
        lines.append(f"")

    # ── Dispatch 函数（仅 *Req 消息）──
    dispatch_funcs = []
    for msg in info.messages:
        if msg.name not in req_names:
            continue
        msg_id_const = f"MMO::Proto::{camel_to_msg_id(msg.name)}"

        func_name = f"Dispatch{msg.name}"
        dispatch_funcs.append((func_name, msg, msg_id_const))

        lines.append(f"    bool {func_name}(MMO::WorldServer &server, uint32 sessionID, const uint8 *body, size_t len)")
        lines.append(f"    {{")
        lines.append(f"        {ns}::{msg.name} req;")
        lines.append(f"        if (!req.ParseFromArray(body, static_cast<int>(len)))")
        lines.append(f"        {{")
        lines.append(f"            MMO::Log::Error(\"{info.name}.gen: {msg.name} parse failed, session={{}}\", sessionID);")
        lines.append(f"            return false;")
        lines.append(f"        }}")
        lines.append(f"")
        lines.append(f"        das::Context *ctx        = server.GetScriptContext();")
        lines.append(f"        auto          fnDispatch = server.GetDispatchMsgFunction();")
        lines.append(f"        if (!ctx || !fnDispatch)")
        lines.append(f"        {{")
        lines.append(f"            return false;")
        lines.append(f"        }}")
        lines.append(f"")
        lines.append(f"        vec4f callArgs[3] = {{")
        lines.append(f"            das::cast<uint32_t>::from(static_cast<uint32_t>({msg_id_const})),")
        lines.append(f"            das::cast<uint32_t>::from(sessionID),")
        lines.append(f"            das::cast<const {ns}::{msg.name} *>::from(&req),")
        lines.append(f"        }};")
        lines.append(f"        ctx->eval(fnDispatch, callArgs);")
        lines.append(f"        return true;")
        lines.append(f"    }}")
        lines.append(f"")

    lines.append(f"}} // namespace")
    lines.append(f"")

    # ── namespace MMO ──
    lines.append(f"namespace MMO")
    lines.append(f"{{")
    lines.append(f"")

    # RegisterXxxProtoBindings
    lines.append(f"    void Register{info.name}ProtoBindings(das::Module &mod, das::ModuleLibrary &lib)")
    lines.append(f"    {{")
    lines.append(f"        (void)mod;")
    has_annotation = False
    for msg in info.messages:
        lines.append(f"        mod.addAnnotation(new {msg.name}Annotation(lib));")
        has_annotation = True
        # string/bytes addExternProperty（仅对非 repeated 的 string/bytes 字段）
        # 注意：属性名必须是 ".`字段名"——daScript 的 expr.field 语法在推断阶段
        # 精确查找 ".`" + 字段名 这个函数名（见 ast_infer_type.cpp 的字段解析），
        # ManagedStructureAnnotation::addProperty 内部就是这样拼接的；直接调用
        # 底层 addExternProperty 时必须手动拼上这个前缀，否则只能以自由函数调用
        # （从 addExtern 的角度看是一样的，但脚本侧不能写 req.username 这种字段语法）。
        for field in msg.fields:
            if (field.is_string or field.is_bytes) and field.label != "repeated":
                func_name = f"{msg.name}_Get{field.name[0].upper()}{field.name[1:]}"
                lines.append(f"        das::addExternProperty<DAS_BIND_FUN({func_name})>(mod, lib, \".`{field.name}\", \"{func_name}\")")
                lines.append(f"            ->args({{\"msg\"}});")
            if field.label == "repeated" and not field.is_map:
                size_func = f"{msg.name}_{field.name[0].upper()}{field.name[1:]}_Size"
                index_func = f"{msg.name}_{field.name[0].upper()}{field.name[1:]}"
                lines.append(f"        das::addExternProperty<DAS_BIND_FUN({size_func})>(mod, lib, \".`{field.name}_size\", \"{size_func}\")")
                lines.append(f"            ->args({{\"msg\"}});")
                lines.append(f"        das::addExtern<DAS_BIND_FUN({index_func})>(mod, lib, \"{field.name}\", das::SideEffects::none)")
                lines.append(f"            ->args({{\"msg\", \"index\"}});")
    if not has_annotation:
        lines.append(f"        // 本文件仅含被引用类型，无 *Req 消息")
    lines.append(f"    }}")
    lines.append(f"")

    # RegisterXxxMsgDispatch（仅当有 *Req 消息）
    if dispatch_funcs:
        lines.append(f"    void Register{info.name}MsgDispatch()")
        lines.append(f"    {{")
        for func_name, msg, msg_id_const in dispatch_funcs:
            lines.append(f"        ScriptDispatchRegistry::Register(static_cast<uint32_t>({msg_id_const}), &{func_name});")
        lines.append(f"    }}")

    lines.append(f"}} // namespace MMO")
    lines.append(f"")

    return "\n".join(lines)


# ═══════════════════════════════════════════════════════════════
# 生成 ProtoBindIndex.gen.h
# ═══════════════════════════════════════════════════════════════

def generate_index_h(closure: list[ProtoFileInfo]) -> str:
    lines = []
    lines.append("/**")
    lines.append(" * @file ProtoBindIndex.gen.h")
    lines.append(" * @brief 自动生成文件——汇总声明，供 MassiveModule.cpp / WorldServer.cpp 手写代码 include")
    lines.append(" *")
    lines.append(" * 生成工具: Tools/Script/GenMsgBindings.py")
    lines.append(" * @warning 不要手动编辑")
    lines.append(" */")
    lines.append("#pragma once")
    lines.append("")
    lines.append("namespace das")
    lines.append("{")
    lines.append("    class Module;")
    lines.append("    class ModuleLibrary;")
    lines.append("} // namespace das")
    lines.append("")
    lines.append("namespace MMO")
    lines.append("{")
    lines.append("")
    lines.append("    /**")
    lines.append("     * @brief 注册当前依赖闭包内全部 .proto 文件对应的 daScript 类型")
    lines.append("     * @note 在 MassiveModule::BindFunctions() 内调用一次")
    lines.append("     */")
    lines.append("    void RegisterAllProtoMessageTypes(das::Module &mod, das::ModuleLibrary &lib);")
    lines.append("")
    lines.append("    /**")
    lines.append("     * @brief 注册当前依赖闭包内全部 .proto 文件对应的消息分发函数")
    lines.append("     * @note 在 WorldServer::InitScriptEngine() 缓存 dispatch_msg 之后调用一次")
    lines.append("     */")
    lines.append("    void RegisterAllMsgDispatch();")
    lines.append("")
    lines.append("} // namespace MMO")
    lines.append("")
    return "\n".join(lines)


# ═══════════════════════════════════════════════════════════════
# 生成 ProtoBindIndex.gen.cpp
# ═══════════════════════════════════════════════════════════════

def generate_index_cpp(closure: list[ProtoFileInfo],
                        msg_id_entries: list[tuple[str, int]]) -> str:
    lines = []
    lines.append("/**")
    lines.append(" * @file ProtoBindIndex.gen.cpp")
    lines.append(" * @brief 自动生成文件——汇总调用各 <ProtoFileName>.gen.cpp 里定义的注册函数")
    lines.append(" *")
    lines.append(" * 生成工具: Tools/Script/GenMsgBindings.py")
    lines.append(" * 来源: Src/Proto/ 下依赖闭包内的全部 .proto 文件")
    lines.append(" * @warning 不要手动编辑——新增/删除 .proto 文件后重新生成本文件")
    lines.append(" * @note 调用顺序已按依赖关系排好（被引用类型所在文件先注册）。")
    lines.append(" */")
    lines.append("#include \"World/AutoGen/ProtoBindIndex.gen.h\"")
    lines.append("")
    lines.append("#include <daScript/daScriptModule.h>")
    lines.append("#include <daScript/ast/ast.h>")
    lines.append("#include <daScript/simulate/simulate.h>")
    lines.append("#include <daScript/simulate/bind_enum.h>")
    lines.append("#include <MsgID.pb.h>")
    lines.append("")
    lines.append("namespace MMO")
    lines.append("{")
    # 前置声明——使用 extern，与定义处的 `namespace MMO { }` 一致
    for info in closure:
        lines.append(f"    extern void Register{info.name}ProtoBindings(das::Module &mod, das::ModuleLibrary &lib);")
    lines.append("")
    for info in closure:
        if info.has_req:
            lines.append(f"    extern void Register{info.name}MsgDispatch();")
    lines.append("} // namespace MMO")
    lines.append("")
    # 匿名命名空间——EMsgID 枚举绑定
    lines.append("namespace")
    lines.append("{")
    lines.append("    using namespace das;")
    lines.append("")
    lines.append("    // EMsgID 枚举绑定——替代手写的 MsgIDConstants.das")
    lines.append("    struct EnumerationEMsgID : das::Enumeration")
    lines.append("    {")
    lines.append("        EnumerationEMsgID() : das::Enumeration(\"EMsgID\")")
    lines.append("        {")
    lines.append("            external = true;")
    lines.append("            cppName = \"MMO::Proto::EMsgID\";")
    lines.append("            baseType = das::Type::tInt;")
    for name, value in msg_id_entries:
        lines.append(f"            addI(\"{name}\", {value}, das::LineInfo());")
    lines.append("        }")
    lines.append("    };")
    lines.append("")
    lines.append("} // namespace")
    lines.append("")
    lines.append("namespace MMO")
    lines.append("{")
    lines.append("")
    lines.append("    void RegisterAllProtoMessageTypes(das::Module &mod, das::ModuleLibrary &lib)")
    lines.append("    {")
    for info in closure:
        lines.append(f"        Register{info.name}ProtoBindings(mod, lib);")
    lines.append("        mod.addEnumeration(new EnumerationEMsgID());")
    lines.append("    }")
    lines.append("")
    lines.append("    void RegisterAllMsgDispatch()")
    lines.append("    {")
    for info in closure:
        if info.has_req:
            lines.append(f"        Register{info.name}MsgDispatch();")
    lines.append("    }")
    lines.append("")
    lines.append("} // namespace MMO")
    lines.append("")
    lines.append("DAS_BIND_ENUM_CAST(MMO::Proto::EMsgID)")
    lines.append("")
    return "\n".join(lines)


# ═══════════════════════════════════════════════════════════════
# 生成 HandlerRegistry.das
# ═══════════════════════════════════════════════════════════════

def generate_handler_registry_das(proto_files: dict[str, ProtoFileInfo],
                                    closure: list[ProtoFileInfo]) -> str:
    """生成 Script/AutoGen/HandlerRegistry.das"""

    # 收集所有 *Req 消息
    req_entries: list[tuple[str, str]] = []  # (MsgName, MsgIDConstant)
    for info in closure:
        for msg in info.messages:
            if msg.name.endswith("Req"):
                msg_id_const = camel_to_msg_id(msg.name)
                req_entries.append((msg.name, msg_id_const))

    lines = []
    lines.append("// ════════════════════════════════════════════════════")
    lines.append("// 自动生成 — GenMsgBindings.py")
    lines.append("//   生成时间: 2026-07-28")
    lines.append("//   来源: Src/Proto/*.proto 中的 *Req 消息 + MsgID.proto")
    lines.append("// ════════════════════════════════════════════════════")
    lines.append("")
    lines.append("options gen2")
    lines.append("options indenting = 4")
    lines.append("")
    lines.append("module HandlerRegistry")
    lines.append("")
    lines.append("require daslib/ast")
    lines.append("require daslib/ast_boost")
    lines.append("require daslib/templates_boost")
    lines.append("")
    lines.append("require massive")

    # 消息名 → msgID 表（EMsgID 是 C++ 侧绑定的枚举，需显式转 uint）
    lines.append("// ── 消息名 → msgID 编译期映射（apply() 内查表用）──")
    lines.append("let g_msg_name_to_id : table<string; uint> <- {")
    for msg_name, msg_id_const in req_entries:
        lines.append(f"    \"{msg_name}\" => uint(EMsgID.{msg_id_const}),")
    lines.append("}")
    lines.append("")

    # 期望数量
    lines.append(f"// ── 期望注册的 handler 数量（等于上表条目数）──")
    lines.append(f"let g_expected_handler_count = {len(req_entries)}")
    lines.append("")

    # 运行期分发表
    # @@(...) { ... } 在这里没有捕获任何外部变量（$c/$v 都是编译期展开），
    # 推断出的类型是 function<...>，不是 lambda<...>——两者不能混用赋值。
    lines.append("// ── 运行期分发表：msgID → handler function ──")
    lines.append("var g_handler_registry : table<uint; function<(sessionID : uint; msgPtr : void?) : void>>")
    lines.append("var g_registered_count : int = 0")
    lines.append("")

    # [msg_handler] 注解
    lines.append("// ── [msg_handler] 注解 ──")
    lines.append("[function_macro(name=\"msg_handler\")]")
    lines.append("class MsgHandlerAnnotation : AstFunctionAnnotation {")
    lines.append("    def override apply(var func : FunctionPtr; var group : ModuleGroup;")
    lines.append("                       args : AnnotationArgumentList; var errors : das_string) : bool {")
    lines.append("        // 步骤 1: msg 参数必须是字符串（消息名，如 \"MoveReq\"）——")
    lines.append("        //         daScript 语法上 msg=Xxx 的裸标识符会被解析成字符串本身，")
    lines.append("        //         此处显式要求写成 msg=\"Xxx\"，与语法行为保持一致。")
    lines.append("        let msgArg = find_arg(args, \"msg\")")
    lines.append("        if (!(msgArg is tString)) {")
    lines.append('            errors := "[msg_handler] 需要 msg=\\"<MessageName>\\" 参数（如 msg=\\"MoveReq\\"）"')
    lines.append("            return false")
    lines.append("        }")
    lines.append("        let msgName = msgArg as tString")
    lines.append("")
    lines.append("        // 步骤 2: 消息名必须在生成器扫描出的表里——防止拼错")
    lines.append("        var msgID : uint")
    lines.append("        var found = false")
    lines.append("        g_msg_name_to_id |> get(msgName) $(id) {")
    lines.append("            msgID = id")
    lines.append("            found = true")
    lines.append("        }")
    lines.append("        if (!found) {")
    lines.append('            errors := "[msg_handler] 未知消息名 \\"{msgName}\\"——检查 .proto 是否存在对应 *Req 消息，或重跑 xmake 触发生成"')
    lines.append("            return false")
    lines.append("        }")
    lines.append("")
    lines.append("        // 步骤 3: 函数签名至少两个参数 (sessionID: uint; req: <MessageName>[; 额外 string/repeated 参数...])")
    lines.append("        if (length(func.arguments) < 2) {")
    lines.append('            errors := "[msg_handler(msg=\\"{msgName}\\")] 函数至少需要两个参数: (sessionID : uint; req : {msgName})"')
    lines.append("            return false")
    lines.append("        }")
    lines.append("        if (func.arguments[0]._type.baseType != Type.tUInt || func.arguments[0]._type.fixedDim != 0) {")
    lines.append('            errors := "[msg_handler(msg=\\"{msgName}\\")] 第一个参数必须是 sessionID : uint"')
    lines.append("            return false")
    lines.append("        }")
    lines.append("")
    lines.append("        // 步骤 4: 第二个参数类型名必须严格等于消息名——按约定推导，无需额外注解")
    lines.append("        // MoveReq 等消息类型通过 ManagedStructureAnnotation 绑定，是 handled type")
    lines.append("        // （baseType == Type.tHandle），类型名要从 annotation.name 取，不是 structType")
    lines.append("        let msgType = func.arguments[1]._type")
    lines.append("        if (msgType.baseType != Type.tHandle || msgType.annotation == null) {")
    lines.append('            errors := "[msg_handler(msg=\\"{msgName}\\")] 第二个参数类型应为 {msgName}，实际不是消息类型"')
    lines.append("            return false")
    lines.append("        }")
    lines.append("        if (msgType.annotation.name != msgName) {")
    lines.append('            errors := "[msg_handler(msg=\\"{msgName}\\")] 第二个参数类型应为 {msgName}，实际是 {msgType.annotation.name}"')
    lines.append("            return false")
    lines.append("        }")
    lines.append("")
    lines.append("        // 步骤 5: 注入注册代码——把 (msgID → 转发 block) 写进全局表")
    lines.append("        // apply() 本身运行在编译期的宏 context 里（Program::makeMacroModule 为")
    lines.append("        // thisModule 单独开了一个 macroContext），这里直接写 g_registered_count")
    lines.append("        // += 1 只会改到宏 context 里的那份全局变量，运行时 _scriptCtx 是另一个")
    lines.append("        // 独立 context，看到的仍是初值 0——所以必须和 g_handler_registry 一样，")
    lines.append("        // 用 qmacro_expr 生成语句、塞进 [init] 函数体，才会在运行时 context 里")
    lines.append("        // 真正执行。setup_call_list(isInit=true) 拿到（或创建）该 [init] 函数。")
    lines.append('        var initBlk <- setup_call_list("msg_handler`init", func.at, true, true)')
    lines.append("        initBlk.list |> push(qmacro_expr(${")
    lines.append("            g_handler_registry[$v(msgID)] = @@(sessionID : uint; msgPtr : void?) {")
    lines.append("                let typedMsg = unsafe(reinterpret<$t(msgType)?> msgPtr)")
    lines.append('                $c("_::{func.name}")(sessionID, *typedMsg)')
    lines.append("            }")
    lines.append("        }))")
    lines.append('        initBlk.list |> push(qmacro_expr(${ g_registered_count += 1 }))')
    lines.append("        func.flags.privateFunction = true")
    lines.append("        return true")
    lines.append("    }")
    lines.append("}")
    lines.append("")

    # dispatch_msg 入口
    lines.append("// ── C++ 唯一调用入口 ──")
    lines.append("[export]")
    lines.append("def dispatch_msg(msgID : uint; sessionID : uint; msgPtr : void?) {")
    lines.append("    g_handler_registry |> get(msgID) $(handler) {")
    lines.append("        invoke(handler, sessionID, msgPtr)")
    lines.append("    }")
    lines.append("}")
    lines.append("")

    # 完整性校验
    lines.append("// ── 完整性校验：handler 数量必须等于生成器扫描出的期望数量 ──")
    lines.append("[export]")
    lines.append("def validate_handler_registry {")
    lines.append("    if (g_registered_count != g_expected_handler_count) {")
    lines.append('        panic("handler 完整性校验失败: 期望 {g_expected_handler_count} 个 [msg_handler]，实际注册 {g_registered_count} 个——检查 Handlers.das 是否有消息漏写或漏加注解")')
    lines.append("    }")
    lines.append("}")
    lines.append("")

    return "\n".join(lines)


# ═══════════════════════════════════════════════════════════════
# 主入口
# ═══════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description="Protobuf → daScript 消息绑定代码生成器")
    parser.add_argument("--proto-dir", required=True, help="Src/Proto 目录路径")
    parser.add_argument("--cpp-out", required=True, help="C++ 生成文件输出路径（如 Src/World/AutoGen）")
    parser.add_argument("--das-out", required=True, help="daScript 生成文件输出路径（如 Script/AutoGen）")
    args = parser.parse_args()

    proto_dir = Path(args.proto_dir)
    cpp_out = Path(args.cpp_out)
    das_out = Path(args.das_out)

    # 确保输出目录存在
    cpp_out.mkdir(parents=True, exist_ok=True)
    das_out.mkdir(parents=True, exist_ok=True)

    # 1. 解析所有 .proto 文件
    proto_files: dict[str, ProtoFileInfo] = {}
    exclude_names = {"MsgID.proto"}
    for proto_path in sorted(proto_dir.glob("*.proto")):
        if proto_path.name in exclude_names:
            continue
        info = parse_proto_file(proto_path)
        proto_files[info.filename] = info

    if not proto_files:
        print("错误：未找到 .proto 文件", file=sys.stderr)
        sys.exit(1)

    for info in proto_files.values():
        status = "有 *Req" if info.has_req else "仅辅助类型"
        print(f"  解析: {info.filename} ({status}, {len(info.messages)} 个消息)")

    # 2. 计算依赖闭包
    closure = compute_closure(proto_files)
    print(f"\n依赖闭包 ({len(closure)} 个文件):")
    for info in closure:
        print(f"  - {info.filename}")

    # 2.5 解析 MsgID.proto——生成 EMsgID 枚举绑定用
    msg_id_proto_path = proto_dir / "MsgID.proto"
    msg_id_entries = parse_msg_id_proto(msg_id_proto_path)
    print(f"\n解析: {msg_id_proto_path.name} ({len(msg_id_entries)} 个枚举值)")

    # 3. 生成每个文件的 .gen.cpp
    for info in closure:
        content = generate_proto_gen_cpp(info, proto_files)
        out_path = cpp_out / f"{info.name}.gen.cpp"
        out_path.write_text(content, encoding="utf-8")
        print(f"  生成: {out_path}")

    # 4. 生成汇总文件
    index_h = generate_index_h(closure)
    index_h_path = cpp_out / "ProtoBindIndex.gen.h"
    index_h_path.write_text(index_h, encoding="utf-8")
    print(f"  生成: {index_h_path}")

    index_cpp = generate_index_cpp(closure, msg_id_entries)
    index_cpp_path = cpp_out / "ProtoBindIndex.gen.cpp"
    index_cpp_path.write_text(index_cpp, encoding="utf-8")
    print(f"  生成: {index_cpp_path}")

    # 5. 生成 HandlerRegistry.das
    das_content = generate_handler_registry_das(proto_files, closure)
    das_path = das_out / "HandlerRegistry.das"
    das_path.write_text(das_content, encoding="utf-8")
    print(f"  生成: {das_path}")

    # 6. 删除旧的 MsgIDConstants.das（已由 C++ EMsgID 绑定替代）
    old_constants = das_out / "MsgIDConstants.das"
    if old_constants.exists():
        old_constants.unlink()
        print(f"  删除: {old_constants}（已由 C++ EMsgID 绑定替代）")

    print(f"\n✅ 全部生成完成")
    print(f"⚠️  新增/删除含 *Req 的 .proto 文件后，需执行一次 xmake f -c 让 xmake 通配符重新扫描")


if __name__ == "__main__":
    main()
