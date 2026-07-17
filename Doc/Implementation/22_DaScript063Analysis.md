# 脚本引擎 #22：daScript 0.6.3 版本更新分析

> 状态：**分析完成**（2026-07-15）
> 关联：[09_ScriptEngine](09_ScriptEngine.md)（总纲, DECS架构绑定）、[20_HandlerValidation](20_HandlerValidation.md)（Handler编译期校验）
> DaScript 版本：0.6.2 → **0.6.3**
> 影响：Handler校验完善、Bridge简化、脚本级Profiling、可选替换C++组件

## 1. 总体变化

0.6.2 → 0.6.3 新增了约20个daslib模块。对Massive最关键的新增能力：

| 新增模块 | 功能 | 直接影响 |
|----------|------|---------|
| `interfaces.das` | `[interface]` + `[implements]` ——编译期Completeness检查 | **优化我们的Handler校验方案** |
| `profiler.das` / `profiler_boost.das` | daScript级函数Timing + Chrome Trace输出 | **替代Tracy脚本侧埋点** |
| `toml.das` | 纯daScript TOML解析器 | **可选：脚本侧直接读Config** |
| `soa.das` | `[soa]`宏 —— Structure-of-Arrays自动转换 | **优化DECS Component存储?** |
| `logger.das` | 结构化日志 (ndjson) —— daScript侧原生日志 | 可选：与C++ spdlog并存或替换 |
| `sql.das` | SQL抽象层 (provider pattern) | **Phase 5: 支持PostgreSQL直连** |

## 2. 核心发现：`interfaces.das` 可直接替代 HandlerRegistry

### 2.1 现有方案回顾

我们在 `20_HandlerValidation.md` 中设计了自定义 `[msg_handler]` 注解 + `static_if` + `qmacro` 注入注册代码的方案。虽然功能正确，但实现复杂（约50行自定义宏代码），且需要维护`AstFunctionAnnotation`子类。

### 2.2 0.6.3的 `interfaces.das`

daScript 0.6.3 提供了官方`[interface]` + `[implements]`抽象：

- `[interface]` 注解 —— 声明一个只有函数字段的抽象接口
- `[implements(InterfaceName)]` —— 一个struct声明实现该接口
- **compile-time completeness checking** —— `implements`的`finish()`方法遍历interface的所有抽象方法，检查struct是否全部实现。缺失方法 → 编译错误 + 精确错误信息（第280-306行）

### 2.3 适配原理

把"handler函数集合"建模为 **interface**，`Handlers.das` 中的struct实现它：

```das
// Scripts/AutoGen/HandlerInterface.das —— GenMsgBindings.py 产出
[interface]
class IMessageHandlers {
    // 所有入站消息的handler函数签名由生成器填入:
    handle_move(sessionID : uint32; var args : MoveReqArgs)
    handle_enter_world(sessionID : uint32; var args : EnterWorldReqArgs)
    handle_chat(sessionID : uint32; var args : ChatReqArgs)
    // ← 新增proto → 生成器追加一行
}
```

```das
// Scripts/Handlers.das —— 开发者手写
require AutoGen/HandlerInterface

// struct 声明实现 IMessageHandlers
// 缺少任何方法 → 编译错误: "Handlers does not implement IMessageHandlers.handle_chat"
[implements(IMessageHandlers)]
struct Handlers {
    handle_move(sessionID : uint32; var args : MoveReqArgs) {
        // 业务逻辑
    }
    handle_enter_world(sessionID : uint32; var args : EnterWorldReqArgs) {
        // 业务逻辑
    }
    // handle_chat 忘记写 → finish()检查到缺失 → 编译错误
}
```

**自动化程度**:
1. 新增proto消息 → `GenMsgBindings.py`在`IMessageHandlers`末尾追加一行方法签名
2. xmake build → daScript编译器在`[implements]`的finish()中检查`Handlers`是否包含所有接口方法
3. 缺少handler → 编译错误，精确到缺失方法名

### 2.4 对比

| | 自定义[msg_handler]方案 | [interface]+[implements]方案 |
|---|----------------------|--------------------------|
| 总代码量 | ~50行自定义注解类 | 5行interface声明 + 1行[implements] |
| Completeness检查 | 手动`static_if` + `concept_assert` | daScript编译器内置检查 |
| 错误信息 | "handler数量不匹配: 期望3，实际2" | "Handlers does not implement IMessageHandlers.handle_chat" ← 精确到方法名 |
| msgID→handler dispatch | 仍需自定义table+dispatch逻辑 | 仍需 — interface不解决dispatch |
| daScript版本依赖 | 兼容0.6.2+ | 需要0.6.3+ |

### 2.5 决策

**保留 `[msg_handler]`的自定义注解设计，不替换为 `[interface]+[implements]`。** 理由：

- `[msg_handler]`除了签名校验，还做了 `qmacro` 自动注入 msgID dispatch注册代码——这是interface不能提供的
- interface只能做签名检查，不能做msgID关联+dispatch注册
- 两者可以并存：interface做编译期签名检查，`[msg_handler]`做自动注册

**建议**：保留现有设计，同时在 `20_HandlerValidation.md`中记录interface方案作为备选（如果自定义注解有兼容性问题）。

## 3. 发现2：`profiler.das` 提供 daScript 函数级Timing

### 3.1 功能

profiler.das 在0.6.3已成熟：daScript的`DapiDebugAgent.onInstrumentFunction`钩子 + PerfEvent时间线 → Chrome Trace输出（chrome://tracing可打开）。

**对比Tracy**:

| | Tracy (C++) | profiler.das (Script) |
|---|------------|-------------------|
| 输出格式 | Tracy独有格式 | Chrome Trace JSON |
| 采样粒度 | C++ frame级别 | daScript函数级别 |
| 数据内容 | CPU时间,Zone,Memory | daScript函数耗时+堆内存 |
| 脚本埋点 | 需要手动`massive_profile_begin/end()` | 自动挂钩所有函数 |
| 生产环境 | ✅ Tracy已集成+调优 | ⚠️ 脚本Profiler有10-15% overhead |

### 3.2 适用场景

- **开发/调试期**: profiler.das代替Tracy脚本埋点 → 自动看到每个DECS Stage、每个Handler、每个AI决策的准确耗时
- **生产环境**: Tracy (C++) 为主，profiler.das按需开启（`--das-profiler-log-file`命令行参数）

### 3.3 影响

`09_ScriptEngine.md` §13的Tracy脚本埋点设计可以简化：不手写`massive_profile_begin/end()`，在开发期用profiler.das自动挂钩。

## 4. 发现3：`toml.das` 允许脚本直接读Config

纯daScript TOML解析器，输出JsValue（与json.das兼容）。

**影响**: 脚本侧可以直接 `read_toml("Config/world.toml")` 读配置，不需要通过Bridge函数暴露Config值。

**决策**: **Phase 5考虑**——目前WorldConfig/GateConfig等启动参数在C++侧解析是合理的（C++需要这些参数来初始化网络层）。Phase 5如果需要脚本动态读取配置文件（如Buff模板），可以使用toml.das。

## 5. 其他模块

### 5.1 `soa.das` —— SOA布局宏

`[soa]` 注解将 struct 的所有字段拆成独立的 column array（SoA布局），同时提供 `[]` 索引访问代理。

**适用场景**: 自定义ECS存储——如果DECS因为某种原因不能用，soa.das可以在脚本侧快速搭建简单的SoA存储。

**决策**: **SKIP for now**——DECS已经是更成熟、功能更全的ECS。soa.das仅作为fallback/学习参考。

### 5.2 `sql.das` —— SQL抽象层

Provider pattern（当前SQLite + 未来PostgreSQL）。

**影响**: **Phase 5** —— 如果PostgreSQL provider在daScript层面就绪，可以让我们脚本侧直连DB（绕过C++ DBWorkerPool）。目前SQLite only，PostgreSQL还在路上。

### 5.3 `logger.das` —— daScript原生结构化日志

写入 ndjson 格式日志，支持分级（LOG_INFO/LOG_WARNING/LOG_ERROR）+ 分级过滤 + Category hierarchy。

**对比 `massive_log_info`**:

| | `massive_log_info` (Bridge) | `logger_info` (daScript) |
|---|---------------------------|------------------------|
| 实现 | C++ spdlog | daScript JSON Lines |
| 性能 | 更快（C++ 直接write） | 稍慢（JSON序列化+daScript的FILE） |
| 集成 | 写入C++统一的日志文件 | 独立日志文件 |

**决策**: **保留 `massive_log_info`**作为主要日志（与C++日志一体化），不替换为logger.das。

## 6. 当务之急的行动项

### 优先级P0（Phase 1启动前）

| 行动 | 原因 |
|------|------|
| ✅ 在`daslang.lua`中增加`--das-profiler-log-file`参数支持 | 开发期即获得daScript函数计时 |
| ✅ 生成 `MsgIDConstants.das` + `HandlerInterface.das` 两个生成产物 | GenMsgBindings.py 产出 |

### 优先级P1（Phase 2-3）

| 行动 | 文档更新 |
|------|---------|
| 在`09_ScriptEngine.md` §13备注profiler.das | 简化脚本埋点设计 |
| 在`20_HandlerValidation.md`备注interface方案 | interface作为备选 |

### 优先级P2（Phase 4）

| 行动 | 说明 |
|------|------|
| Phase 4 AI系统性能调优 → 开启 daScript profiler + Chrome Trace | 精确看到每个AI decision的耗时 |
| 评估 tomldas 用于 ConfigTable 脚本绑定 | 如果 C++ ConfigTable 桥接太复杂 |

## 7. 引用

| 模块 | 源码 |
|------|------|
| `interfaces.das` finish() completeness check | `daScript/daslib/interfaces.das:262-306` |
| `profiler.das` PerfEvent/PerfContext | `daScript/daslib/profiler.das:35-57` |
| `toml.das` read_toml() | `daScript/daslib/toml.das` |
| `soa.das` [soa] structure macro | `daScript/daslib/soa.das:1-15` |
| `sql.das` SqlType/ColumnInfo | `daScript/daslib/sql.das:30-44` |
