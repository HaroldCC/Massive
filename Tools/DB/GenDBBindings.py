# GenDBBindings.py — 扫描 DDL 文件，生成 C++ TableSchema + RowType (.gen.h)
#
# 输入: Tools/DB/SQL/*.sql (CREATE TABLE 语句)
# 输出: Src/Common/DB/AutoGen/*.gen.h
#
# 用法:
#   python GenDBBindings.py --sql-dir Tools/DB/SQL --output Src/Common/DB/AutoGen

import argparse
import re
import sys
from pathlib import Path

if sys.platform == "win32":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")


# ── 类型映射 ──

TYPE_MAP = {
    "serial":       "int32_t",
    "bigserial":    "int64_t",
    "integer":      "int32_t",
    "int":          "int32_t",
    "int4":         "int32_t",
    "bigint":       "int64_t",
    "int8":         "int64_t",
    "smallint":     "int16_t",
    "int2":         "int16_t",
    "real":         "float",
    "float4":       "float",
    "double":       "double",
    "double precision": "double",
    "float8":       "double",
    "boolean":      "bool",
    "bool":         "bool",
    "text":         "std::string",
    "varchar":      "std::string",
    "character varying": "std::string",
    "char":         "std::string",
    "timestamp":    "Timestamp",
    "timestamptz":  "Timestamp",
    "timestamp with time zone": "Timestamp",
    "bytea":        "std::vector<uint8_t>",
    "jsonb":        "std::string",
    "json":         "std::string",
    "uuid":         "std::string",
}


def normalize_type(pg_type: str) -> str:
    """规范化 PG 类型（去除数字后缀如 VARCHAR(255) → varchar）"""
    base = pg_type.lower().strip()
    base = re.sub(r"\(.*\)", "", base).strip()
    return base


def cpp_type(pg_type: str) -> str:
    """PG 类型 → C++ 类型"""
    base = normalize_type(pg_type)
    mapped = TYPE_MAP.get(base)
    if mapped:
        return mapped
    for key, val in TYPE_MAP.items():
        if key in base:
            return val
    raise ValueError(f"Unknown PG type: {pg_type}")


# ── DDL 解析 ──

def parse_sql_file(filepath: Path) -> list[dict]:
    """解析 SQL 文件，返回 [{table_name, columns: [{name, pg_type, cpp_type, flags}]}]"""
    text = filepath.read_text(encoding="utf-8")
    tables = []

    # 匹配 CREATE TABLE ... ( ... );
    table_re = re.compile(
        r"CREATE\s+TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?(\w+)\s*\((.*?)\)\s*;",
        re.DOTALL | re.IGNORECASE
    )

    for match in table_re.finditer(text):
        table_name = match.group(1)
        body = match.group(2)

        columns = []
        col_lines = [c.strip() for c in body.split(",") if c.strip()]
        for col_line in col_lines:
            col_line = col_line.strip()
            if re.match(r"^\s*(CONSTRAINT|PRIMARY|FOREIGN|CHECK|UNIQUE)\b", col_line, re.IGNORECASE):
                continue

            # 列定义: col_name TYPE [DEFAULT ...] [NOT NULL] ...
            col_match = re.match(r"(\w+)\s+(.+?)(\s+DEFAULT\s+.+)?$", col_line, re.IGNORECASE)
            if not col_match:
                continue

            col_name = col_match.group(1)
            rest = col_match.group(2).strip()

            # 提取 PG 类型（可能含 double precision 这种多词）
            pg_type = rest.split()[0]
            if pg_type.lower() == "double" and rest.lower().startswith("double precision"):
                pg_type = "double precision"

            # 收集标志位
            flags = []
            rest_upper = rest.upper()
            has_pk = "PRIMARY KEY" in rest_upper
            has_serial = "SERIAL" in rest_upper or "BIGSERIAL" in rest_upper
            has_not_null = "NOT NULL" in rest_upper
            has_default = "DEFAULT" in rest_upper

            if has_pk:
                flags.append("kPK")
            if has_serial:
                flags.append("kAutoInc")
            if has_not_null:
                flags.append("kRequired")
            elif has_default:
                flags.append("kDefaulted")
            if not flags:
                flags.append("kNone")

            columns.append({
                "name": col_name,
                "pg_type": pg_type,
                "cpp_type": cpp_type(pg_type),
                "flags": " | ".join(flags),
            })

        tables.append({"name": table_name, "columns": columns})

    return tables


# ── 代码生成 ──

def snake_to_pascal(name: str) -> str:
    """snake_case → PascalCase"""
    return "".join(part.capitalize() for part in name.split("_"))


def generate_gen_h(table: dict, source_file: str) -> str:
    """生成一个 .gen.h 文件"""
    table_name = table["name"]
    pascal = snake_to_pascal(table_name)
    struct_name = table_name + "_row"
    class_name = pascal + "Table"

    lines = [
        "/**",
        f" * @file {class_name}.gen.h",
        f" * @brief 自动生成 —— PostgreSQL table '{table_name}'",
        " *",
        f" * 来源: {source_file}",
        " * 生成工具: Tools/DB/GenDBBindings.py",
        " * @warning 不要手动编辑",
        " */",
        "#pragma once",
        "",
        '#include "Common/DB/Column.h"',
        '#include "Common/DB/Timestamp.h"',
        "",
        "namespace MMO::DB::AutoGen",
        "{",
        "",
    ]

    # Row 结构体
    lines.append(f"struct {struct_name}")
    lines.append("{")
    for col in table["columns"]:
        default_val = "{}" if col["cpp_type"] == "std::string" else " = 0"
        lines.append(f"    {col['cpp_type']:32s} {col['name']}{default_val};")
    lines.append("};")
    lines.append("")

    # Table 结构体
    pk_type = None
    pk_col_name = None
    for col in table["columns"]:
        if "kPK" in col["flags"]:
            pk_type = col["cpp_type"]
            pk_col_name = col["name"]
            break
    if not pk_col_name:
        pk_col_name = table["columns"][0]["name"]
        pk_type = table["columns"][0]["cpp_type"]

    lines.append(f"struct {class_name}")
    lines.append("{")
    lines.append(f"    static constexpr auto kTableName = \"{table_name}\";")
    lines.append(f"    using RowType = {struct_name};")
    lines.append(f"    using PKType  = {pk_type};")
    lines.append("")

    for col in table["columns"]:
        lines.append(
            f"    static constexpr auto {col['name']:20s} = "
            f"Column<{col['cpp_type']}>{{\"{col['name']}\", {col['flags']}}};"
        )

    lines.append(f"    static constexpr auto PK = {pk_col_name};")
    lines.append("};")
    lines.append("")
    lines.append("} // namespace MMO::DB::AutoGen")
    lines.append("")

    return "\n".join(lines)


# ── 主流程 ──

def main():
    parser = argparse.ArgumentParser(description="DDL → C++ .gen.h 代码生成")
    parser.add_argument("--sql-dir", required=True, help="DDL .sql 目录")
    parser.add_argument("--output", required=True, help=".gen.h 输出目录")
    args = parser.parse_args()

    sql_dir = Path(args.sql_dir)
    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)

    for sql_file in sorted(sql_dir.glob("*.sql")):
        source = sql_file.name
        tables = parse_sql_file(sql_file)
        for table in tables:
            content = generate_gen_h(table, source)
            pascal = snake_to_pascal(table["name"])
            out_path = output_dir / f"{pascal}Table.gen.h"
            if out_path.exists() and out_path.read_text(encoding="utf-8") == content:
                continue
            out_path.write_text(content, encoding="utf-8")
            print(f"[GenDB] {sql_file.name}:{table['name']} → {out_path.name}")


if __name__ == "__main__":
    main()
