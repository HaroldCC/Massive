# 生成器修复 + 多服务参数化执行文档（ScriptLayer_05）

> 面向 `Tools/Script/GenMsgBindings.py`。两块内容：
> **A. 6 项审查修复**（对照 daScript `integration_cpp_*.rst` 教程，全部对源码核实）；
> **B. 多服务参数化**（World/Social 共用一套生成/AOT 机制）。
> 所有改动在生成器内完成，无需手改已生成文件。可照抄执行。

---

## 优先级

| # | 项 | 严重度 | 后果 |
|---|---|---|---|
| A1 | 注解注册顺序 topo 排序 | 🔴 崩溃 | 同/跨文件前向引用 → `DAS_FATAL_ERROR` 进程崩 |
| A2 | bytes 字段截断 | 🔴 数据错 | DH key/token 含 0x00 被 strlen 截断 |
| A3 | repeated 索引越界 | 🔴 UB | release 越界读；脚本可触发 |
| A4 | string getter 生命周期 | 🟡 隐患 | 脚本存指针 → 悬垂读 |
| A5 | canCopy override 冗余 | 🟡 清理 | 无害，删或注释 |
| A6 | 无 typeFactory<EMsgID> | 🟡 latent | 当前 OK，将来 addExtern 用 EMsgID 会编译错 |
| B | 多服务参数化 | 功能 | Social 复用生成/AOT |

先做 A1→A2→A3（都影响正确性），再 B，最后 A4-A6。

---

## A. 审查修复

### A1. 注解注册按字段依赖 topo 排序（🔴 会崩）

**问题**：教程 `integration_cpp_04_binding_types.rst:117-124`「B 含 A 字段则先注册 A」。生成器按 `.proto` 声明顺序 emit/register（`GenMsgBindings.py:510-511,580-581`）。同文件内若 msg M 有个 singular 字段类型是**后声明**的 msg I，`new MAnnotation` 在 I 注册前跑 → 注解 ctor 里 `addProperty` 立即解析 `const I&` → `makeHandleType` 找不到注解 → `DAS_FATAL_ERROR`（`ast_module.cpp:1194`）进程崩。跨文件 `compute_closure` 的「被引用次数降序」（line 279）也非真 topo（A→B→C 反例）。repeated message 索引访问器（`addExtern const T&`, line 522）同样 eager 解析，一并受影响。

**修**：文件内 + 跨文件都按「singular/repeated message 字段依赖」做真 topo 排序。

**① 文件内消息 topo 排序** —— 新增函数，在发射前重排 `info.messages`：

```python
def _topo_sort_messages(messages: list[MessageInfo]) -> list[MessageInfo]:
    """按消息间字段依赖 topo 排序：被依赖者在前。
    依赖 = 某消息的字段（singular 或 repeated）类型是同文件内另一消息。
    循环依赖时退回原顺序（循环无法用 ctor 内 addProperty 满足，另见备注）。"""
    by_name = {m.name: m for m in messages}
    visited: dict[str, int] = {}   # 0=访问中, 1=完成
    ordered: list[MessageInfo] = []
    has_cycle = [False]

    def visit(m: MessageInfo):
        state = visited.get(m.name)
        if state == 1:
            return
        if state == 0:
            has_cycle[0] = True     # 检测到环
            return
        visited[m.name] = 0
        for fld in m.fields:
            if fld.is_scalar or fld.is_string or fld.is_bytes or fld.is_map:
                continue
            dep = by_name.get(fld.type_name)   # 仅同文件内依赖
            if dep is not None and dep is not m:
                visit(dep)
        visited[m.name] = 1
        ordered.append(m)

    for m in messages:
        visit(m)
    if has_cycle[0]:
        # 循环引用：ctor 内 addProperty 无法满足任何顺序——退回原序并告警
        print(f"警告：检测到消息循环引用，注解注册顺序可能触发 DAS_FATAL_ERROR；"
              f"如确有循环字段，请改为注解外后置 addExternProperty 绑定该字段",
              file=sys.stderr)
        return messages
    return ordered
```

在 `generate_proto_gen_cpp` 里，emit 前重排一次（emit 与 register 都用这个顺序）：

```python
def generate_proto_gen_cpp(info: ProtoFileInfo, proto_files: dict[str, ProtoFileInfo]) -> str:
    # ★ A1: 文件内按字段依赖 topo 排序，保证被依赖的消息注解先注册
    info.messages = _topo_sort_messages(info.messages)
    # ... 后续 emit_annotation / emit_register_bindings 用重排后的 info.messages ...
```

> 因为 `emit_annotation`、`emit_register_bindings`、dispatch 都遍历 `info.messages`，一次重排全部受益。

**② 跨文件 topo 排序** —— `compute_closure` 的返回顺序改为真 topo（被依赖文件在前），替换现有「ref_count 降序」（line 279 附近）：

```python
def compute_closure(proto_files: dict[str, ProtoFileInfo]) -> list[ProtoFileInfo]:
    """含 *Req 的文件 + 其（递归）引用类型所在文件；按【文件间依赖】topo 排序（被依赖者在前）。"""
    req_files: set[str] = {f for f, i in proto_files.items() if i.has_req}

    type_to_file: dict[str, str] = {}
    for fname, info in proto_files.items():
        for msg in info.messages:
            type_to_file[msg.name] = fname

    # 收集闭包
    closure: set[str] = set(req_files)
    queue = deque(req_files)
    while queue:
        cur = queue.popleft()
        for msg in proto_files[cur].messages:
            for fld in msg.fields:
                if fld.is_scalar or fld.is_string or fld.is_bytes or fld.is_map:
                    continue
                ref = type_to_file.get(fld.type_name)
                if ref and ref not in closure:
                    closure.add(ref)
                    queue.append(ref)

    # 文件间依赖边：file X 依赖 file Y（X 的某消息字段类型定义在 Y，且 Y != X）
    def file_deps(fname: str) -> set[str]:
        deps: set[str] = set()
        for msg in proto_files[fname].messages:
            for fld in msg.fields:
                if fld.is_scalar or fld.is_string or fld.is_bytes or fld.is_map:
                    continue
                ref = type_to_file.get(fld.type_name)
                if ref and ref in closure and ref != fname:
                    deps.add(ref)
        return deps

    visited: dict[str, int] = {}
    ordered: list[str] = []

    def visit(fname: str):
        st = visited.get(fname)
        if st == 1:
            return
        if st == 0:
            return   # 文件级环：极少见，忽略（消息级 topo 已处理大部分）
        visited[fname] = 0
        for dep in sorted(file_deps(fname)):   # sorted 保证确定性
            visit(dep)
        visited[fname] = 1
        ordered.append(fname)

    for fname in sorted(closure):
        visit(fname)
    return [proto_files[f] for f in ordered]
```

> `ProtoBindIndex.gen.cpp` 的 `RegisterAllProtoMessageTypes` 按 closure 顺序调各 `RegisterXxxProtoBindings`（生成器 line 639-642），topo 后「被依赖文件先注册」成立。

**循环引用备注**：protobuf 允许 msg 互相引用（A 有 B 字段、B 有 A 字段）。这种循环**无法**用「注解 ctor 内 addProperty」的任何顺序满足——两个注解都在对方注册前需要对方。若将来真出现，那些字段要改成**注解外后置绑定**（所有 annotation 都 `addAnnotation` 之后，再用自由函数 `addExternProperty` 补绑）。当前 4 个消息无循环，`_topo_sort_messages` 检测到环会告警并退回原序，不静默。

---

### A2. bytes 字段用长度携带表示，不走 string（🔴 数据错）

**问题**：`emit_string_accessor`（`GenMsgBindings.py:391-392`）对 bytes 返回 `reinterpret_cast<const char*>(...data())` → daScript 当 NUL 结尾 string，strlen 取长（`hash.h:11`）。二进制含 0x00 被截断。`string` 字段（真 proto string）走 `.c_str()` 是对的。

**修**：拆开 string 与 bytes 两条路径。string 保持 `.c_str()`；**bytes 改为 `_size` + `(msg,index)->uint8` 索引访问**（复用已有 repeated 模式的形状），脚本侧按长度逐字节读，0x00 不丢。

改 `_iter_string_fields`（只留真 string）+ 新增 bytes 处理。在字段分类处区分：

```python
def _iter_plain_string_fields(msg: MessageInfo):
    """非 repeated 的 string 字段（真文本，可安全 c_str）"""
    for fld in msg.fields:
        if fld.is_map or fld.label == "repeated":
            continue
        if fld.is_string:      # 仅 string，不含 bytes
            yield fld

def _iter_bytes_fields(msg: MessageInfo):
    """非 repeated 的 bytes 字段（二进制，须按长度逐字节暴露）"""
    for fld in msg.fields:
        if fld.is_map or fld.label == "repeated":
            continue
        if fld.is_bytes:
            yield fld
```

bytes 访问器发射（`_size` 属性 + `(msg,index)->uint8`）：

```python
def _bytes_size_name(msg: str, fld: FieldInfo) -> str:
    return f"{msg}_{cap_first(fld.name)}_Size"

def _bytes_at_name(msg: str, fld: FieldInfo) -> str:
    return f"{msg}_{cap_first(fld.name)}_At"

def emit_bytes_accessors(msg: MessageInfo, fld: FieldInfo) -> list[str]:
    size_func = _bytes_size_name(msg.name, fld)
    at_func   = _bytes_at_name(msg.name, fld)
    return [
        f"    // ── bytes 字段：长度 + 逐字节 uint8，避免 NUL 截断 ──",
        f"    int {size_func}(const {PROTO_NS}::{msg.name} &msg)",
        "    {",
        f"        return static_cast<int>(msg.{fld.name}().size());",
        "    }",
        "",
        f"    uint8_t {at_func}(const {PROTO_NS}::{msg.name} &msg, int index)",
        "    {",
        f"        const auto &b = msg.{fld.name}();",
        "        if (index < 0 || index >= static_cast<int>(b.size())) { return 0u; }",  # A3 同款守卫
        f"        return static_cast<uint8_t>(b[static_cast<size_t>(index)]);",
        "    }",
        "",
    ]
```

注册（`.\`fieldname_size` 属性 + `fieldname` 自由函数带 index）：

```python
# emit_register_bindings 里，对每个 bytes 字段：
for fld in _iter_bytes_fields(msg):
    size_func = _bytes_size_name(msg.name, fld)
    at_func   = _bytes_at_name(msg.name, fld)
    out.append(f'        das::addExternProperty<DAS_BIND_FUN({size_func})>(mod, lib, ".`{fld.name}_size", "{size_func}")')
    out.append('            ->args({"msg"});')
    out.append(f'        das::addExtern<DAS_BIND_FUN({at_func})>(mod, lib, "{fld.name}", das::SideEffects::none)')
    out.append('            ->args({"msg", "index"});')
```

**脚本侧读法**（文档给业务用）：`for (i in range(req.session_token_size)) { let b = session_token(req, i); ... }`。若业务需要整块 `array<uint8>`，可在 das 侧写个 helper 循环拷进 `array<uint8>` 再 `:=` 保存。

> repeated **bytes** 字段（`emit_repeated_accessors` 里 `fld.is_bytes` 分支，line 408-409）同样有此问题。若你的 proto 有 `repeated bytes`（当前没有），一并改成返回 size+逐字节；当前 4 个消息无 repeated bytes，可暂留 TODO 注释。

---

### A3. repeated 索引加边界检查（🔴 UB）

**问题**：`emit_repeated_accessors`（`GenMsgBindings.py:423-426`）`return msg.field(index)` 直传 protobuf，无守卫。本 build protobuf `BoundsCheckMode=kNoEnforcement`（`port.h:854`）→ release 越界读 UB。daScript 原生 `arr[i]` 会抛可捕获错误（`runtime_array.h:21`）。

**修**：索引访问器加守卫，越界返回默认值（与 A2 bytes 访问器同款）。

```python
def emit_repeated_accessors(msg: MessageInfo, fld: FieldInfo) -> list[str]:
    size_func  = _repeated_size_name(msg.name, fld)
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
        access   = f"msg.{fld.name}(index)"
        default  = f"{PROTO_NS}::{fld.type_name}::default_instance()"
    return [
        "    // ── repeated _size：addExternProperty，无括号 ──",
        f"    int {size_func}(const {PROTO_NS}::{msg.name} &msg)",
        "    {",
        f"        return msg.{fld.name}_size();",
        "    }",
        "",
        "    // ── repeated 索引访问：带边界检查（protobuf 本 build 不做越界防护）──",
        f"    {ret_type}{index_func}(const {PROTO_NS}::{msg.name} &msg, int index)",
        "    {",
        f"        if (index < 0 || index >= msg.{fld.name}_size()) {{ return {default}; }}",
        f"        return {access};",
        "    }",
        "",
    ]
```

> `default_instance()` 是 protobuf 每个 message 都有的静态方法，返回 `const T&`，永久有效，越界返回它安全。scalar 返回 0，string 返回 `""`，bytes 返回 `nullptr`（脚本侧 `_size` 已能先判空）。

---

### A4. string getter 生命周期（🟡 文档 + 可选加固）

**问题**：string getter 返回 `const char*` 指向栈上 `req` 内部，Dispatch 返回后 `req` 析构。脚本用 `=`（拷指针）存起来 → 悬垂读。`canCopy/canClone=false` 只护 message handle。

**两个处理层次，按需选**：

**(最小) 文档 + 修正误导注释**：在生成文件头注释明确「从 req 抽出的 string/bytes 仅在本次 dispatch 调用内有效，不得用 `=` 存到全局/结构体；需保留请 `:=` 克隆」。并修 `emit_file_header` 里那句会误导的注释（现说 req 不能逃逸——对 handle 真、对抽出 string 假）：

```python
def emit_file_header(info: ProtoFileInfo) -> list[str]:
    # ...
    if req_names:
        out.append(f" * @note {', '.join(req_names)} 覆写 canClone=false——req 句柄不能存进全局/结构体。")
        out.append(" *       ⚠ 但【从 req 抽出的 string】是指向 req 内部的临时指针，")
        out.append(" *       仅本次 dispatch 调用内有效；脚本如需保留必须 := 克隆，不能 = 存储。")
    # ...
```

**(彻底，可选) 堆分配返回**：把 string getter 改成 interop 形式（`context.allocateString`，见 `integration_cpp_06_interop.rst:86`），返回克隆进脚本堆的 string，脚本存了也安全。代价：getter 要绑成带 `Context&` 的 interop 函数，生成复杂度上升。**建议先走最小方案**（文档 + 注释），除非确认业务会跨调用保留字段。

---

### A5. 删冗余 canCopy override（🟡 清理）

**问题**：protobuf 非平凡拷贝 → `canCopy/canMove` 默认已 false（`ast.h:525` + `ast_handle.h:267`）；只有 `canClone` 默认 true 需 override。`canCopy` override 无害但多余。

**修**：`emit_annotation` 的 `is_req` 分支只留 `canClone`：

```python
    if is_req:
        out += [
            "",
            "        // §2.4 生命周期防护——阻断脚本 clone req 存进逃逸本次调用的存储位置",
            "        // （canCopy/canMove 因 protobuf 非平凡拷贝已默认 false，无需 override）",
            "        virtual bool canClone() const override { return false; }",
        ]
```

同时修 A1 提到的 Rsp 注释 nit（现说「可安全复制」不准——canCopy 也 false，只能 clone）：`emit_annotation` 里 value 类型的 `@note` 改为「可 clone(:=)/存储，不可 = 复制」。

---

### A6. typeFactory<EMsgID>（🟡 latent，加注释即可）

**问题**：只 emit `DAS_BIND_ENUM_CAST`（仅 `cast<>`，非 typeFactory）。当前 OK——dispatch 把 msgID 转 `uint32` 从不以 `EMsgID` 类型跨界。但若将来 `addExtern` 参数/返回是 `EMsgID` 会编译错（`ToBasicType` static_assert）。

**修**：不改行为，在 `generate_index_cpp` 的 EMsgID 绑定处加注释说明假设：

```python
        "    // 注：仅 addEnumeration + DAS_BIND_ENUM_CAST，未 emit typeFactory<EMsgID>。",
        "    // 因为 msgID 始终以 uint32 跨 C++/脚本边界（见 Dispatch），从不以 EMsgID 类型传参。",
        "    // 若将来有 addExtern 的参数/返回类型是 MMO::Proto::EMsgID，需改用 DAS_BASE_BIND_ENUM_FACTORY。",
```

---

## B. 多服务参数化（World / Social 共用）

### B0. 认识：运行期已自动，只需参数化构建期

「按服务导入不同模块」运行期天然成立——World 进程链接 `WorldDasModule`（`IDasLangModuleProvider`），Social 链接 `SocialDasModule`，各自 `CreateModules` 只建自己的模块；das 侧各跑各自 `main.das`。引擎不需 `if(isWorld)`。

要参数化的是**构建期**：代码生成 + AOT 入口，目前硬编码在 `Src/World/xmake.lua`。

### B1. proto 按服务分子目录

```
Src/Proto/
  Common.proto          # 公共消息（Vector3/ErrorInfo，两服务都要）
  MsgID.proto           # 全量 EMsgID（保持单一枚举，两服务共用值空间）
  World/                # World 专用消息（Move/Login/Replicate...）
  Social/               # Social 专用消息（好友/公会/聊天...）
```

> `MsgID.proto` 保持单文件全量枚举——消息 ID 是全局唯一值空间，World/Social 各取自己那段。生成器对每个服务只绑定该服务用到的消息类型，但 `EMsgID` 枚举可全量绑（多余的值无害）或按服务过滤（更干净）。建议**全量绑**，简单。

### B2. 生成器加 `--service`

```python
# main() 里
parser.add_argument("--service", required=True, help="服务名：world / social")

# 扫 proto：Common（公共） + <Service>/（专用）
def collect_service_protos(proto_dir: Path, service: str) -> dict[str, ProtoFileInfo]:
    proto_files: dict[str, ProtoFileInfo] = {}
    exclude = {"MsgID.proto"}
    # 公共
    for p in sorted(proto_dir.glob("*.proto")):
        if p.name not in exclude:
            info = parse_proto_file(p)
            proto_files[info.filename] = info
    # 服务专用
    svc_dir = proto_dir / service.capitalize()   # World / Social
    if svc_dir.is_dir():
        for p in sorted(svc_dir.glob("*.proto")):
            info = parse_proto_file(p)
            proto_files[info.filename] = info
    return proto_files
```

`main()` 用 `collect_service_protos(proto_dir, args.service)` 替换原来的平铺扫描；其余（闭包、生成）不变。输出目录仍由 `--cpp-out` 指定（各服务传自己的 `Src/<Service>/AutoGen`）。

### B3. 共享 xmake rule（带 service 参数）

抽到 `Src/xmake_rules.lua`（或放 `Src/ScriptEngine/`）供各服务 include：

```lua
-- 共享：按服务生成消息绑定
rule("gen_msg_bindings")
    on_load(function (target)
        local service = target:extraconf("rules", "gen_msg_bindings", "service")
        assert(service, "gen_msg_bindings 需指定 service：add_rules(..., {service=\"world\"})")
        local protoDir   = path.join(os.projectdir(), "Src/Proto")
        local autogenDir = path.join(target:scriptdir(), "AutoGen")
        local genScript  = path.join(os.projectdir(), "Tools/Script/GenMsgBindings.py")

        os.vrunv("python", { genScript, "--service", service,
                             "--proto-dir", protoDir, "--cpp-out", autogenDir })
        for _, f in ipairs(os.files(path.join(autogenDir, "*.gen.cpp"))) do
            target:add("files", f)
        end
    end)
```

各服务：

```lua
-- Src/World/xmake.lua
target("WorldServer")
    add_rules("gen_msg_bindings", { service = "world" })

-- Src/Social/xmake.lua（将来）
target("SocialServer")
    add_rules("gen_msg_bindings", { service = "social" })
```

### B4. AOT 入口也按服务（呼应 04b）

`das_aot` rule 的入口列表同样从服务参数派生，不写死：

```lua
-- 每服务的 AOT 入口 = 该服务的 main.das（其 require 闭包自动覆盖 Handlers/MsgHandlerRegistry）
rule("das_aot")
    on_load(function (target)
        target:data_set("aot_entry", target:extraconf("rules", "das_aot", "entry"))
    end)
    before_build(function (target)
        -- ... 用 target:data("aot_entry") 作为唯一入口脚本 ...
    end)

target("WorldServer")
    if is_mode("release") then
        add_rules("das_aot", { entry = "World/main.das" })
    end
```

> 「服务名分片」贯穿：**proto 子目录 / 生成绑定 / AOT 入口 / provider / 脚本目录**。加 Social 时只需：建 `Src/Proto/Social/*.proto` + `Src/Social/{AutoGen,DasModule,xmake}` + `Script/Social/main.das`，rule 全复用。

---

## 落地顺序与验证

1. **A1**（topo）→ 生成器加 `_topo_sort_messages` + 改 `compute_closure`。验证：造一个「后声明 msg 被前面 msg 引用」的 proto，生成后注册顺序应被依赖者在前。
2. **A2**（bytes）+ **A3**（越界）→ 改字段分类与访问器发射。验证：生成的 bytes 字段有 `_size` + `_At`，repeated 索引有守卫。
3. **A4/A5/A6** → 注释与 canClone-only。
4. **B** → `--service` + proto 子目录 + 共享 rule。验证：`python GenMsgBindings.py --service world --proto-dir Src/Proto --cpp-out /tmp/w` 只生成 World+Common 的绑定。
5. 全量重生成 `Src/World/AutoGen`，编译 WorldServer（需先完成 R5 的引擎接线：DispatchRegistry 归属、DECLARE/PULL 等）。

> 每步生成后先 `python -c "import ast; ast.parse(...)"` 验 py 语法，再对真实 proto 跑一遍看输出 diff，最后编译。
