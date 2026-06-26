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


def generate_deserialize_method(columns: list) -> str:
    """生成 DeserializeRow 方法——按列名匹配反序列化"""
    lines = [
        "    /**",
        "     * @brief 按列名匹配反序列化一行，不依赖 SELECT 列顺序",
        "     * @param res      DBResult",
        "     * @param rowIdx   行索引",
        "     */",
        "    static RowType DeserializeRow(const DBResult &res, int rowIdx)",
        "    {",
        "        RowType row;",
        "        auto &cols = res.Columns();",
        "        for (int c = 0; c < res.ColCount(); ++c)",
        "        {",
        "            (void)cols;",
    ]
    for col in columns:
        name = col["name"]
        ct = col["cpp_type"]
        var = "row." + name
        getter = f"res.Get(rowIdx, c)"

        if ct == "Timestamp":
            lines.append(f'            if (cols[c] == "{name}")')
            lines.append("            {")
            lines.append(f"                if (!{getter}.IsNull())")
            lines.append(f"                    {var} = Timestamp::FromPGText({getter}.Text());")
            lines.append("            }")
        elif ct == "int32_t":
            lines.append(f'            if (cols[c] == "{name}")    {{ {var} = {getter}.AsInt32(); }}')
        elif ct == "int64_t":
            lines.append(f'            if (cols[c] == "{name}")    {{ {var} = {getter}.AsInt64(); }}')
        elif ct == "int16_t":
            lines.append(f'            if (cols[c] == "{name}")    {{ {var} = static_cast<int16_t>({getter}.AsInt32()); }}')
        elif ct == "float":
            lines.append(f'            if (cols[c] == "{name}")    {{ {var} = {getter}.AsFloat(); }}')
        elif ct == "double":
            lines.append(f'            if (cols[c] == "{name}")    {{ {var} = {getter}.AsDouble(); }}')
        elif ct == "bool":
            lines.append(f'            if (cols[c] == "{name}")    {{ {var} = {getter}.AsInt32() != 0; }}')
        elif ct == "std::string":
            lines.append(f'            if (cols[c] == "{name}")    {{ {var} = {getter}.Text(); }}')
        elif ct == "std::vector<uint8_t>":
            lines.append(f'            if (cols[c] == "{name}")    {{ /* bytea: {var} = {getter}.Text(); */ }}')
        else:
            lines.append(f'            if (cols[c] == "{name}")    {{ /* unhandled type {ct} */ }}')

    lines.append("        }")
    lines.append("        return row;")
    lines.append("    }")
    return "\n".join(lines)


def generate_serialize_insert(columns: list, table_name: str) -> str:
    """生成 SerializeInsert 方法——INSERT SQL + 参数"""
    insert_cols = [c for c in columns if "kAutoInc" not in c["flags"]]

    def push_param(col):
        ct = col["cpp_type"]
        name = col["name"]
        nullable = "kNullable" in col["flags"] or ("kDefaulted" in col["flags"] and "kRequired" not in col["flags"])
        if ct == "int32_t":
            v = f"row.{name} == 0 ? DBValue(nullptr) : DBValue(static_cast<int32_t>(row.{name}))" if nullable else f"static_cast<int32_t>(row.{name})"
        elif ct == "int64_t":
            v = f"row.{name} == 0 ? DBValue(nullptr) : DBValue(static_cast<int64_t>(row.{name}))" if nullable else f"static_cast<int64_t>(row.{name})"
        elif ct == "int16_t":
            v = f"row.{name} == 0 ? DBValue(nullptr) : DBValue(static_cast<int32_t>(row.{name}))" if nullable else f"static_cast<int32_t>(row.{name})"
        elif ct == "float":
            v = f"row.{name} == 0.0f ? DBValue(nullptr) : DBValue(static_cast<double>(row.{name}))" if nullable else f"static_cast<double>(row.{name})"
        elif ct == "double":
            v = f"row.{name} == 0.0 ? DBValue(nullptr) : DBValue(row.{name})" if nullable else f"row.{name}"
        elif ct == "bool":
            v = f"static_cast<int32_t>(row.{name} ? 1 : 0)"
        elif ct == "std::string":
            v = f"row.{name}.empty() ? DBValue(nullptr) : DBValue(row.{name})" if nullable else f"row.{name}"
        elif ct == "Timestamp":
            v = f"row.{name}.unix_ms == 0 ? DBValue(nullptr) : DBValue(static_cast<int64_t>(row.{name}.unix_ms))" if nullable else f"static_cast<int64_t>(row.{name}.unix_ms)"
        else:
            v = "row." + name
        return f"        params.emplace_back({v});"

    head = [
        "    /**",
        "     * @brief 构建 INSERT SQL + 参数（自动跳过自增列）",
        "     * @param row     要插入的行",
        "     * @return (SQL, 参数列表)",
        "     */",
        "    static std::pair<std::string, std::vector<DBValue>> SerializeInsert(const RowType &row)",
        "    {",
        "        std::vector<DBValue> params;",
        "        std::string sql;",
        "",
    ]

    col_names_lines = []
    param_lines = []
    for col in insert_cols:
        col_names_lines.append(f"        // {col['name']}")
        param_lines.append(push_param(col))

    # Build SQL column list
    sql_cols = ", ".join(c["name"] for c in insert_cols)

    # Build SQL placeholder list (with to_timestamp for Timestamp)
    parts = []
    for col in insert_cols:
        idx = insert_cols.index(col) + 1
        if col["cpp_type"] == "Timestamp":
            parts.append(f"to_timestamp(${idx}::bigint / 1000.0)")
        else:
            parts.append(f"${idx}")
    sql_placeholders = ", ".join(parts)

    sql_lines = [
        f'        sql = "INSERT INTO {table_name}(";',
        f'        sql += "{sql_cols}";',
        f'        sql += ") VALUES({sql_placeholders})";',
        "",
        "        return {std::move(sql), std::move(params)};",
        "    }",
    ]

    lines = head + col_names_lines + param_lines + [""] + sql_lines
    return "\n".join(lines)


def generate_serialize_update_pk(columns: list, table_name: str) -> str:
    """生成 SerializeUpdateByPK 方法——按 PK 更新"""
    # 找 PK 列
    pk_col = None
    non_pk_cols = []
    for col in columns:
        if "kPK" in col["flags"]:
            pk_col = col
        else:
            non_pk_cols.append(col)
    if not pk_col:
        pk_col = columns[0]

    lines = [
        "    /**",
        "     * @brief 构建 UPDATE BY PK SQL + 参数",
        "     * @param row     要更新的行",
        "     * @return (SQL, 参数列表)",
        "     */",
        "    static std::pair<std::string, std::vector<DBValue>> SerializeUpdateByPK(const RowType &row)",
        "    {",
        "        std::vector<DBValue> params;",
        "        std::string sql = \"UPDATE " + table_name + " SET \";",
        "",
    ]

    param_idx = 0
    for col in non_pk_cols:
        param_idx += 1
        ct = col["cpp_type"]
        name = col["name"]
        sep = ", " if param_idx > 1 else ""

        nullable = "kNullable" in col["flags"] or ("kDefaulted" in col["flags"] and "kRequired" not in col["flags"])

        if ct == "Timestamp":
            lines.append(f'        sql += "{sep}{name} = to_timestamp(${param_idx}::bigint / 1000.0)";')
        else:
            lines.append(f'        sql += "{sep}{name} = ${param_idx}";')

        # Append param
        if ct == "int32_t":
            if nullable:
                lines.append(f'        params.emplace_back(row.{name} == 0 ? DBValue(nullptr) : DBValue(static_cast<int32_t>(row.{name})));')
            else:
                lines.append(f'        params.emplace_back(static_cast<int32_t>(row.{name}));')
        elif ct == "int64_t":
            if nullable:
                lines.append(f'        params.emplace_back(row.{name} == 0 ? DBValue(nullptr) : DBValue(static_cast<int64_t>(row.{name})));')
            else:
                lines.append(f'        params.emplace_back(static_cast<int64_t>(row.{name}));')
        elif ct == "int16_t":
            if nullable:
                lines.append(f'        params.emplace_back(row.{name} == 0 ? DBValue(nullptr) : DBValue(static_cast<int32_t>(row.{name})));')
            else:
                lines.append(f'        params.emplace_back(static_cast<int32_t>(row.{name}));')
        elif ct == "float":
            if nullable:
                lines.append(f'        params.emplace_back(row.{name} == 0.0f ? DBValue(nullptr) : DBValue(static_cast<double>(row.{name})));')
            else:
                lines.append(f'        params.emplace_back(static_cast<double>(row.{name}));')
        elif ct == "double":
            lines.append(f'        params.emplace_back(row.{name});')
        elif ct == "bool":
            lines.append(f'        params.emplace_back(static_cast<int32_t>(row.{name} ? 1 : 0));')
        elif ct == "std::string":
            if nullable:
                lines.append(f'        params.emplace_back(row.{name}.empty() ? DBValue(nullptr) : DBValue(row.{name}));')
            else:
                lines.append(f'        params.emplace_back(row.{name});')
        elif ct == "Timestamp":
            if nullable:
                lines.append(f'        params.emplace_back(row.{name}.unix_ms == 0 ? DBValue(nullptr) : DBValue(static_cast<int64_t>(row.{name}.unix_ms)));')
            else:
                lines.append(f'        params.emplace_back(static_cast<int64_t>(row.{name}.unix_ms));')

    # WHERE PK
    param_idx += 1
    pk_name = pk_col["name"]
    pk_ct = pk_col["cpp_type"]
    lines.append("")
    lines.append(f'        sql += " WHERE {pk_name} = ${param_idx}";')
    if pk_ct == "int32_t":
        lines.append(f'        params.emplace_back(static_cast<int32_t>(row.{pk_name}));')
    elif pk_ct == "int64_t":
        lines.append(f'        params.emplace_back(static_cast<int64_t>(row.{pk_name}));')
    elif pk_ct == "std::string":
        lines.append(f'        params.emplace_back(row.{pk_name});')
    else:
        lines.append(f'        params.emplace_back(static_cast<int32_t>(row.{pk_name}));')

    lines.append("")
    lines.append("        return {std::move(sql), std::move(params)};")
    lines.append("    }")
    return "\n".join(lines)


def generate_gen_h(table: dict, source_file: str) -> str:
    """生成一个 .gen.h 文件"""
    table_name = table["name"]
    pascal = snake_to_pascal(table_name)
    struct_name = table_name + "_row"
    class_name = pascal + "Table"
    columns = table["columns"]

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
        '#include <utility>',
        '#include "Common/DB/Column.h"',
        '#include "Common/DB/Timestamp.h"',
        '#include "Common/DB/Types.h"',
        "",
        "namespace MMO::DB::AutoGen",
        "{",
        "",
    ]

    # Row 结构体
    lines.append(f"struct {struct_name}")
    lines.append("{")
    for col in columns:
        ct = col["cpp_type"]
        if ct == "std::string":
            default_val = "{}"
        elif ct == "std::vector<uint8_t>":
            default_val = "{}"
        elif ct == "bool":
            default_val = " = false"
        elif ct == "Timestamp":
            default_val = " = {}"
        else:
            default_val = " = 0"
        lines.append(f"    {ct:32s} {col['name']}{default_val};")
    lines.append("};")
    lines.append("")

    # Table 结构体
    pk_type = None
    pk_col_name = None
    for col in columns:
        if "kPK" in col["flags"]:
            pk_type = col["cpp_type"]
            pk_col_name = col["name"]
            break
    if not pk_col_name:
        pk_col_name = columns[0]["name"]
        pk_type = columns[0]["cpp_type"]

    lines.append(f"struct {class_name}")
    lines.append("{")
    lines.append(f"    static constexpr auto kTableName = \"{table_name}\";")
    lines.append(f"    using RowType = {struct_name};")
    lines.append(f"    using PKType  = {pk_type};")
    lines.append("")

    for col in columns:
        lines.append(
            f"    static constexpr auto {col['name']:20s} = "
            f"Column<{col['cpp_type']}>{{\"{col['name']}\", {col['flags']}}};"
        )

    lines.append(f"    static constexpr auto PK = {pk_col_name};")
    lines.append("")
    lines.append(generate_deserialize_method(columns))
    lines.append("")
    lines.append(generate_serialize_insert(columns, table_name))
    lines.append("")
    lines.append(generate_serialize_update_pk(columns, table_name))
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
