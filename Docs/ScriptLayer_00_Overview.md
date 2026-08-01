# 脚本层总览（ScriptLayer_00_Overview）

> 本文是 Massive 脚本层重构的执行文档总纲。系列共 5 篇，本篇给出全局蓝图、
> 目录结构、组件职责与依赖关系；后续 4 篇给出可照抄的实现。
>
> - `ScriptLayer_00_Overview.md`（本篇）— 蓝图、目录、分层、发布形态
> - `ScriptLayer_01_MsgBinding.md` — 消息绑定：生成器 + 手写 `[msg_handler]` 宏 + `main.das` 入口 + dispatch 全链路
> - `ScriptLayer_02_HotReload.md` — 热重载：引擎接线、`DoSwap`、文件监视、安全点
> - `ScriptLayer_03_Dabin.md` — `.dabin` 二进制缓存/补丁：序列化 + 可选 AES 加密
> - `ScriptLayer_04_AOT.md` — AOT：daslang 工具链、xmake 生成规则、`aotRequire`

---

## 1. 目标与边界

### 1.1 本轮要完成的

把 `Src/ScriptEngine/` 从「能编译源码 + 建 Context」的骨架，补全为一个**可交付的脚本层底层**，具备三项正交能力：

| 能力 | 解决的问题 | 形态 |
|---|---|---|
| **热重载** | 开发期改脚本无需重启服务器 | 文件监视 → 重编译 → tick 之间换 Context |
| **.dabin 二进制缓存/补丁** | 发布期免去启动编译开销；线上无明文脚本、可打热补丁 | 编译后 AST 序列化成二进制 blob（可选 AES 加密） |
| **AOT** | 发布期脚本以接近原生速度运行 | 构建期 `.das → .cpp` 编入宿主，运行期按语义哈希链接 |

三者**正交**：缓存解决「启动编译慢」、AOT 解决「运行慢」、热重载解决「开发迭代慢」。发布形态可叠加（AOT 基线全速 + 完整编译器保留 + 热补丁能力）。

### 1.2 本轮**不**做的

- **World / Social 具体业务模块**：这些是后续按业务功能开发时再拼装的上层。本轮只把**底层组件**设计好，让业务层是「拼装 + 封装」而非「从零搭地基」。
- 因此 `WorldModule` / `SocialModule` 的字段与绑定函数**不实现**，只在文档里约定它们如何接入 `IDasLangModuleProvider`。

---

## 2. 分层架构（关键约束）

```
┌─────────────────────────────────────────────────────────┐
│  各服务进程（分进程）: WorldServer / SocialServer / ...    │
│    - 实现 IDasLangModuleProvider（提供本服务的 native 模块）│
│    - 实现 IDasLangHost（回调：发包给客户端等）             │
│    - 每帧驱动 DasLangEngine::Tick()                       │
│    - 持有服务专用 Module（WorldModule / SocialModule）     │
│    - AutoGen/*.gen.cpp（本服务的消息绑定 + 分发）           │
└───────────────────────────┬─────────────────────────────┘
                            │ 依赖（单向，向下）
┌───────────────────────────▼─────────────────────────────┐
│  Src/ScriptEngine/（服务无关的下层公共库）                 │
│    - DasLangEngine（单例，编译/simulate/tick/热重载/AOT）  │
│    - DasCommonModule（"Common" 模块：日志等全服务通用绑定） │
│    - DasLangSerializer（.dabin 序列化 + 可选加密）         │
│    - DasFileWatcher（mtime/size 轮询）                    │
│    - IDasLangModuleProvider / IDasLangHost（接口）        │
└───────────────────────────┬─────────────────────────────┘
                            │ 依赖
┌───────────────────────────▼─────────────────────────────┐
│  Src/Common/*（ByteBuffer / Log / Crypto / Config / ...） │
│  ThirdParty/daScript（libDaScript）                       │
└─────────────────────────────────────────────────────────┘
```

**铁律：`Src/ScriptEngine/` 不得反向依赖任何具体服务（World/Social）。**

这条约束当前被违反了——`DasEngine.cpp` 曾 `#include "World/AutoGen/ProtoBindIndex.gen.h"` 并调 `RegisterAllMsgDispatch()`。重构后：

- `ScriptEngine` 只认接口 `IDasLangModuleProvider`（`CreateModules(group)` 交回服务专用模块）与 `IDasLangHost`。
- 消息类型注册 / 分发注册由**服务侧**在脚本 `Load` 成功后自行调用（`RegisterAllMsgDispatch()` 归 World）。
- 生成的 `*.gen.cpp` 里的 `Dispatch<Msg>Req` 不再引用 `WorldServer`，改走 `DasLangEngine::GetIns()`（见 01 篇），从而符号上也与 World 解耦。

---

## 3. 运行模式 `EScriptMode`

```cpp
enum class EScriptMode
{
    Develop, // 源码编译 + 热重载 + debugger；不缓存不 AOT
    Release, // AOT 基线 + .dabin 缓存/补丁；保留完整编译器以支持热补丁
};
```

| 维度 | Develop | Release |
|---|---|---|
| 脚本来源 | 磁盘 `.das` 源码 | `.dabin`（无明文），miss 时回退源码全量编译 |
| 编译 policy | `debugger=true` | `aot=true, fail_on_no_aot=false` |
| 文件监视 | 开（改盘即热重载） | 关（或仅监视补丁目录） |
| 热更新 | 改 `.das` → 自动 swap | 下发 `.dabin` 补丁 → `RequestReload` → swap |
| 性能 | 解释执行 | AOT 命中的函数原生速度，未命中回退解释器 |

---

## 4. 组件清单与职责

`Src/ScriptEngine/` 下（用户当前命名，本系列沿用）：

| 文件 | 类型 | 职责 |
|---|---|---|
| `DasEngine.h/.cpp` | `DasLangEngine`（单例） | 初始化 daScript 环境、编译、simulate、tick、GC、热重载 swap、AOT policy。**唯一脚本宿主入口** |
| `DasEngineConfig.h` | `DasLangEngineConfig` / `EScriptMode` | 配置：脚本根目录、dabin 目录、补丁目录、模式、监视开关 |
| `DasImage.h` | `DasLangImage`（结构体） | 一次编译产物：`moduleGroup + fileAccess + program + ctx + 缓存的入口 SimFunction*` |
| `DasSerializer.h/.cpp` | `DasLangSerializer` | `.dabin` 序列化/反序列化 + 版本/依赖校验 + 可选 AES 加密（03 篇） |
| `DasFileWatcher.h/.cpp` | `DasFileWatcher` | mtime+size 轮询线程，变更回调（02 篇） |
| `IDasModuleProvider.h` | `IDasLangModuleProvider` | 服务侧提供 native 模块集 + tick 前转发 + swap 通知 + 定时器清空（接口） |
| `IDasHost.h` | `IDasLangHost` | 服务侧回调（发包给客户端等）（接口） |
| `Module/DasCommonModule.h/.cpp` | `DasCommonModule` | `"Common"` 模块：日志等全服务通用绑定 |
| `Module/DasCommonBinds.h` | （新增，AOT 用） | Log 绑定函数的公共头声明（04 篇 AOT-4） |

依赖模块（复用，不重造）：

| 复用 | 来源 | 用途 |
|---|---|---|
| `ByteBuffer` | `Common/Core/ByteBuffer.h` | `.dabin` 大端读写（Own/Wrap） |
| `Log` | `Common/Log/Log.h` | `Log::At/Info/Warn/Error` |
| `Aes256Gcm` | `Common/Crypto/Aes256Gcm.h`（target `CommonCrypto`） | `.dabin` 可选加密（AEAD，加密+防篡改一体） |
| `ConfigLoader` | `Common/Config/ConfigLoader.h` | 从 TOML 读 `EScriptMode` / 目录 / 监视间隔 |
| `std::filesystem` | 标准库 | 文件 mtime/size、读写 |
| `MASSIVE_ASSERT` | `Common/Core/MassiveAssert.h` | 关键不变量 |

---

## 5. 数据流总览

### 5.1 冷启动（Load）

```
        Develop                              Release
        ───────                              ───────
   compileDaScript(main.das)          DasLangSerializer::Load(main.dabin)
        │  (parse+infer+opt)                │  (反序列化 AST，可选解密)
        ▼                                    ▼  miss/损坏 → 回退左路全量编译
   Program(AST) ◄───────────────────────────┘
        │
        ▼  program->simulate(newCtx)   ← AOT 命中函数在此 linkCppAot 绑定原生
   Context
        │
        ▼  findFunction("Init"/"Update"/"dispatch_msg")  缓存 SimFunction*
   DasLangImage（就绪）
        │
        ▼  eval Init  →  服务侧 RegisterAllMsgDispatch()
```

### 5.2 每帧（Tick，逻辑线程）

```
Tick(sceneID, dt):
  ├─ 若 _reloadPending：安全点 swap（见 02 篇）
  ├─ provider->OnPrevTick(dt)
  ├─ ctx->restart()                    // 每帧清 stopFlags/exception（非换 Context）
  ├─ evalWithCatch(funcUpdate, {sceneID, dt})
  └─ 按堆增长阈值 collectHeap
```

### 5.3 消息分发（逻辑线程）

```
网络消息(msgID, sessionID, body,len)
  └─ ScriptDispatchRegistry::Dispatch(msgID, ...)   // O(1) 定长表
       └─ Dispatch<Msg>Req(sessionID, body, len)    // *.gen.cpp
            ├─ req.ParseFromArray(body,len)          // protobuf 解析
            └─ ctx->evalWithCatch(dispatch_msg, {msgID, sessionID, &req})
                 └─ dispatch_msg 查 g_handler_registry[msgID] → handler(sessionID, req)
```

---

## 6. 消息绑定形态变化（本轮重点之一）

**旧**：`GenMsgBindings.py` 同时生成 C++ 绑定 **和** `Script/AutoGen/HandlerRegistry.das`（含 `[msg_handler]` 宏 + `g_msg_name_to_id` 表 + `dispatch_msg`）。

**新**：
- **C++ 侧继续生成**：`EMsgID` 枚举绑定、每个消息的 `ManagedStructureAnnotation`、`Dispatch<Msg>Req`。
- **das 侧改为手写**：`[msg_handler]` 宏、`dispatch_msg`、handler 业务逻辑，全部手写在 `Script/` 下，不再生成。
- **入口更名**：`ServerTick.das` → `main.das`。

详见 01 篇。生成器 v2 已重写并验证（`Tools/Script/GenMsgBindings.py`）。

---

## 7. 最终发布形态

```
发布包（无 .das 明文）：
  bin/WorldServer(.exe)         ← 含 AOT 生成的 *.das.cpp（原生速度）+ 完整 daScript 编译器
  Script/*.dabin                ← 编译后 AST 二进制（可选 AES 加密），启动加载
  patch/*.dabin                 ← 热补丁（下发后 RequestReload 热切换）
```

- **AOT 基线**：`main.das` 及依赖在构建期转 `.cpp` 编进宿主，启动 `simulate` 时按 `aotHash` 把解释节点替换为原生函数。
- **保留完整编译器**：`.dabin` 补丁里被改动的函数 `aotHash` 不匹配 → 自动回退解释器执行；未改动的仍走 AOT。于是线上能打小补丁修逻辑，无需整包发版。
- **无明文**：`.dabin` 是编译后 AST（daScript 已确认序列化不含源码明文），再叠可选 AES 加密防随手 dump。

---

## 8. 实施顺序（建议）

后续 4 篇可按此顺序落地，每步都能独立编译验证：

1. **01 消息绑定 + 引擎接线基线**（含修复编译级阻断 F1/F2/F7）→ 引擎能独立编译、被服务 Initialize/Load/Tick，跑通最小 `main.das`。
2. **02 热重载** → Develop 模式改盘即生效。
3. **03 .dabin** → Release 缓存/补丁跑通（先明文，再叠加密）。
4. **04 AOT** → 构建 daslang 工具、生成 rule、`aotRequire`，Release 全速。

> 每篇文档均给出可照抄的完整 `.h/.cpp/.das/.lua/.py`，不含占位或未实现片段。
