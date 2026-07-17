# 脚本引擎 #18：热更新——ReloadScript + decs_live

> 状态：**设计中**（2026-07-15）
> 关联：[09_ScriptEngine](09_ScriptEngine.md)（总纲 §9）、[10_BridgeModule](10_BridgeModule.md)（MassiveModule）
> 前置依赖：Phase 1-3 完成（DasLang Context 初始化 + DECS 集成 + 脚本 Handler 可用）
> 对应 Phase：Phase 4

## 1. 定位

热更新是脚本引擎的核心价值——修改 Buff 公式、技能 CD、AI 决策后**不重启服务器**即可生效。DaScript 原生支持 `Context::restart()` + `relocateCode()`，DECS `decs_live` 一行 require 搞定状态持久化。

## 2. 机制概述

```
[ReloadScript 被触发] (GM 指令 / HTTP endpoint / 文件监控)
        │
  1. 保存 DECS 状态
     decs_live: mem_archive_save(decsState)
        │
  2. 脚本 shutdown()
     注销事件监听器、取消注册中的定时器（正在执行中的定时器自然过期）
        │
  3. Context::restart()
     清空 DasLang 虚拟机的内部状态——变量、堆栈、全局表
     但 DECS 的 raw data 已 save 到 archive，不受影响
        │
  4. 重新编译 .das 文件
     compileDaScript("Scripts/ServerTick.das")
     → 若编译失败 → Error 日志 + return（旧代码继续运行）
        │
  5. relocateCode(newProgram)
     函数级热替换——同签名函数直接用新实现
        │
  6. decs_live: mem_archive_load(data, decsState)
     恢复所有 DECS Component——entity、archetype、组件数据
        │
  7. 脚本 init()（带 is_reload 标记）
     注册新的事件监听器、重建内部索引
        │
  8. update() 继续
     正常 Tick——DECS 数据完整恢复
```

## 3. 实现

### 3.1 C++ 侧——WorldServer::ReloadScript()

```cpp
// WorldServer.h 新增方法
class WorldServer
{
public:
    /**
     * @brief 热重载脚本
     *
     * 由 GM 指令 /reload_script 或文件监控触发。
     * 编译失败时保留旧代码，服务不中断。
     */
    void ReloadScript();

private:
    das::ProgramPtr compileDaScript(const std::string &entryFile);
    das::ProgramPtr _scriptProgram;  // 当前运行的脚本程序
    std::string     _scriptEntryFile = "Scripts/ServerTick.das";
};
```

```cpp
void WorldServer::ReloadScript()
{
    Log::Info("ReloadScript: starting...");

    MASSIVE_PROFILE_NAME("ReloadScript");

    // 1. 保存 DECS 状态
    // decs_live 在脚本 init 时注册了一个 before_restart hook
    // Context::restart() 触发该 hook → mem_archive_save
    // 如果 decs_live 未正确集成，此步骤手动调用：
    //   auto fnSave = _scriptCtx->findFunction("decs_save_state");
    //   das_invoke<void>::invoke(_scriptCtx.get(), nullptr, fnSave);

    // 2. call shutdown() — 脚本清理
    {
        auto fnShutdown = _scriptCtx->findFunction("shutdown");
        if (fnShutdown) {
            das_invoke<void>::invoke(_scriptCtx.get(), nullptr, fnShutdown);
            Log::Debug("ReloadScript: shutdown() called");
        }
    }

    // 3. restart Context
    _scriptCtx->restart();
    Log::Debug("ReloadScript: context restarted");

    // 4. 重新编译
    auto newProgram = compileDaScript(_scriptEntryFile);
    if (!newProgram || newProgram->failed())
    {
        Log::Error("ReloadScript: compilation FAILED — keeping old code running");

        // 旧 Context 已经被 restart 了——需要重新加载旧 Program
        // 解决方案：restart 前备份旧 Program，失败后 reload 旧 Program
        _scriptCtx->relocateCode(_scriptProgram);  // 恢复旧代码

        // 重新 init()——decs_live 会恢复 DECS 数据
        auto fnInit = _scriptCtx->findFunction("init");
        if (fnInit) {
            das_invoke<void>::invoke(_scriptCtx.get(), nullptr, fnInit, false);  // false = not reload
        }

        _metrics.IncrementCounter("massive_script_reload_failed_total");
        return;
    }

    // 5. 热替换
    _scriptCtx->relocateCode(newProgram);
    _scriptProgram = newProgram;
    Log::Info("ReloadScript: code relocated");

    // 6. decs_live 恢复 DECS 状态
    // Context::restart() 后，decs_live 的 after_restart hook 自动运行 mem_archive_load
    // 如果未自动触发，手动调用：
    //   auto fnLoad = _scriptCtx->findFunction("decs_load_state");
    //   das_invoke<void>::invoke(_scriptCtx.get(), nullptr, fnLoad);

    // 7. re-init——脚本重新初始化
    auto fnInit = _scriptCtx->findFunction("init");
    if (fnInit)
    {
        das_invoke<void>::invoke(_scriptCtx.get(), nullptr, fnInit, true);  // true = is_reload
    }

    // 8. 重新绑定 DasInvoke 函数指针（函数指针可能已变化）
    _fnUpdate     = _scriptCtx->findFunction("update");
    _fnHandleMove = _scriptCtx->findFunction("handle_move");
    _fnHandleEnterWorld = _scriptCtx->findFunction("handle_enter_world");
    // ... 其他 handler

    _metrics.IncrementCounter("massive_script_reload_total");
    Log::Info("ReloadScript: COMPLETE — new code is now active");
}
```

### 3.2 DasLang Program 编译

```cpp
das::ProgramPtr WorldServer::compileDaScript(const std::string &entryFile)
{
    // Phase 1 中实现——创建 TextFileAccess + compile
    auto fAccess = das::make_smart<das::FsFileAccess>();

    // 设定 include 路径——daslib 标准库和项目脚本目录
    fAccess->addIncludeDirectory("ThirdParty/daScript/daslib");
    fAccess->addIncludeDirectory("Scripts");

    auto program = das::compileDaScript(entryFile, fAccess,
                                        *_scriptCtx->getLibraryAccess());
    if (!program || program->failed())
    {
        // 打印编译错误
        for (auto &err : program->errors)
        {
            Log::Error("Script compile error: {}:{} {}",
                       err.at.fileInfo ? err.at.fileInfo->name : "?",
                       err.at.line, err.what);
        }
    }

    return program;
}
```

### 3.3 启动时初始化

```cpp
bool WorldServer::InitScriptEngine()
{
    _scriptCtx = das::make_smart<das::Context>();

    // 加载 builtin modules
    auto builtin = das::Module::builtIn();
    _scriptCtx->addModule(builtin);

    // 加载 MassiveModule
    auto massiveMod = das::make_smart<MassiveModule>(
        this, _entityMgr.get(), &_sceneMgr, &_logicThread.GetTimingWheel(), &_sessions);
    _scriptCtx->addModule(massiveMod);

    // 编译入口脚本
    _scriptProgram = compileDaScript(_scriptEntryFile);
    if (!_scriptProgram || _scriptProgram->failed())
    {
        Log::Error("InitScriptEngine: compilation FAILED");
        return false;
    }

    // simulate + restart（首次初始化）
    _scriptProgram->simulate(*_scriptCtx);
    _scriptCtx->restart();

    // 首次 init
    auto fnInit = _scriptCtx->findFunction("init");
    if (fnInit)
    {
        das_invoke<void>::invoke(_scriptCtx.get(), nullptr, fnInit, false);
    }

    // 缓存函数指针
    _fnUpdate     = _scriptCtx->findFunction("update");
    _fnHandleMove = _scriptCtx->findFunction("handle_move");
    _fnHandleEnterWorld = _scriptCtx->findFunction("handle_enter_world");

    Log::Info("InitScriptEngine: OK ({} functions cached)", 3);
    return true;
}
```

## 4. 脚本侧支持

### 4.1 decs_live 集成

```das
// Scripts/ServerTick.das
require daslib/decs_boost
require daslib/decs_live    // ← 热更新状态保存/恢复
require daslib/archive      // decs_live 依赖 archive 序列化
require massive

var g_isFirstInit = true

[export]
def init(isReload : bool)
{
    if g_isFirstInit {
        // 首次启动——spawn 初始 NPC、加载静态数据
        spawn_initial_npcs()
        g_isFirstInit = false
        massive_log_info("init: first boot — NPCs spawned")
    } else {
        // 热重载——decs_live 已恢复 DECS 数据
        massive_log_info("init: hot reload — DECS state restored")
    }

    // 重新注册 handler 映射（二者都需要）
    register_handlers()
    register_event_listeners()
}

[export]
def shutdown()
{
    // 清理回调、取消残留的定时器
    // 注意：定时器在 C++ TimingWheel 中的注册需要手动清理
    massive_log_info("shutdown: script cleanup complete")
}
```

### 4.2 decs_live 配置

```das
// decs_live 通过以下方式集成——在 init() 首次调用前设置
// decs_live 自动在 Context::restart() 前后 hook 状态

// 可选：手动控制 save/load 时序
[export]
def decs_save_state() {
    // decs_live 提供的高层 API
    // 将所有活跃的 DECS archetype + component 数据序列化到 mem_archive
}

[export]
def decs_load_state() {
    // 从 mem_archive 恢复 DECS 数据
}
```

> **注意**：decs_live 的具体 API（`mem_archive_save` / `decs_live` 的 require 行为）需要在 Phase 1 中实测验证。如果 decs_live 的集成方式与文档预期有偏差，Phase 4 开始前修正本节设计。

## 5. 触发方式

### 5.1 GM 指令（推荐）

```cpp
// WorldServer 中注册一个简单的控制台/GM 通道
void WorldServer::OnGMCommand(uint32 sessionID, const std::string &cmd)
{
    if (cmd == "/reload_script") {
        ReloadScript();
        SendToClient(sessionID, MSG_GM_NTF,
                     BuildGMNotification("Script reloaded successfully"));
    }
    else if (cmd == "/togglescript off") {
        _useScriptHandlers = false;
    }
    else if (cmd == "/togglescript on") {
        _useScriptHandlers = true;
    }
}
```

### 5.2 HTTP endpoint（运营期）

```cpp
// 在 WorldServer 加一个简单的 HTTP 监听端口（或复用现有的 admin 端口）
// POST /admin/reload_script → ReloadScript()
```

### 5.3 文件监控（开发期）

```cpp
// 开发期可选——监控 Scripts/ 目录的 .das 文件变更
// 检测到 .das 文件 mtime 变化 → 自动触发 ReloadScript()
// 使用 std::filesystem::file_time_type 轮询或平台相关文件变更通知
```

## 6. 什么能热更、什么不能

| 更新内容 | 热更？ | 操作 | 状态影响 |
|----------|-------|------|---------|
| DECS Stage 函数实现（改 Buff 伤害公式） | ✅ | 保存 .das → ReloadScript | 无影响——DECS 组件数据在 archive 中保留 |
| DECS struct 增加字段 | ✅ | 改 Components.das → ReloadScript | 旧 entity 保留旧字段值，新字段填默认值——Archive 处理 |
| DECS struct 删除字段 | ✅ | 同上 | archive 反序列化时忽略多余字节 |
| `ecs_stage` 调度顺序 | ✅ | 改 `update()` → ReloadScript | 即刻生效 |
| 新增 Stage 函数 | ✅ | 加 `[decs(stage=...)]` → ReloadScript | 即刻生效 |
| 新增消息 Handler | ✅ | 加 `[export] def handle_xxx` → ReloadScript | 新消息类型立刻可用 |
| C++ Component 定义（Position/Health） | ❌ | 改 .h → 重启服务器 | 所有 entity 和 EnTT state 丢失 |
| C++ 消息分派 switch case | ❌ | 改 .cpp → 重启服务器 | 新增消息类型需要重新注册 DAS_BIND_FUN |
| MassiveModule Bridge 函数 | ❌ | 改 MassiveModule.cpp → 重启服务器 | Module 在 Context 注册时绑定——不可热更 |

## 7. 错误恢复

### 编译错误

```cpp
void WorldServer::ReloadScript()
{
    auto newProgram = compileDaScript(_scriptEntryFile);
    if (!newProgram || newProgram->failed())
    {
        Log::Error("ReloadScript FAILED — keeping old code");
        // 恢复旧 Program + re-init
        _scriptCtx->relocateCode(_scriptProgram);
        auto fnInit = _scriptCtx->findFunction("init");
        if (fnInit) das_invoke<void>::invoke(_scriptCtx.get(), nullptr, fnInit, false);
        return;  // ← 旧代码继续运行，服务不中断
    }
    // ... relocation ...
}
```

### DECS 反序列化失败

```cpp
// decs_live 提供 try/recover 机制
// 反序列化失败 → WARN 日志 + 清空 DECS 世界 + 重新 spawn 初始 NPC
// 这意味着玩家身上的 Buff 会丢失——可接受的权衡（比服务中断好）
```

## 8. 文件清单

```
Scripts/
├── ServerTick.das                 # 修改——追加 init(isReload) / shutdown / decs_live 集成
└── Handlers.das                   # 修改——register_handlers() 区分首次/重载

Src/World/
├── WorldServer.h                  # 修改——追加 ReloadScript() / _scriptProgram
├── WorldServer.cpp                # 修改——追加 compileDaScript() / GM 指令处理
```

## 9. 依赖

| 依赖 | 状态 |
|------|------|
| `das::Context` 初始化 / `compileDaScript` | Phase 1 产出 |
| DECS 集成（decs_live / archive） | Phase 1 产出 + Phase 4 实测验证 |
| 脚本 Handler（handle_move / handle_enter_world） | Phase 3 产出 |
| GM 指令通道 | Phase 4 新增 |
