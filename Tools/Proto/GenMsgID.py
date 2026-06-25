# GenMsgID.py — 扫描业务 .proto，增量生成 MsgID.proto / InternalMsgID.proto
#
# 规则:
#   1. 扫描指定目录下所有 *.proto（排除 MsgID.proto/InternalMsgID.proto 自身）
#   2. 提取顶层 message Xxx(Req|Rsp|Ntf) → 转 MSG_XXX_后缀
#   3. 读现有 proto:
#        - 已存在的消息 → ID 保持不变
#        - 新消息 → 分配 max(已有 ID) + 1
#        - 已删除的消息 → enum 项保留并标记 [deprecated]，ID 不复用
#   4. 重写 proto（确定性输出）
#
# 用法:
#   # 客户端消息（扫描顶层 .proto）
#   python GenMsgID.py --proto-dir Src/Proto --enum-name EMsgID --output MsgID.proto
#   # 内部 RPC 消息（扫描 Internal/ 子目录）
#   python GenMsgID.py --proto-dir Src/Proto --subdir Internal --enum-name EInternalMsgID \\
#                      --output Internal/InternalMsgID.proto

import argparse
import re
import sys
from pathlib import Path

# 修复 Windows GBK 终端编码
if sys.platform == "win32":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# 顶层 message 且以 Req/Rsp/Ntf 结尾
_MESSAGE_RE = re.compile(r"^\s*message\s+(\w+(?:Req|Rsp|Ntf))\s*\{", re.MULTILINE)

# 已有序号 enum 项: MSG_XXX = 123;（可能带 // [deprecated] ...）
_ENUM_ENTRY_RE = re.compile(r"^\s*(MSG_\w+)\s*=\s*(\d+)\s*;(.*)$", re.MULTILINE)


def CamelToMsgName(camel: str) -> str:
    """MoveReq → MSG_MOVE_REQ"""
    # 在小写→大写边界插入下划线
    snake = re.sub(r"(?<![A-Z])([A-Z])", r"_\1", camel)
    snake = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", snake)
    return "MSG" + snake.upper()


def ScanMessages(protoDir: Path, subdir: str, excludeNames: set[str]) -> list[str]:
    """扫描 .proto，返回 MSG_XXX 名字列表（按文件名+出现顺序）"""
    names: list[str] = []
    seen: set[str] = set()
    scanDir = protoDir / subdir if subdir else protoDir

    for protoPath in sorted(scanDir.glob("*.proto")):
        if protoPath.name in excludeNames:
            continue
        text = protoPath.read_text(encoding="utf-8")
        for match in _MESSAGE_RE.finditer(text):
            msgName = CamelToMsgName(match.group(1))
            if msgName not in seen:
                seen.add(msgName)
                names.append(msgName)

    return names


def ParseExisting(outputPath: Path) -> tuple[dict[str, int], set[str]]:
    """解析现有 MsgID.proto，返回 ({名: ID}, 已废弃名集合)"""
    mapping: dict[str, int] = {}
    deprecated: set[str] = set()

    if not outputPath.exists():
        return mapping, deprecated

    text = outputPath.read_text(encoding="utf-8")
    for match in _ENUM_ENTRY_RE.finditer(text):
        name = match.group(1)
        msgId = int(match.group(2))
        trailing = match.group(3)
        if name == "MSG_NONE":
            continue
        mapping[name] = msgId
        if "deprecated" in trailing.lower():
            deprecated.add(name)

    return mapping, deprecated


def AssignIDs(
    currentMsgs: list[str],
    existing: dict[str, int],
    deprecated: set[str],
) -> tuple[dict[str, int], set[str]]:
    """
    计算最终的 {名: ID} 映射 + 废弃集合。
    - 已有保持不变；新增分配 max+1；
    - 现有 proto 已删但记录里有的 → 标记废弃，ID 保留
    """
    result = dict(existing)
    nextId = max(existing.values(), default=0) + 1

    currentSet = set(currentMsgs)

    # 新消息分配 ID
    for name in currentMsgs:
        if name not in result:
            result[name] = nextId
            nextId += 1

    # 记录里有但当前 proto 已删 → 废弃
    newDeprecated = set(deprecated)
    for name in result:
        if name not in currentSet:
            newDeprecated.add(name)
        else:
            # 重新出现的消息取消废弃
            newDeprecated.discard(name)

    return result, newDeprecated


def WriteMsgID(outputPath: Path, mapping: dict[str, int], deprecated: set[str],
               enumName: str, subdir: str = "") -> None:
    """生成 MsgID.proto（按 ID 升序，确定性输出）"""
    pkg = "MMO.Internal" if subdir else "MMO"

    # 零值枚举项命名约定：
    #   EMsgID         → MSG_NONE
    #   EInternalMsgID → INTERNAL_MSG_NONE
    none_name = "INTERNAL_MSG_NONE" if subdir else "MSG_NONE"

    lines = [
        f"// {outputPath.name} — 消息 ID 枚举",
        "// ⚠ 本文件由 Tools/Proto/GenMsgID.py 自动生成并维护——不要手动编辑 enum 值。",
        "//   已有消息 ID 永不变更，新消息追加，删除的消息保留为 [deprecated] 不复用 ID。",
        'syntax = "proto3";',
        "",
        f"package {pkg};",
        "",
        f"enum {enumName}",
        "{",
        f"    {none_name} = 0;",
    ]

    for name, msgId in sorted(mapping.items(), key=lambda kv: kv[1]):
        if name in deprecated:
            lines.append(f"    {name} = {msgId};  // [deprecated] 消息已删除，ID 保留不复用")
        else:
            lines.append(f"    {name} = {msgId};")

    lines.append("}")
    lines.append("")

    content = "\n".join(lines)

    # 内容无变化则不写（保护时间戳，利于增量）
    if outputPath.exists() and outputPath.read_text(encoding="utf-8") == content:
        return

    outputPath.write_text(content, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description="增量生成 MsgID.proto")
    parser.add_argument("--proto-dir", required=True, help="业务 .proto 所在根目录")
    parser.add_argument("--subdir", default="",
                        help="子目录名（如 Internal），空=扫描根目录下 *.proto")
    parser.add_argument("--enum-name", default="EMsgID", help="生成 enum 名称")
    parser.add_argument("--output", required=True, help="输出文件路径（相对 proto-dir）")
    args = parser.parse_args()

    protoDir = Path(args.proto_dir)
    outputRelPath = Path(args.output)
    outputPath = protoDir / outputRelPath
    excludeNames = {outputRelPath.name, "MsgID.proto", "InternalMsgID.proto"}

    currentMsgs = ScanMessages(protoDir, args.subdir, excludeNames)
    existing, deprecated = ParseExisting(outputPath)
    mapping, newDeprecated = AssignIDs(currentMsgs, existing, deprecated)
    WriteMsgID(outputPath, mapping, newDeprecated, args.enum_name, args.subdir)

    activeCount = len(mapping) - len(newDeprecated)
    print(f"[GenMsgID] {activeCount} active, {len(newDeprecated)} deprecated → {outputPath}")


if __name__ == "__main__":
    main()
