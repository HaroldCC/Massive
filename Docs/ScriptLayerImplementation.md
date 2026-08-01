# Massive 脚本层实现细化文档 — DasLangEngine + `.dabin` 热补丁

> 承接 `ScriptLayerDesign.md`。本文是**手动编码实现指南**：文件结构、类结构、`.dabin` 格式、序列化/反序列化接线、热重载 swap、补丁构建工具、线程模型。
>
> **发布形态已定**：AOT 基线（全速）+ 序列化 blob（`.dabin`）热补丁（无明文源码、能配合 AOT）+ 运行时全量热重载。
>
> **重构方向已定**：WorldServer 的脚本模拟整体删除，编译/simulate/tick/热重载生命周期全部收敛到 `DasLangEngine`；WorldServer 退化为 `IDasLangtHost` 回调提供方。
>
> **模块分层已定**：单一 `MassiveModule` 删除重构，改为三层 native 模块——`CommonModule`（公共：Log/Timer/数学/Entity 通用 + **公共 Proto 绑定**）为两个服务共用；`WorldModule`（Scene/战斗/AOI + World 专用 Proto）、`SocialModule`（好友/公会/聊天 + Social 专用 Proto）各服务专用。服务是**分进程**的（World / Social 独立进程）。

---

## 0. 四条硬约束（编码前必读，来自源码核实）

1. **`.dabin` 加载必须用 `AstSerializer::serializeProgram(program, libGroup)`，不能用裸 C API。**
   - 读取端对非 builtin 脚本模块，会用 `libGroup.findModule(name)` 复用你**活着的 C++ native 模块**（`CommonModule`/`WorldModule`/…），只反序列化脚本模块本身。
   - 裸 `das_program_serialize`/`das_program_deserialize` 走 `Program::serialize`，会把 native 模块整体反序列化成普通 Module，**C++ 函数指针失效**——不可用。
   - 源码依据：`module_builtin_ast_serialize.cpp:2662-2683`（读取端 `findModule`+`serializeModule(already_exists=true)`）。

2. **`.dabin` 只是编译后 AST（含 `aotHash`），不含源码明文，但也不是强加密。**
   - `TextFileInfo::serialize`（:1991）只写 `name/tabSize/sourceLength`，源码文本被显式注释掉不写入。
   - blob 保存 `hash`+`aotHash`（:1374）→ 反序列化的 Program 带 aotHash，simulate 时能命中 AOT 库。
   - 若要防逆向，在 `.dabin` 外层再套项目自己的对称加密 + 签名（与脚本层解耦，见 §7）。

3. **反序列化 ≠ 可运行。** blob 反序列化出的是 `Program`，仍需 `simulate()` 才得到可执行 `Context`。流水线不变：`blob → Program → simulate → Context → eval`。

4. **`.dabin` 按服务隔离，模块集必须与生成时逐一对齐。**
   - 反序列化端 `libGroup` 必须含**该服务运行时的全部 live native 模块**（World = Common+World；Social = Common+Social）。
   - World 的补丁 `.dabin` 只能在 World 进程加载——Social 进程缺 `WorldModule` 会 `failToCompile`。
   - builtin/daslib 模块按 `cumulativeHash` 校验（:2646），native 模块结构变化也会导致哈希不符 → 生成端（`DasPack`）与运行端的**模块集、`CodeOfPolicies`、daslang 版本必须完全一致**。

---

## 0.5 模块分层与依赖方向（架构关键）

`ScriptEngine` 是被各服务依赖的**下层公共库，不能反向依赖 World/Social**。因此模块归属：

```
Src/ScriptEngine/                         ← 下层公共库（不认识任何具体服务）
├── DasEngine                             DasLangEngine（服务无关宿主）
├── Module/CommonModule.h/.cpp            公共 native 模块（require common）
└── IScriptModuleProvider.h               服务向引擎提供模块集的接口

Src/World/Script/                         ← World 进程实现
├── WorldModule.h/.cpp                    World 专用 native（require world）
└── WorldModuleProvider.h/.cpp            实现 IScriptModuleProvider：addModule(Common+World)

Src/Social/Script/                        ← Social 进程实现
├── SocialModule.h/.cpp                   Social 专用 native（require social）
└── SocialModuleProvider.h/.cpp           实现 IScriptModuleProvider：addModule(Common+Social)
```

- **依赖方向**：`World → ScriptEngine`、`Social → ScriptEngine`；ScriptEngine 不依赖任何服务。`CommonModule` 因两服务共用，放在 ScriptEngine。
- **脚本侧 require**：`Script/Common/*.das` → `require common`；`Src/World/Script/*.das` → `require common` + `require world`；Social 同理。
- **运行时 ModuleGroup**：World 进程 = `CommonModule + WorldModule`；Social 进程 = `CommonModule + SocialModule`。
- **Proto 绑定归属**：跨服务共享的消息（Login/Common）→ `CommonModule`；World/Social 专属消息 → 各自模块。即现有的 `RegisterCommonProtoBindings` 挂进 CommonModule，World 专属的 `RegisterXxxProtoBindings` 挂进 WorldModule。

### `IScriptModuleProvider` 接口

```cpp
// Src/ScriptEngine/IScriptModuleProvider.h
#pragma once
#include "daScript/ast/ast.h"
#include <string>

namespace MMO
{
    // 服务（World/Social）向引擎提供其 native 模块集与入口脚本。
    // 引擎不认识任何具体服务，只通过此接口拿模块。
    class IScriptModuleProvider
    {
    public:
        virtual ~IScriptModuleProvider() = default;

        // 把本服务需要的所有 live native 模块 addModule 进 group（含公共 CommonModule）。
        // ★ 每次编译/反序列化都会调用——它决定了 libGroup 内容（硬约束1/4）。
        virtual void CreateModules(das::ModuleGroup &group) = 0;

        // 入口脚本相对路径，如 "World/ServerTick.das"。
        virtual const char *EntryScript() const = 0;

        // 服务标识（用于 .dabin 服务隔离校验 + 缓存/补丁目录），如 "world"。
        virtual const char *ServiceName() const = 0;
    };
} // namespace MMO
```

> `CommonModule` 由谁 `new` / 持有？两种做法：(a) 引擎持有 CommonModule 单例，Provider 的 `CreateModules` 里 `group.addModule(engine.Common())` + 自己的模块；(b) Provider 全权持有全部模块。推荐 **(a)**——公共模块生命周期归引擎，避免每服务重复 new。`WorldModuleProvider` 持有 `WorldModule`，`CreateModules` 里先 `addModule(CommonModule)` 再 `addModule(WorldModule)`。

---

## 1. 文件结构

```
Src/ScriptEngine/                   ← 下层公共库（服务无关）
├── DasEngine.h / .cpp              # DasLangEngine 单例：唯一脚本宿主（重写，服务无关）
├── DasEngineConfig.h               # 配置：模式/根目录/缓存目录（扩展）
├── IDasHost.h                      # IDasLangtHost 宿主回调接口（保留）
├── IScriptModuleProvider.h         # ★ 新增：服务向引擎提供模块集的接口
├── DasHelpers.h                    # TArray 等辅助（保留）
├── ScriptImage.h                   # ★ 新增：一次编译产物（program/ctx/moduleGroup/access）
├── ScriptSerializer.h / .cpp       # ★ 新增：.dabin 读写（serializeProgram 封装 + 头部 + 校验）
├── ScriptWatcher.h / .cpp          # ★ 新增：开发期文件监视（mtime+size 轮询）
├── Module/
│   └── CommonModule.h / .cpp       # ★ 公共 native 模块（Log/Timer/数学/Entity 通用 + 公共 Proto）
│                                   #   （原 DasCommonModule 并入/改名；原 MassiveModule 公共部分迁入）
└── xmake.lua                       # 增加 das_aot rule（阶段 4）

Src/World/                          ← World 进程
├── WorldServer.h / .cpp            # 删除脚本模拟成员，持有 DasLangEngine + 实现 IDasLangtHost
├── Script/
│   ├── WorldModule.h / .cpp        # ★ World 专用 native（Scene/战斗/AOI + World 专属 Proto）
│   └── WorldModuleProvider.h/.cpp  # ★ 实现 IScriptModuleProvider（Common+World）
└── ...

Src/Social/                        ← Social 进程（对称）
├── SocialServer.h / .cpp
└── Script/
    ├── SocialModule.h / .cpp       # ★ Social 专用 native
    └── SocialModuleProvider.h/.cpp # ★ 实现 IScriptModuleProvider（Common+Social）

Script/                            ← 脚本资源
├── Common/                         # require common 的公共 .das
├── World/ServerTick.das            # require common + world
└── Social/ServerTick.das           # require common + social

Tools/                             # ★ 补丁构建工具（宿主外的独立可执行）
└── DasPack/
    ├── main.cpp                    # DasPack --service world <entry.das> <out.dabin>
    └── xmake.lua

Bin/<plat>-<arch>-<mode>/
├── daslang(.exe)                   # ★ 阶段4：AOT 代码生成工具（新增 target）
├── ScriptCache/<service>/          # 二进制缓存（本地加速，非分发物）
│   └── ServerTick.dabin
└── Patches/<service>/              # 运行时热补丁投放目录（分发物，按服务隔离）
    └── *.dabin
```

---

## 2. 类结构

### 2.1 `ScriptImage` — 一次编译的完整产物

```cpp
// Src/ScriptEngine/ScriptImage.h
#pragma once
#include "daScript/ast/ast.h"
#include "daScript/simulate/simulate.h"
#include <memory>
#include <string>

namespace MMO
{
    // 一次脚本编译的完整产物，作为整体原子 swap。
    struct ScriptImage
    {
        // ★ moduleGroup 必须声明在最前 → 最后析构。
        //   Program/Context 通过 library.modules 持有指入 moduleGroup 的裸 Module*，
        //   moduleGroup 先析构会造成悬垂。析构顺序 = 声明逆序。
        std::unique_ptr<das::ModuleGroup> moduleGroup;
        das::FileAccessPtr                access;
        das::ProgramPtr                   program;
        std::shared_ptr<das::Context>     ctx;

        // ★ 只缓存热路径函数指针（swap 后必须重取，见 §4.3）。
        //   fnInit / fnShutdown 是一次性调用（Load/HotReload 各一次），
        //   用局部变量即可，不作为成员——findFunction 是 O(N) strcmp 全表扫描
        //   (context.cpp:606)，只有每帧/每消息的热路径才值得缓存。
        das::SimFunction *fnUpdate      = nullptr;   // 每 tick 调用
        das::SimFunction *fnDispatchMsg = nullptr;   // 每消息调用

        std::string errors;

        bool IsValid() const { return ctx != nullptr; }
    };
} // namespace MMO
```

> **析构顺序是正确性关键**：`~ScriptImage` 按成员声明逆序析构 → `ctx` → `program` → `access` → `moduleGroup`。moduleGroup 最后走，安全。
>
> **为何不缓存 `fnInit`**：`Context::findFunction` 是 O(N) 线性遍历 + 每项 `strcmp`（`context.cpp:606`，非哈希查找）。`fnUpdate`（每 20ms）、`fnDispatchMsg`（每条消息）是热路径，必须缓存；`fnInit`/`fnShutdown` 每次加载/重载只调一次，用完即弃的局部变量更合适。

### 2.2 `DasLangEngine` — 唯一脚本宿主

```cpp
// Src/ScriptEngine/DasEngine.h
#pragma once
#include "DasEngineConfig.h"
#include "ScriptImage.h"
#include "IDasHost.h"
#include <atomic>
#include <memory>
#include <string>

namespace MMO
{
    class CommonModule;
    class IScriptModuleProvider;
    class ScriptWatcher;

    class DasLangEngine
    {
    public:
        static DasLangEngine &GetIns();

        // 生命周期。provider 由具体服务（World/Social）注入——引擎服务无关。
        bool Initialize(const DasLangEngineConfig &cfg, IDasLangtHost *host, IScriptModuleProvider *provider);
        void Shutdown();

        // 引擎持有的公共模块（Provider 的 CreateModules 里 addModule 它）
        CommonModule *Common() const { return _common.get(); }

        // 首次加载：优先 .dabin 缓存/补丁 → 失败回退全量编译 → simulate → Init()
        bool Load(const std::string &entryFile);

        // 逐帧驱动（在 LogicThread 内调用）
        void Tick(float dt);                                   // 内含 swap 检查点
        bool DispatchMsg(uint32 sessionID, uint32 msgID, const uint8 *body, size_t len);

        // 热补丁：投放 .dabin 后请求重载（线程安全，仅置标志）
        void RequestReload(const std::string &dabinPath);      // 补丁热更
        void RequestReloadFromSource();                        // 开发期改源码重载

        // 供 IDasLangtHost 实现读取
        das::Context     *GetScriptContext() const { return _image.ctx.get(); }
        das::SimFunction *GetDispatchFunc() const  { return _image.fnDispatchMsg; }

        const std::string &GetLastErrors() const { return _image.errors; }

    private:
        DasLangEngine() = default;

        // 编译一个全新 ScriptImage（不 swap）。cacheOrPatch 非空则先试 .dabin。
        ScriptImage Compile(const std::string &entryFile, const std::string &dabinPath);
        // simulate + 重取函数指针
        bool        SimulateImage(ScriptImage &img);
        void        RebindFunctions(ScriptImage &img);
        // tick 边界原子 swap
        void        DoSwap(ScriptImage &&neo);

        das::CodeOfPolicies MakePolicies() const;
        // 组装 ModuleGroup：委托 provider->CreateModules（内部 addModule Common+服务专用模块）
        void                BuildModuleGroup(ScriptImage &img);

    private:
        DasLangEngineConfig            _cfg;
        IDasLangtHost                 *_host     = nullptr;
        IScriptModuleProvider         *_provider = nullptr;  // 由服务注入，引擎不 own
        std::unique_ptr<CommonModule>  _common;              // 引擎持有的公共 live 模块
        ScriptImage                    _image;                // 当前运行镜像
        std::string                    _entryFile;

        // 热重载请求（跨线程）
        std::atomic<bool>              _reloadPending{false};
        std::string                    _pendingDabin;           // 由 _reloadMtx 保护
        std::mutex                     _reloadMtx;

        std::unique_ptr<ScriptWatcher> _watcher;    // 仅开发期
        bool                           _initialized = false;
    };
} // namespace MMO
```

### 2.3 `ScriptSerializer` — `.dabin` 读写（核心）

```cpp
// Src/ScriptEngine/ScriptSerializer.h
#pragma once
#include "daScript/ast/ast.h"
#include "daScript/ast/ast_serializer.h"
#include <cstdint>
#include <string>
#include <vector>

namespace MMO
{
    // .dabin 文件头（明文，用于快速校验，不含脚本内容）
    struct DabinHeader
    {
        char     magic[4];        // "DBIN"
        uint32   formatVersion;   // 我们自己的格式版本（改动格式时 +1）
        uint32   dasVersion;      // = das::AstSerializer::getVersion() (=93)
        uint32   pointerSize;     // = sizeof(void*)
        uint32   depCount;        // 依赖文件数
        // 之后：depCount × { uint16 pathLen; char path[]; int64 mtime }
        // 之后：uint64 blobSize; uint8 blob[blobSize]   (serializeProgram 产物)
    };

    class ScriptSerializer
    {
    public:
        // 写：把已编译 program 序列化成 .dabin。libGroup 必须与编译时一致。
        // deps = program->getCtx? 用 ctx->getAllFiles() 收集的 {path,mtime}
        static bool Save(const std::string           &outPath,
                         das::ProgramPtr               program,
                         das::ModuleGroup             &libGroup,
                         const std::vector<std::pair<std::string, int64>> &deps);

        // 读：校验头 → 反序列化成 program。失败返回 nullptr（调用方回退全量编译）。
        // ★ libGroup 必须已含该服务全部 live native 模块（Common+World），供读取端 findModule 复用。
        // access 用于校验依赖 mtime（可选，nullptr 跳过 mtime 校验，仅靠版本/hash）。
        static das::ProgramPtr Load(const std::string &inPath,
                                    das::ModuleGroup  &libGroup,
                                    das::FileAccess   *access,
                                    std::string       &outError);
    };
} // namespace MMO
```

`ScriptSerializer.cpp` 关键实现（**这是全篇最容易踩坑处**）：

```cpp
#include "ScriptSerializer.h"
#include "daScript/misc/smart_ptr.h"

namespace MMO
{
    bool ScriptSerializer::Save(const std::string &outPath,
                                das::ProgramPtr program,
                                das::ModuleGroup &libGroup,
                                const std::vector<std::pair<std::string, int64>> &deps)
    {
        // 1. 序列化 Program（写模式）
        das::SerializationStorageVector storage;
        {
            das::AstSerializer ser(&storage, /*isWriting*/ true);
            ser.thisModuleGroup = &libGroup;
            ser.serializeProgram(program, libGroup);   // ★ 用 serializeProgram，不用 Program::serialize
            ser.moduleLibrary = nullptr;               // 断开引用，避免析构顺序问题
        }

        // 2. 写文件：头 + 依赖表 + blob
        DabinHeader h{};
        memcpy(h.magic, "DBIN", 4);
        h.formatVersion = kDabinFormatVersion;
        h.dasVersion    = das::AstSerializer::getVersion();
        h.pointerSize   = (uint32)sizeof(void *);
        h.depCount      = (uint32)deps.size();
        // ... 写 h、每个 dep 的 {pathLen,path,mtime}、blobSize、storage.buffer ...
        return WriteAllBytes(outPath, h, deps, storage.buffer);
    }

    das::ProgramPtr ScriptSerializer::Load(const std::string &inPath,
                                           das::ModuleGroup &libGroup,
                                           das::FileAccess *access,
                                           std::string &outError)
    {
        // 1. 读头 + 校验
        std::vector<uint8> bytes;
        if (!ReadAllBytes(inPath, bytes)) { outError = "read fail"; return nullptr; }
        DabinHeader h; std::vector<std::pair<std::string,int64>> deps; std::vector<uint8> blob;
        if (!ParseDabin(bytes, h, deps, blob)) { outError = "parse fail"; return nullptr; }

        if (memcmp(h.magic, "DBIN", 4) != 0)           { outError = "bad magic"; return nullptr; }
        if (h.formatVersion != kDabinFormatVersion)    { outError = "format ver"; return nullptr; }
        if (h.dasVersion != das::AstSerializer::getVersion()) { outError = "das ver"; return nullptr; }
        if (h.pointerSize != sizeof(void *))           { outError = "ptr size"; return nullptr; }
        // 依赖 mtime 校验（开发期缓存用；纯补丁分发可跳过）
        if (access)
            for (auto &[path, savedMtime] : deps)
                if (access->getFileMtime(path) != savedMtime) { outError = "stale dep: " + path; return nullptr; }

        // 2. 反序列化（读模式）—— ★ 关键接线
        das::SerializationStorageVector storage;
        storage.buffer.assign(blob.begin(), blob.end());
        das::ProgramPtr program = das::make_smart<das::Program>();
        {
            das::AstSerializer deser(&storage, /*isWriting*/ false);
            deser.thisModuleGroup = &libGroup;         // ★ 供读取端复用 native 模块（见硬约束1）
            deser.serializeProgram(program, libGroup);
            deser.moduleLibrary = nullptr;
        }

        // 3. builtin/daslib cumulativeHash 不符 或 native 模块缺失 → failToCompile
        if (program->failed())
        {
            outError = "deserialize failed (module hash mismatch or native module missing)";
            return nullptr;
        }
        program->thisModuleGroup = &libGroup;
        return program;   // 仍需 simulate
    }
} // namespace MMO
```

> **为何 `serializeProgram` 而非 `Program::serialize`**：见 §0 硬约束 1。前者读取端 `libGroup.findModule(name)` 复用 live native 模块；后者不重连。二者字节格式**不兼容**，写读必须成对。

### 2.4 `ScriptWatcher` — 开发期文件监视

```cpp
// Src/ScriptEngine/ScriptWatcher.h
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>

namespace MMO
{
    // 后台线程轮询 mtime+size，变化则回调（回调只置标志，不做 swap）。
    class ScriptWatcher
    {
    public:
        using OnChanged = std::function<void()>;

        void Start(std::vector<std::string> files, int64 pollMs, OnChanged cb);
        void SetFiles(std::vector<std::string> files);   // swap 后依赖集变化时重建
        void Stop();

    private:
        void Loop();
        struct Snapshot { int64 mtime; int64 size; };
        std::unordered_map<std::string, Snapshot> _snapshot;   // 由 _mtx 保护
        std::mutex        _mtx;
        std::thread       _thread;
        std::atomic<bool> _running{false};
        int64             _pollMs = 500;
        OnChanged         _cb;
    };
} // namespace MMO
```

> 同时比对 **mtime 和 size**（捕获同一秒内的快速编辑，daslang-live 的做法）。文件清单来自 `ctx->getAllFiles()`（含所有 require 依赖）。

### 2.45 `WorldModuleProvider` — World 进程的模块提供者（服务侧）

```cpp
// Src/World/Script/WorldModuleProvider.h
#pragma once
#include "ScriptEngine/IScriptModuleProvider.h"
#include "WorldModule.h"
#include <memory>

namespace MMO
{
    class WorldModuleProvider : public IScriptModuleProvider
    {
    public:
        explicit WorldModuleProvider(IDasLangtHost *host) : _world(std::make_unique<WorldModule>(host)) { _world->BindFunctions(); }

        void CreateModules(das::ModuleGroup &group) override {
            group.addModule(DasLangEngine::GetIns().Common());   // ★ 先加公共模块（引擎持有）
            group.addModule(_world.get());                       // 再加 World 专用模块
        }
        const char *EntryScript() const override { return "World/ServerTick.das"; }
        const char *ServiceName() const override { return "world"; }

        WorldModule *World() const { return _world.get(); }      // swap 时重绑 ctx 用

    private:
        std::unique_ptr<WorldModule> _world;
    };
} // namespace MMO
```

> `SocialModuleProvider` 完全对称（`SocialModule`、`"Social/ServerTick.das"`、`"social"`）。
> **注意**：`WorldModule` 的 ctx 重绑 / 定时器 drain，引擎在 swap 时也要通知它。若专用模块也有 ctx 状态，`IScriptModuleProvider` 可再加 `virtual void OnContextSwapped(shared_ptr<Context>) {}` / `virtual void DrainTimers() {}`，引擎在 §4.1 步骤4、§4.4 (c) 里对 provider 一并调用（比逐个模块硬编码更解耦）。

### 2.5 WorldServer 侧（删脚本模拟，改实现 `IDasLangtHost`）

```cpp
// WorldServer.h（删除所有脚本模拟成员，保留宿主回调）
class WorldServer : public IDasLangtHost
{
    // 删除：_scriptCtx / _scriptProgram / _massiveModule / _fnInit / _fnUpdate / _fnDispatchMsg / CompileDaScript / InitScriptEngine
    // IDasLangtHost 实现：
    das::Context     *GetScriptContext() const override { return DasLangEngine::GetIns().GetScriptContext(); }
    das::SimFunction *GetDispatchFunc() const override  { return DasLangEngine::GetIns().GetDispatchFunc(); }
    void SendRawToClient(uint32 sessionID, uint32 msgID, const uint8 *data, size_t len) override;  // 真正实现
};

// OnTick（LogicThread 内）
void WorldServer::OnTick(float dt) { DasLangEngine::GetIns().Tick(dt); /* Tick 内含 swap 检查 + Update() */ }
// OnMessage
bool WorldServer::OnMessage(uint32 sid, uint32 msgID, const uint8 *body, size_t len) {
    if (ScriptDispatchRegistry::Dispatch(*this, sid, msgID, body, len)) return true;  // gen.cpp 经 getter 取 ctx/fn
    return DasLangEngine::GetIns().DispatchMsg(sid, msgID, body, len);
}
```

> **命名修复**：把生成代码里的 `GetDispatchMsgFunction()` 统一成 `GetDispatchFunc()`（改 `GenMsgBindings.py` 模板），或反之——二者取一，注意 CodingStandard §1.2 的 `ID` 大写规则。

---

## 3. `.dabin` 格式（二进制布局）

```
偏移   内容
0      char[4]   magic = "DBIN"
4      uint32    formatVersion        (我们的格式版本)
8      uint32    dasVersion           (= AstSerializer::getVersion() = 93)
12     uint32    pointerSize          (= 8)
16     uint32    depCount
20     ┌─ depCount × 依赖项 ─────────────────
       │  uint16  pathLen
       │  char[]  path (相对路径, 归一化为 '/')
       │  int64   mtime
       └──────────────────────────────────
...    uint64    blobSize
...    uint8[]   blob = serializeProgram 产物 (含 aotHash, 不含源码明文)
[可选] 外层加密/签名见 §7
```

- **依赖表**用途：开发期缓存判过期（任一 dep mtime 变 → 失效回退编译）。纯补丁分发场景可把 depCount 置 0（补丁由服务端版本管理，不靠本地 mtime）。
- **路径归一化**：统一用 `/` 分隔符（避免 `\` vs `/` 差异；这点也影响 AOT 哈希，见 Design 文档 §5.4）。

---

## 4. 热重载 / 热补丁流程

### 4.1 首次加载 `Load(entryFile)`

```
1. _common = make_unique<CommonModule>(_host, ...);  _common->BindFunctions();
   （WorldModule 等专用模块由 provider 持有并 BindFunctions）
2. img.moduleGroup = make_unique<ModuleGroup>();
   _provider->CreateModules(*img.moduleGroup);        // addModule(Common) + addModule(World)
3. img = Compile(entryFile, cacheDabinPath):
     a. 若存在 .dabin: program = ScriptSerializer::Load(dabin, *img.moduleGroup, access, err)
        命中 → 跳过全量编译
     b. 未命中/失败: program = compileDaScript(entryFile, access, logs, *img.moduleGroup, MakePolicies())
        成功后: ScriptSerializer::Save(cacheDabinPath, program, *img.moduleGroup, deps)  // 写缓存
4. SimulateImage(img):  img.ctx = make_shared<Context>(program->getContextStackSize());
                        _common->SetContext(img.ctx);          // 重绑公共模块 ctx（World 模块同理）
                        program->simulate(*img.ctx, logs)     // AOT 在此按 aotHash 链接
5. RebindFunctions(img):  fnUpdate/fnDispatchMsg = findFunction(...)   // 仅热路径
6. if (auto *fnInit = img.ctx->findFunction("Init")) img.ctx->evalWithCatch(fnInit, nullptr);  // 局部
7. _image = std::move(img)
8. (开发期) _watcher->Start(img.ctx->getAllFiles(), 500, [this]{ RequestReloadFromSource(); })
```

> `provider->CreateModules` 内部（推荐做法 a）：`group.addModule(DasLangEngine::GetIns().Common()); group.addModule(_worldModule.get());`。每次编译/反序列化都调它，保证 libGroup 始终含全部 live native 模块（硬约束 1/4）。

### 4.2 请求重载（跨线程，仅置标志）

```cpp
void DasLangEngine::RequestReload(const std::string &dabinPath) {   // 补丁下发线程调用
    { std::lock_guard lk(_reloadMtx); _pendingDabin = dabinPath; }
    _reloadPending.store(true, std::memory_order_release);
}
void DasLangEngine::RequestReloadFromSource() {                     // 文件监视线程调用
    { std::lock_guard lk(_reloadMtx); _pendingDabin.clear(); }      // 空 = 从源码重编
    _reloadPending.store(true, std::memory_order_release);
}
```

### 4.3 `Tick(dt)` — LogicThread 内的安全 swap 点

```cpp
void DasLangEngine::Tick(float dt) {
    // ★ swap 必须在任何 eval 之前（Context::restart 有 insideContext==0 断言）
    if (_reloadPending.exchange(false, std::memory_order_acquire)) {
        std::string dabin; { std::lock_guard lk(_reloadMtx); dabin = _pendingDabin; }
        HotReload(dabin);   // 见 4.4
    }
    // 正常 tick
    _image.ctx->restart();
    vec4f args[2] = { cast<int32>::from(sceneID), cast<float>::from(dt) };
    _common->SetScriptDt(dt);
    if (_image.fnUpdate) _image.ctx->evalWithCatch(_image.fnUpdate, args);
    HandleException("Update");
}
```

### 4.4 `HotReload(dabinPath)` — 编译新镜像 + 原子 swap

```cpp
void DasLangEngine::HotReload(const std::string &dabinPath) {
    // (a) 旧 ctx 收尾：保存状态 + shutdown
    _image.ctx->restart();
    CallAnnotated(_image.ctx, "before_reload");     // 或读全局序列化到临时 store
    // (b) 编 全新镜像（独立 moduleGroup，复用 live native 模块）
    ScriptImage neo;
    neo.moduleGroup = std::make_unique<das::ModuleGroup>();
    _provider->CreateModules(*neo.moduleGroup);      // ★ 复用同一批 live native 模块（Common+World）
    neo.access = das::make_smart<das::FsFileAccess>();
    das::FsFileAccess *fa = static_cast<das::FsFileAccess*>(neo.access.get());
    fa->introduceDaslib();

    das::ProgramPtr program;
    if (!dabinPath.empty())                           // 补丁：优先反序列化 .dabin
        program = ScriptSerializer::Load(dabinPath, *neo.moduleGroup, neo.access.get(), neo.errors);
    if (!program)                                     // 开发期或补丁失败：全量编译
        program = compileDaScript(_entryFile, neo.access, logs, *neo.moduleGroup, MakePolicies());
    neo.program = program;

    if (!program || program->failed() || !SimulateImage(neo)) {
        // ★ 安全网：新镜像失败 → 保留旧 ctx 继续跑
        Log::Error("HotReload failed, keep running old script: {}", neo.errors);
        _image.ctx->restart();
        CallAnnotated(_image.ctx, "after_reload");
        if (auto *fnInit = _image.ctx->findFunction("Init")) _image.ctx->evalWithCatch(fnInit, nullptr);
        return;
    }
    RebindFunctions(neo);                             // 仅 fnUpdate/fnDispatchMsg

    // (c) 原子 swap → 旧 ctx/program/moduleGroup 在此析构
    _common->DrainTimerCallbacks();                   // 旧定时器持旧 ctx，清空（World 模块若有同理）
    _image = std::move(neo);                          // ★ 覆盖 = 旧镜像析构（moduleGroup 最后）
    _common->SetContext(_image.ctx);                  // 重绑公共模块 ctx（专用模块同理）

    // (d) 迁移状态 + 重新初始化
    CallAnnotated(_image.ctx, "after_reload");        // after_reload 在 init 之前
    if (auto *fnInit = _image.ctx->findFunction("Init")) _image.ctx->evalWithCatch(fnInit, nullptr);
    if (_watcher) _watcher->SetFiles(GetWatchList(_image.ctx));
}
```

### 4.5 swap 后必须刷新的失效项清单

| 项 | 处理 |
|---|---|
| `_image.ctx / program / moduleGroup` | `std::move(neo)` 整体替换 |
| `_image.fnUpdate / fnDispatchMsg` | `RebindFunctions` 重取 `findFunction`（fnInit 用局部变量，无需重取成员） |
| `CommonModule::_ctx`（及 WorldModule 等专用模块的 ctx） | `SetContext(newCtx)` 重绑 |
| 各模块 `_timerCallbacks[].ctx` | **drain**：`DrainTimerCallbacks()` 清空，否则定时器触发踩悬垂 ctx |
| `_nextTimerID` | 保留或归零（视语义） |
| 脚本全局值 | `before_reload`/`after_reload` 迁移；`g_handler_registry` 由 `[msg_handler]` 宏 simulate 时自动重建 |
| WorldServer 侧无缓存指针 | 因全部经 getter 取 `_image`，天然刷新 |

---

## 5. 补丁构建工具 `Tools/DasPack`

宿主外的独立可执行，把 `.das` 编译并序列化成 `.dabin`。**在构建机/发布流水线上运行，不进发布包。**

```cpp
// Tools/DasPack/main.cpp
// 用法: DasPack --service <world|social> <entry.das> <out.dabin> [--key <hexkey>]
int main(int argc, char **argv) {
    das::setDasRoot(...);
    NEED_ALL_DEFAULT_MODULES;
    das::Module::Initialize();

    // ★ 按 --service 注册与运行时该服务完全相同的 native 模块集
    das::ModuleGroup libGroup;
    std::unique_ptr<CommonModule> common = std::make_unique<CommonModule>(/*桩 host*/);
    common->BindFunctions();
    libGroup.addModule(common.get());
    if (service == "world") { auto w = std::make_unique<WorldModule>(...); w->BindFunctions(); libGroup.addModule(w.get()); }
    else if (service == "social") { /* SocialModule 同理 */ }

    auto access = das::make_smart<das::FsFileAccess>(); access->introduceDaslib();
    das::TextWriter logs;
    das::CodeOfPolicies pol = MakeScriptPolicies(EScriptMode::Release);  // ★ 与运行时共用同一函数
    auto program = das::compileDaScript(entry, access, logs, libGroup, pol);
    if (!program || program->failed()) { /* 报错退出，打印 logs */ }

    // 收集依赖 mtime（可选，补丁分发可置空）
    std::vector<std::pair<std::string,int64>> deps;
    // ... 编译后从 ctx->getAllFiles() 或 getPrerequisits 枚举 ...

    ScriptSerializer::Save(out, program, libGroup, deps);   // 写 .dabin
    // [可选] EncryptAndSign(out, key);  见 §7
    return 0;
}
```

> **关键**：DasPack 的 `--service`、`CodeOfPolicies`、daslang 版本、native 模块集合，必须与运行时该服务的 `DasLangEngine` **完全一致**——否则 builtin `cumulativeHash` 或 native 模块结构不符，运行时 `Load` 会 `failToCompile`。
> - 把 `MakeScriptPolicies(mode)` 抽到 `ScriptEngine` 的共享头，DasPack 与引擎都用它。
> - DasPack 应能**复用 Provider 的 `CreateModules`**（把 provider 构造逻辑抽到共享代码），确保模块集与运行时逐字节一致，而不是在 DasPack 里手写第二份 addModule。

**补丁流程（按服务隔离）**：
```
构建机: DasPack --service world World/ServerTick.das world_ServerTick.dabin
  → 投放到 World 服务器 Bin/<...>/Patches/world/
  → 运维/中心服触发 World 进程 DasLangEngine::RequestReload("Patches/world/world_ServerTick.dabin")
  → 下个 tick 边界 swap 生效
  → 改过的函数 aotHash 不命中 → 走解释器（新逻辑正确）；未改函数继续 AOT 全速
  ⚠ world 补丁投到 Social 进程会因缺 WorldModule 而 Load 失败 → 保留旧脚本（安全网）
```

---

## 6. 线程模型

| 线程 | 职责 | 与脚本的关系 |
|---|---|---|
| **LogicThread** | tick / Update / dispatch / **swap** | 唯一执行/替换脚本 Context 的线程 |
| **文件监视线程**（开发期） | 轮询 mtime+size | 只调 `RequestReloadFromSource()` 置标志 |
| **补丁下发线程** | 接收补丁、落盘 | 只调 `RequestReload(path)` 置标志 |
| **IO 线程** | 网络收发 | 经 `_sessionsMtx` 碰 `_sessions`，不碰脚本 |

- **`Context::restart()` 有 `DAS_ASSERTF(insideContext==0)`** → 不能在脚本执行中 swap。所有 eval 都在 LogicThread，swap 也放 LogicThread（Tick 开头），故**无需锁保护 Context**。
- 跨线程只有 `_reloadPending`(atomic) + `_pendingDabin`(mutex) 两个小同步点。
- **进阶优化（可选，非首版）**：把 `compileDaScript`/`Load` 放后台 worker 线程编好，只把 swap 留在 LogicThread，避免大脚本重编造成 tick 卡顿。需给 worker 线程独立 `daScriptEnvironment` bound（`das_environment_get_bound`/`set_bound`）。首版先同步编译。

---

## 7. 防逆向（可选外层）

`.dabin` 是非明文 AST，但非强加密。若需防逆向：

- **DasPack** 写完 blob 后，对整个 `.dabin`（或仅 blob 段）做对称加密（AES-GCM）+ 签名（Ed25519/HMAC）。
- **运行时** `ScriptSerializer::Load` 先验签 + 解密到内存 buffer，再喂 `AstSerializer`。
- 密钥管理与分发安全属项目策略，与脚本层解耦。首版可先不加，`.dabin` 默认防护级别（非明文）通常够用。

---

## 8. 实现顺序与验证清单

### 阶段 0 — 收敛到 DasLangEngine + 模块分层（前置）
- [ ] 拆分 `MassiveModule`：公共部分 → `Src/ScriptEngine/Module/CommonModule`（原 `DasCommonModule` 并入/改名）；World 专用 → `Src/World/Script/WorldModule`。
- [ ] 定义 `IScriptModuleProvider`；实现 `WorldModuleProvider`（Common+World）。
- [ ] `DasLangEngine` 服务无关化：`Initialize(cfg, host, provider)`，持有 `CommonModule`，编译时委托 `provider->CreateModules`。
- [ ] Proto 绑定归位：公共 `RegisterCommonProtoBindings` → CommonModule；World 专属 → WorldModule。
- [ ] WorldServer 删除全部脚本模拟成员，改实现 `IDasLangtHost`（真正实现 getter）+ 构造 `WorldModuleProvider` 注入引擎。
- [ ] `DasLangEngine::Load/Tick/DispatchMsg` 打通全量编译路径。
- [ ] 统一 `GetDispatchFunc` 命名（含 gen 模板 `GenMsgBindings.py`）。
- [ ] **验证**：xmake 编译通过；World 进程加载运行 `World/ServerTick.das`（`require common` + `require world`），Update/dispatch 正常。

### 阶段 1 — `.dabin` 序列化往返
- [ ] `ScriptImage` + `ScriptSerializer::Save/Load`（用 `serializeProgram`+libGroup）。
- [ ] `Load` 走"先试 .dabin → 回退编译 → 写缓存"。
- [ ] **验证**：冷启动写 `.dabin`；二次启动命中、跳过编译（对比日志耗时）；手改 `.das` 后依赖 mtime 失效回退编译。**桥接函数（MassiveModule）反序列化后仍可调用**（重点验证硬约束1）。

### 阶段 2 — 热重载 / 热补丁
- [ ] `ScriptWatcher`（开发期）+ `RequestReload*` + `Tick` 内 swap + `HotReload` + drain 定时器。
- [ ] 编译失败保留旧 ctx 安全网。
- [ ] `Tools/DasPack` 构建工具。
- [ ] **验证**：开发期改源码不重启即生效；`DasPack` 生成 `.dabin` → `RequestReload` → swap 生效；改出语法错时旧脚本继续跑。

### 阶段 3 — AOT 基线
- [ ] 新增 `daslang` 可执行 target；`rule("das_aot")`（见 Design §5.3）。
- [ ] release `MakePolicies` 开 `aot=true; fail_on_no_aot=false`。
- [ ] `MassiveModule` 实现 `Module::aotRequire()`。
- [ ] **验证**：release 构建 `findFunction("Update")->aot==true`；打 `.dabin` 补丁后改过的函数 `aot==false`（走解释器）、未改的 `aot==true`。

---

## 9. 一手 API 速查（本方案用到的）

| API | 头 / 位置 | 用途 |
|---|---|---|
| `AstSerializer(SerializationStorage*, bool writing)` | `ast/ast_serializer.h` | 序列化器 |
| `AstSerializer::serializeProgram(ProgramPtr, ModuleGroup&)` | 同上 | ★ 读写 Program（复用 native 模块） |
| `AstSerializer::getVersion()` | 同上 | =93，写入 `.dabin` 头校验 |
| `SerializationStorageVector` | 同上 | `vector<uint8> buffer` 存 blob |
| `AstSerializer::thisModuleGroup` | 同上 | 读取端设为 &libGroup |
| `compileDaScript(fn, access, logs, libGroup, policies)` | `ast/ast.h:1831` | 全量编译 |
| `Program::simulate(ctx, logs)` | `ast/ast.h:1731` | AST→Context，AOT 在此链接 |
| `Program::failed()` | `ast/ast.h` | 编译/反序列化失败判定 |
| `Context::findFunction(name)` | `simulate/simulate.h:534` | swap 后重取函数指针 |
| `Context::getAllFiles()` | `simulate/simulate.h:768` | 依赖清单（监视 + 缓存 key） |
| `Context::restart()` | `simulate/simulate.h:447` | eval/swap 前调用；insideContext==0 断言 |
| `Context::evalWithCatch(fn, args)` | `simulate/simulate.h:490` | 带异常捕获调用 |
| `FsFileAccess` / `introduceDaslib()` | `simulate/fs_file_info.h:29` | 磁盘文件源；每次重载新建 |
| `FileAccess::getFileMtime(name)` | `simulate/debug_info.h:257` | 依赖 mtime 校验 |
| `CodeOfPolicies{ aot, fail_on_no_aot, threadlock_context, persistent_heap, rtti }` | `ast/ast.h:1509` | 编译策略（Save/运行时必须一致） |

---

*本文档基于 `ThirdParty/daScript` 源码逐一核实。硬约束 1（serializeProgram + libGroup 复用 native 模块）是整个 `.dabin` 方案能否正确工作的关键，务必按 §2.3 接线。*
