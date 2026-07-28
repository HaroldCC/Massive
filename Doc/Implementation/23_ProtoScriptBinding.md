# 脚本引擎 #23：Protobuf 消息直绑定 daScript —— `[msg_handler]` + `ManagedStructureAnnotation`

> 状态：**已实现**（2026-07-28，修订 3：落地验证发现的技术细节同步 + MsgID 改为 C++ enum 绑定）
> 修订 2：统一属性/函数调用语法 + req 生命周期编译期防护 + 生成代码命名空间隔离
> 修订 3（本次）：MsgIDConstants.das 不再手写维护，改为 C++ 侧 `EnumerationEMsgID` 直接绑定 `MMO::Proto::EMsgID`；§5.4 `HandlerRegistry.das` 代码同步为实际编译通过的版本；§6 `Handlers.das` 同步；§2.3 补充 `.\`` 属性名前缀机制说明；§5.4 补充编译期/运行时 context 隔离说明；§9 xmake 从 `on_config` 改为 `on_load` 解决首次构建缺少符号问题
> 关联：[09_ScriptEngine](09_ScriptEngine.md)（总纲）、[10_BridgeModule](10_BridgeModule.md)（MassiveModule 现有 15 个桥接函数）、[13_MessageMigration](13_MessageMigration.md)（本设计取代其 §4/§6/§11 的 `MsgArgs`/`das_invoke` 扁平参数方案）、[20_HandlerValidation](20_HandlerValidation.md)（本设计取代其 `msg=<常量>` 语法，改为 `msg="<消息名>"` 字符串）
> 前置依赖：`Src/World/MassiveModule.cpp` 现有 `ManagedStructureAnnotation<BattleStats>` 模式已验证可行（见 `10_BridgeModule.md §3.2`），本设计是对同一机制的推广，不引入新的绑定原语
> 对应 Phase：替代原 Phase 2（MassiveModule）+ Phase 3（消息迁移）+ Phase 4（HandlerValidation）中与消息处理相关的部分

## 0. 变更摘要

### 相对 13_MessageMigration.md / 20_HandlerValidation.md

| 项目 | 旧方案（13/20 号文档） | 新方案（本文档） |
|---|---|---|
| 脚本侧收到的数据 | 手写 `MoveReqArgs` 等扁平结构体，字段逐个从 protobuf 拷贝 | protobuf 生成的 C++ Message 类本身，整体注册为 daScript 类型 |
| C++ → 脚本传参方式 | `vec4f args[]` 手动打包标量 | 单个指针参数：`das::cast<const Proto::MoveReq *>::from(&req)` |
| `msg_handler` 注解参数 | `msg=MSG_MOVE_REQ`（daScript 语法上会被解析成裸标识符字符串，原文档未察觉这一点——**此为原文档已确认的技术错误**） | `msg="MoveReq"`（显式字符串，与实际语法行为一致） |
| string/bytes 字段 | 标注为"需要 Bridge 层特殊处理"，未给出具体方案 | 生成器自动产出取值属性，机制与标量字段等价（见 §5.1、§8） |
| repeated 字段 | 未处理 | 生成器自动产出 `_size` 属性 + 索引访问函数（见 §5.1、§8） |
| **生成产物粒度** | 单个 `ProtoBind.gen.cpp` + 单个 `MsgDispatch.gen.cpp` | **按 .proto 文件拆分**——每个含业务消息的 `.proto` 文件对应一个同名 `.gen.cpp` |

### 本次修订（相对上一版本文档）

| 项目 | 上一版 | 本版 |
|---|---|---|
| string/bytes 字段调用语法 | `req.username()`（走 `addExtern`，函数调用带括号） | `req.username`（改走 `addExternProperty`，属性语法不带括号）——见 §2.3 |
| `req` 生命周期防护 | 只有文档约定，无编译期强制 | 生成的 `XxxAnnotation` 覆写 `canCopy()`/`canClone()` 为 `false`——见 §2.4 |
| 生成代码命名空间 | 生成的 helper 裸置于文件全局作用域 | 包进匿名命名空间隔离，公开注册函数保留在 `namespace MMO { }`——见 §5.1、§5.3 |
| MsgID 常量维护 | `Script/AutoGen/MsgIDConstants.das` 手写 `let MSG_XXX = Nu` 常量 | C++ 侧 `EnumerationEMsgID` 直接绑定 `MMO::Proto::EMsgID` 枚举到 daScript，脚本用 `uint(EMsgID.MSG_MOVE_REQ)`；生成器产出绑定代码到 `ProtoBindIndex.gen.cpp`——见 §5.3 |
| xmake 构建时机 | `on_config` 触发生成，依赖 `xmake f -c` 重新通配符扫描 | `on_load` 触发生成 + `target:add("files", f)` 显式纳入，无需手动操作——见 §9 |
| `HandlerRegistry.das` 实际编译 | 代码仅为设计示意，从未通过实际编译器验证 | 全部代码已在实际 daScript 编译器下编译通过并运行验证——见 §5.4 |

**结论**：本文档完全取代 `13_MessageMigration.md` 的 §4/§5/§6/§11 与 `20_HandlerValidation.md` 的 §3/§4。两份旧文档的问题背景（§1/§2/§8 验证流程/§9 废弃清单）继续有效。

---

## 1. 设计目标

1. 脚本 handler 直接操作 protobuf 消息对象（`req.speed`、`req.position.x`），不经过手写的中间结构体。
2. 消息处理函数用注解声明"我处理哪条消息"，编译期做尽可能多的静态校验，杜绝魔法数字和手误。
3. C++ 侧新增一条协议消息时，除了写 `.proto` 文件本身，**不需要手写任何 C++/daScript 绑定代码**——由生成器产出。
4. 复杂字段（`string`/`bytes`/`repeated`）与标量字段享受同等的自动化程度和同等的调用语法，不留"需要手工特殊处理"或"语法不一致"的模糊地带。
5. `req` 的生命周期越界（脚本试图跨调用保留引用）应尽量在编译期而不是运行期暴露。
6. 生成代码不污染全局命名空间，遵循项目既有的匿名命名空间隔离约定。
7. 与项目既有的窄接口 Bridge 哲学一致：不做通用的反射式序列化，每个消息类型的绑定代码都是具体、编译期确定的。
8. 生成的 C++ 文件按来源 `.proto` 文件拆分，避免单个巨型 `.gen.cpp` 随协议消息数量增长而膨胀。

## 2. 核心技术依据

### 2.1 `ManagedStructureAnnotation` 可以整体绑定任意 C++ 类，不要求 POD 布局

现有代码 `Src/World/MassiveModule.cpp` 尚未启用的 `BattleStats` 绑定路径、以及 `ThirdParty/daScript/modules/dasPUGIXML/src/dasPUGIXML.cpp` 中对 `pugi::xml_node`（一个不透明第三方类型）的绑定，都证明了 `ManagedStructureAnnotation<T>::addProperty<FunT, PROP>`（`include/daScript/ast/ast_handle.h:280-286`）绑定的是**任意成员函数指针**，不依赖固定内存偏移：

```cpp
// 已在上游 daScript 验证——dasPUGIXML.cpp:104-109
struct XmlNodeAnnotation : ManagedStructureAnnotation<pugi::xml_node, true, true> {
    XmlNodeAnnotation(ModuleLibrary &ml) : ManagedStructureAnnotation("xml_node", ml, "pugi::xml_node") {
        addProperty<DAS_BIND_MANAGED_PROP(name)>("name");   // 绑定 xml_node::name() 方法
        addProperty<DAS_BIND_MANAGED_PROP(value)>("value");
    }
};
```

protobuf 生成的消息类（`Proto::MoveReq` 等）恰好也是"公开成员函数返回字段值"的形状（`sequence()`、`position()`、`speed()`……），因此可以照搬同一手法整体注册，无需先拆成扁平结构体。

### 2.2 `msg_handler` 注解参数必须是字符串，不是符号常量

daScript 语法规则（`src/parser/ds2_parser.ypp:1131-1141`）中，annotation 参数里的裸标识符（`NAME` token）永远被解析成**字面字符串**（标识符文本本身），语法阶段不做常量替换——`daslib/decs_boost.das` 里 `[decs(stage=update_ai)]` 的 `stage` 参数就是同样处理成字符串（该文件 `argPass = find_arg(args, "stage"); if (!(argPass is tString))`）。因此本文档采用 `msg="MoveReq"`（显式字符串字面量），而不是 `msg=MSG_MOVE_REQ`（会被解析成字符串 `"MSG_MOVE_REQ"`，与预期的整数比较必然失败）。

### 2.3 `addExternProperty` 可以把任意自由函数注册成无括号属性语法——不限于成员函数指针

`ManagedStructureAnnotation::addProperty<FunT, PROP>`（`ast_handle.h:280-286`）要求 `PROP` 是**成员函数指针**（`DAS_BIND_MANAGED_PROP` 宏的展开形式），但 protobuf 生成的 string 字段访问需要一层自由函数包装（`std::string` → `const char*`），不能直接拿 `msg.username()` 的方法指针去绑（返回类型不匹配 daScript 认识的类型）。

底层的 `addExternProperty<FuncT, fn, SimNodeT>`（`ast_interop.h:236-256`）不要求 `fn` 是成员函数指针——它接受任意函数指针类型，`ManagedStructureAnnotation::addProperty` 内部的 `RegisterProperty<callT::ref, callT>::reg`（`ast_handle.h:218-224`）正是靠这一层把成员函数指针包装成一个自由的 `static_call` 函数后再传给 `addExternProperty`。这意味着我们可以直接对自己写的自由函数（如 `LoginAuthReq_GetUsername(const Proto::LoginAuthReq &msg)`）调用 `addExternProperty<DAS_BIND_FUN(LoginAuthReq_GetUsername)>(...)`，跳过成员函数指针这一层要求，得到跟标量字段完全一致的无括号属性语法：`req.username`。

这不是新发明的机制——是 `ManagedStructureAnnotation::addProperty` 本身依赖的更底层 API，daScript 官方文档 `integration_cpp_09_operators_and_properties.rst` 明确把"属性看起来像字段但背后调用 C++ 方法"作为受支持的绑定模式，本文档只是把这一层直接暴露给自由函数使用。

**关键实现细节——`.`` 属性名前缀**：`expr.field` 语法在 daScript 类型推断阶段被翻译为查找名为 `".`" + fieldName` 的函数（`ast_infer_type.cpp:4157`）。`ManagedStructureAnnotation::addProperty` 内部自动做了这个拼接——传给底层 `addExternProperty` 的 name 参数是 `".`speed"` 而非裸 `"speed"`。但当我们直接调用底层 `addExternProperty<DAS_BIND_FUN(fn)>(mod, lib, "username")` 时，**这个拼接必须手动完成**：传 `".`username"` 作为 name 参数，并显式传入原 C++ 函数名作为 `cppName` 供 AOT emit 使用。如果传裸 `"username"`，编译器的字段访问语法找不到对应的属性函数，脚本侧 `req.username` 会报 `field 'username' not found`——生成器必须知晓这一规则并在所有非成员函数指针的 `addExternProperty` 调用里加上 `.`` 前缀。

**语法一致性规则**：只要一个 protobuf 字段的取值不需要额外参数（标量、string/bytes、嵌套 message、repeated 的 `_size`），生成器就用 `addExternProperty`，脚本侧写 `req.field`，不带括号。只有需要额外参数的操作（repeated 的索引访问 `moves(i)`）才用 `addExtern`，脚本侧必然带括号——这是该操作本身需要传参数的语法结构决定的，不是字段复杂度决定的。

### 2.4 `req` 生命周期防护——编译期阻断跨调用逃逸

`req` 是 C++ 侧栈上局部变量的引用，仅在 `handle_xxx` 这一次调用期间有效。仅靠文档约定"脚本必须复制值、不能保留引用"不够可靠——脚本作者可能疏忽把 `req` 赋值给全局变量或存进某个持久化的 daScript 结构体字段。

**防护机制**：在每个生成的 `XxxAnnotation : ManagedStructureAnnotation<...>` 类里显式覆写 `canCopy()` 和 `canClone()` 为 `false`：

```cpp
struct MoveReqAnnotation : ManagedStructureAnnotation<MMO::Proto::MoveReq, false, false>
{
    MoveReqAnnotation(ModuleLibrary &ml)
        : ManagedStructureAnnotation("MoveReq", ml, "MMO::Proto::MoveReq")
    {
        addProperty<DAS_BIND_MANAGED_PROP(sequence)>("sequence");
        // ...
    }

    // 阻断任何形式的赋值/克隆——脚本只能读字段/调用方法/按引用传递给同调用链内的函数，
    // 不能把 req 存进全局变量、数组、或另一个结构体字段。
    virtual bool canCopy() const override { return false; }
    virtual bool canClone() const override { return false; }
};
```

`canCopy() == false` 阻断 `g_saved = req` 这类指针复制赋值；`canClone() == false` 阻断 `g_saved := req` 这类深拷贝赋值（`daslib` 的 `:=` clone 操作符依赖 `canClone()`）。两者都覆写为 `false` 后，脚本里任何试图把 `req` 存进全局变量、数组元素、或另一个 struct 字段的写法都会在**编译期**报错（`error: type is not clonable/copyable`），而不是运行期读到失效内存才发现问题。

**残余风险（有意保留，不在本方案范围内解决）**：如果脚本作者故意用 `unsafe(addr(req))` 之类手法绕过类型系统去存指针，这个防护挡不住——但这已经越过了本项目 daScript 使用规范里"`unsafe` 是显式选择进入危险区"的既有边界（见 `ThirdParty/daScript/CLAUDE.md` "Unsafe" 一节），属于故意违反编码规范的行为，不属于"不小心写错"的范畴，交由 code review 兜底即可，不需要额外的编译期机制。

**只读访问不受影响**：`canCopy`/`canClone` 只影响"把整个对象存起来"这类操作，读字段/属性（`req.speed`）、调用方法、把 `req` 作为参数传给同一调用链内的另一个函数都不受影响——这正是设计目标里"handler 只应该在本次调用内使用 `req`"这个使用模式所需要的全部能力。

## 3. 总体架构

```
Client → Gate → World IO Thread → Per-Session inbox → LogicThread::ProcessMessages()
                                                          │
                                             ┌────────────┘
                                             ▼
                                     控制消息（心跳/未路由 EnterWorld）→ C++ 处理（不变）
                                             │
                                     业务消息（*Req）
                                             │
                                     ScriptDispatchRegistry::Dispatch(msgID)
                                     → 找到该 .proto 文件对应 gen.cpp 里生成的分发函数
                                             │
                                     C++ 侧 protobuf ParseFromArray（仅此一步，不拆字段）
                                             │
                                             ▼
                                     ctx->eval(dispatch_msg, {msgID, sessionID, &req})
                                             │
                                     dispatch_msg() 按 msgID 查表 → 调用对应 [msg_handler]
                                             │
                                     脚本 handler：req.字段 直接读取 → 写 EnTT/DECS →
                                     构造出站消息 → SendToClient
```

## 4. 生成产物清单（按 .proto 文件拆分）

```
Tools/Script/GenMsgBindings.py                # 新增——生成器本体
Src/World/AutoGen/<ProtoFileName>.gen.cpp      # 生成——每个含 *Req 消息或被引用类型的 .proto 文件对应一个
                                                #   例: Common.gen.cpp / Login.gen.cpp / Move.gen.cpp
Src/World/AutoGen/ProtoBindIndex.gen.h         # 生成——汇总声明，供手写代码 include
Src/World/AutoGen/ProtoBindIndex.gen.cpp       # 生成——汇总调用 + EMsgID 枚举绑定（替代手写 MsgIDConstants.das）
Src/World/ScriptDispatchRegistry.h             # 新增，手写基础设施——msgID → 脚本分发函数表
Src/World/ScriptDispatchRegistry.cpp           # 新增，手写基础设施
Script/AutoGen/HandlerRegistry.das             # 生成——[msg_handler] 注解类 + 注册表 + 完整性校验
                                                #        EMsgID 枚举值通过 C++ 侧的 EnumerationEMsgID 绑定
                                                #        提供，不再依赖独立的 MsgIDConstants.das 文件
```

不再产出 `MsgArgs.gen.h`——脚本直接操作 protobuf 对象，无需中间结构体。也不再产出单一的 `ProtoBind.gen.cpp`/`MsgDispatch.gen.cpp`——两者的内容按来源 `.proto` 文件拆分进各自的 `<ProtoFileName>.gen.cpp`。

**生成范围判定规则**：生成器只为落在"依赖闭包"内的 `.proto` 文件产出 `.gen.cpp`。闭包 = 全部 `*Req` 消息所在的文件 ∪ 这些消息（递归）引用的其它 message 所在的文件。例如 `Replicate.proto` 只含 `*Ntf`（出站专用，没有 `*Req`，也不被任何 `*Req` 引用），不在闭包内，不产出 `Replicate.gen.cpp`。

## 5. 生成产物详细规格

### 5.1 每个 `.proto` 文件对应的 `<ProtoFileName>.gen.cpp`

单个生成文件只包含**该 `.proto` 文件里定义的类型**——依赖类型（如 `Vector3`）的注册函数、该文件内 `*Req` 消息的 `ManagedStructureAnnotation`、该文件内 `*Req` 消息的分发函数与 string/repeated 辅助函数。每个文件对外只暴露两类函数：`Register<ProtoFileName>ProtoBindings(...)`（类型注册）和 `Register<ProtoFileName>MsgDispatch()`（分发表注册）。**文件内部的 helper 类型/函数全部放进匿名命名空间**，与项目现有 `Src/World/MassiveModule.cpp`（`namespace { MassiveModule *g_massiveMod; struct ResolvedEntity { ... }; ResolveEntity(...) { ... } } using namespace das; using namespace MMO;`）的既有约定保持一致，只有两个对外注册函数留在 `namespace MMO { }` 里。

#### `Src/World/AutoGen/Common.gen.cpp`（依赖类型，无 `*Req`，因此没有分发注册）

```cpp
/**
 * @file Common.gen.cpp
 * @brief 自动生成文件——Common.proto 中被业务消息引用的辅助类型注册
 *
 * 生成工具: Tools/Script/GenMsgBindings.py
 * 来源: Src/Proto/Common.proto
 * @warning 不要手动编辑
 */
#include "Proto/AutoGen/Common.pb.h"
#include "World/ScriptDispatchRegistry.h"
#include "World/WorldServer.h"

#include <daScript/simulate/simulate.h>
#include <daScript/ast/ast_interop.h>
#include <daScript/ast/ast_handle.h>
#include <daScript/daScriptModule.h>
#include <daScript/ast/ast_typefactory.h>

MAKE_TYPE_FACTORY(Vector3, MMO::Proto::Vector3)
MAKE_TYPE_FACTORY(ErrorInfo, MMO::Proto::ErrorInfo)
namespace
{
    using namespace das;

    // ── string/bytes 字段：用 addExternProperty 注册，脚本侧写 req.message（无括号）──
    // 注册时 name 必须是 ".`message"（见 §2.3 的 .` 前缀规则），cppName 用于 AOT emit
    const char *ErrorInfo_GetMessage(const MMO::Proto::ErrorInfo &msg)
    {
        return msg.message().c_str();
    }

    struct Vector3Annotation : ManagedStructureAnnotation<MMO::Proto::Vector3, false, false>
    {
        Vector3Annotation(ModuleLibrary &ml)
            : ManagedStructureAnnotation("Vector3", ml, "MMO::Proto::Vector3")
        {
            addProperty<DAS_BIND_MANAGED_PROP(x)>("x");
            addProperty<DAS_BIND_MANAGED_PROP(y)>("y");
            addProperty<DAS_BIND_MANAGED_PROP(z)>("z");
        }

        // Vector3 是值类型坐标数据，脚本可以安全复制/克隆保存——不覆写 canCopy/canClone。
    };

    struct ErrorInfoAnnotation : ManagedStructureAnnotation<MMO::Proto::ErrorInfo, false, false>
    {
        ErrorInfoAnnotation(ModuleLibrary &ml)
            : ManagedStructureAnnotation("ErrorInfo", ml, "MMO::Proto::ErrorInfo")
        {
            addProperty<DAS_BIND_MANAGED_PROP(code)>("code");
        }

        // ErrorInfo 是值类型，允许复制保存——不覆写 canCopy/canClone。
    };

} // namespace

namespace MMO
{

    void RegisterCommonProtoBindings(das::Module &mod, das::ModuleLibrary &lib)
    {
        (void)mod;
        mod.addAnnotation(new Vector3Annotation(lib));
        mod.addAnnotation(new ErrorInfoAnnotation(lib));
        das::addExternProperty<DAS_BIND_FUN(ErrorInfo_GetMessage)>(mod, lib, ".`message", "ErrorInfo_GetMessage")
            ->args({"msg"});
    }

} // namespace MMO
```

> `Vector3` 不覆写 `canCopy`/`canClone`——它是纯坐标值（`x`/`y`/`z` 三个 `float`），没有 §2.4 描述的"背后是转瞬即逝的栈内存"问题，允许脚本把 `let pos = req.position` 的结果继续保存或传递。§2.4 的防护只施加在直接来自网络包解析、生命周期与本次调用绑定的顶层 `*Req` 消息类型上。

#### `Src/World/AutoGen/Move.gen.cpp`

```cpp
/**
 * @file Move.gen.cpp
 * @brief 自动生成文件——Move.proto 消息类型注册 + 消息分发
 *
 * 生成工具: Tools/Script/GenMsgBindings.py
 * 来源: Src/Proto/Move.proto
 * @warning 不要手动编辑
 * @note MoveReq 覆写了 canCopy/canClone 为 false——脚本不能把 req 存进
 *       全局变量或结构体字段，只能在本次调用内读取字段/按引用传递。
 */
#include "Proto/AutoGen/Move.pb.h"
#include "World/ScriptDispatchRegistry.h"
#include "World/WorldServer.h"

#include "Proto/AutoGen/Move.pb.h"
#include "Proto/AutoGen/Common.pb.h"
#include <MsgID.pb.h>
#include "World/ScriptDispatchRegistry.h"
#include "World/WorldServer.h"

#include <daScript/simulate/simulate.h>
#include <daScript/ast/ast_interop.h>
#include <daScript/ast/ast_handle.h>
#include <daScript/daScriptModule.h>
#include <daScript/ast/ast_typefactory.h>

MAKE_TYPE_FACTORY(MoveReq, MMO::Proto::MoveReq)
MAKE_TYPE_FACTORY(MoveRsp, MMO::Proto::MoveRsp)
MAKE_TYPE_FACTORY(Vector3, MMO::Proto::Vector3)
namespace
{
    using namespace das;

    struct MoveReqAnnotation : ManagedStructureAnnotation<MMO::Proto::MoveReq, false, false>
    {
        MoveReqAnnotation(ModuleLibrary &ml)
            : ManagedStructureAnnotation("MoveReq", ml, "MMO::Proto::MoveReq")
        {
            addProperty<DAS_BIND_MANAGED_PROP(sequence)>("sequence");
            addProperty<DAS_BIND_MANAGED_PROP(position)>("position");   // 返回 const Vector3&（Common.gen.cpp 已注册）
            addProperty<DAS_BIND_MANAGED_PROP(direction)>("direction");
            addProperty<DAS_BIND_MANAGED_PROP(speed)>("speed");
            addProperty<DAS_BIND_MANAGED_PROP(timestamp)>("timestamp");
        }

        // §2.4 生命周期防护——阻断脚本把 req 赋值/克隆进任何逃逸本次调用的存储位置
        virtual bool canCopy() const override { return false; }
        virtual bool canClone() const override { return false; }
    };

    bool DispatchMoveReq(MMO::WorldServer &server, uint32 sessionID, const uint8 *body, size_t len)
    {
        MMO::Proto::MoveReq req;
        if (!req.ParseFromArray(body, static_cast<int>(len)))
        {
            MMO::Log::Error("Move.gen: MoveReq parse failed, session={}", sessionID);
            return false;
        }

        das::Context *ctx        = server.GetScriptContext();
        auto          fnDispatch = server.GetDispatchMsgFunction();
        if (!ctx || !fnDispatch)
        {
            return false;
        }

        das::vec4f callArgs[3] = {
            das::cast<uint32_t>::from(static_cast<uint32_t>(MMO::Proto::MSG_MOVE_REQ)),
            das::cast<uint32_t>::from(sessionID),
            das::cast<const MMO::Proto::MoveReq *>::from(&req),
        };
        ctx->eval(fnDispatch, callArgs);
        return true;
    }
} // namespace

namespace MMO
{

    void RegisterMoveProtoBindings(das::Module &mod, das::ModuleLibrary &lib)
    {
        (void)mod;
        mod.addAnnotation(new MoveReqAnnotation(lib));
    }

    void RegisterMoveMsgDispatch()
    {
        ScriptDispatchRegistry::Register(static_cast<uint32_t>(MMO::Proto::MSG_MOVE_REQ), &DispatchMoveReq);
    }

} // namespace MMO
```

#### `Src/World/AutoGen/Login.gen.cpp`（含 string 字段示例——属性语法，无括号）

```cpp
/**
 * @file Login.gen.cpp
 * @brief 自动生成文件——Login.proto 消息类型注册 + 消息分发
 *
 * 生成工具: Tools/Script/GenMsgBindings.py
 * 来源: Src/Proto/Login.proto
 * @warning 不要手动编辑
 * @note HeartbeatReq, LoginAuthReq, LoginEnterWorldReq 覆写了 canCopy/canClone 为 false——同 Move.gen.cpp 的生命周期防护。
 */
#include "Proto/AutoGen/Login.pb.h"
#include "Proto/AutoGen/Common.pb.h"
#include <MsgID.pb.h>
#include "World/ScriptDispatchRegistry.h"
#include "World/WorldServer.h"

#include <daScript/simulate/simulate.h>
#include <daScript/ast/ast_interop.h>
#include <daScript/ast/ast_handle.h>
#include <daScript/daScriptModule.h>
#include <daScript/ast/ast_typefactory.h>

MAKE_TYPE_FACTORY(LoginAuthReq, MMO::Proto::LoginAuthReq)
MAKE_TYPE_FACTORY(ErrorInfo, MMO::Proto::ErrorInfo)
namespace
{
    using namespace das;

    // ── string/bytes 字段用 addExternProperty 注册（§2.3）──
    // name 参数必须是 ".`字段名"，cppName 传原 C++ 函数名供 AOT emit 使用
    const char *LoginAuthReq_GetUsername(const MMO::Proto::LoginAuthReq &msg)
    {
        return msg.username().c_str();
    }

    const char *LoginAuthReq_GetPassword(const MMO::Proto::LoginAuthReq &msg)
    {
        return msg.password().c_str();
    }

    struct LoginAuthReqAnnotation : ManagedStructureAnnotation<MMO::Proto::LoginAuthReq, false, false>
    {
        LoginAuthReqAnnotation(ModuleLibrary &ml)
            : ManagedStructureAnnotation("LoginAuthReq", ml, "MMO::Proto::LoginAuthReq")
        {
            // 标量字段通过 addProperty 绑定（无括号）——在构造函数体里
        }

        virtual bool canCopy() const override { return false; }
        virtual bool canClone() const override { return false; }
    };

    // ... HeartbeatReqAnnotation / LoginEnterWorldReqAnnotation 同理，省略 ...

    bool DispatchLoginAuthReq(MMO::WorldServer &server, uint32 sessionID, const uint8 *body, size_t len)
    {
        MMO::Proto::LoginAuthReq req;
        if (!req.ParseFromArray(body, static_cast<int>(len)))
        {
            MMO::Log::Error("Login.gen: LoginAuthReq parse failed, session={}", sessionID);
            return false;
        }

        das::Context *ctx        = server.GetScriptContext();
        auto          fnDispatch = server.GetDispatchMsgFunction();
        if (!ctx || !fnDispatch)
        {
            return false;
        }

        das::vec4f callArgs[3] = {
            das::cast<uint32_t>::from(static_cast<uint32_t>(MMO::Proto::MSG_LOGIN_AUTH_REQ)),
            das::cast<uint32_t>::from(sessionID),
            das::cast<const MMO::Proto::LoginAuthReq *>::from(&req),
        };
        ctx->eval(fnDispatch, callArgs);
        return true;
    }
} // namespace

namespace MMO
{

    void RegisterLoginProtoBindings(das::Module &mod, das::ModuleLibrary &lib)
    {
        (void)mod;
        mod.addAnnotation(new LoginAuthReqAnnotation(lib));
        // ... 其他 Annotation 注册 ...

        // §2.3：addExternProperty + name=".`username" + cppName="LoginAuthReq_GetUsername"
        // 脚本侧 req.username / req.password，不带括号
        das::addExternProperty<DAS_BIND_FUN(LoginAuthReq_GetUsername)>(mod, lib, ".`username", "LoginAuthReq_GetUsername")
            ->args({"msg"});
        das::addExternProperty<DAS_BIND_FUN(LoginAuthReq_GetPassword)>(mod, lib, ".`password", "LoginAuthReq_GetPassword")
            ->args({"msg"});
    }

    void RegisterLoginMsgDispatch()
    {
        ScriptDispatchRegistry::Register(static_cast<uint32_t>(MMO::Proto::MSG_LOGIN_AUTH_REQ), &DispatchLoginAuthReq);
        // ... 其他 Dispatch 注册 ...
    }

} // namespace MMO
```

**repeated 字段生成模板**（以假设的 `ItemMoveReq { repeated ItemMove moves = 1; }` 为例，当前 `.proto` 尚无此消息，此处作为生成器模板说明，若真实存在会落在对应的 `Item.gen.cpp` 里，同样包在匿名命名空间内）：

```cpp
namespace
{
    using namespace das;

    // _size 无需额外参数——用 addExternProperty，脚本侧 req.moves_size（无括号）
    int ItemMoveReq_MovesSize(const MMO::Proto::ItemMoveReq &msg)
    {
        return msg.moves_size();
    }

    // 索引访问需要额外的 index 参数——只能用 addExtern，脚本侧 req.moves(i)（必须带括号，
    // 因为这里的括号是"传参数"的语法，不是"复杂类型"的标记）
    const MMO::Proto::ItemMove &ItemMoveReq_Moves(const MMO::Proto::ItemMoveReq &msg, int index)
    {
        return msg.moves(index);
    }
} // namespace

// 注册（在同文件的 Register<ProtoFileName>ProtoBindings 内）：
das::addExternProperty<DAS_BIND_FUN(ItemMoveReq_MovesSize)>(mod, lib, ".`moves_size", "ItemMoveReq_MovesSize")->args({"msg"});
das::addExtern<DAS_BIND_FUN(ItemMoveReq_Moves)>(mod, lib, "moves", das::SideEffects::none)->args({"msg", "index"});
```
> **注意**：`moves_size` 属性的注册名是 `".\`moves_size"`（有 `.`` 前缀——见 §2.3 的规则），而 `moves` 是 `addExtern`（不是 `addExternProperty`），不需要 `.`` 前缀，因为它提供的是函数调用语法 `req.moves(i)` 而非属性访问语法。

`ItemMove`（repeated 元素类型）本身按普通消息类型在同一个 `.gen.cpp` 内注册一份 `ManagedStructureAnnotation`（除非它被其它 `.proto` 文件的消息引用，此时按依赖顺序移到被引用最早的公共依赖文件里，通常是定义该类型的那个 `.proto` 文件本身）。如果 `ItemMove` 本身只是数组元素、不是直接来自网络包顶层解析出的对象（它的生命周期跟随 `req.moves_size()`/`req.moves(i)` 调用时机，本质上仍然依附于外层 `req` 的生命周期），同样需要覆写 `canCopy`/`canClone` 为 `false`，与外层 `*Req` 类型一致。

**`map<K,V>` 字段**：生成器识别后直接跳过并输出 `// TODO: 字段 X 是 protobuf map，暂不支持自动绑定，需手写`，不尝试自动生成——这是当前唯一保留人工介入的复杂类型。

### 5.2 `Src/World/ScriptDispatchRegistry.h` / `.cpp`（手写基础设施，非生成）

因为分发代码现在分散在多个 `.gen.cpp` 文件里，不能再用一个中心 `switch(msgID)` 语句——改为运行期注册表：每个 `.gen.cpp` 在加载期显式调用 `ScriptDispatchRegistry::Register(msgID, fn)`，`WorldServer::OnMessage` 只需要调用统一的 `ScriptDispatchRegistry::Dispatch(...)`，不感知消息具体来自哪个 `.proto` 文件。这个注册表是**手写的基础设施代码**，添加一次之后不会因为消息数量增长而改动，复用了项目里 `MessageDispatcher<T>`（`Src/Common/Network/MessageDispatcher.h`）同款的"定长数组 + msgID 做下标"设计，保持风格一致。

```cpp
/**
 * @file ScriptDispatchRegistry.h
 * @brief msgID → 脚本消息分发函数注册表
 *
 * 各 Src/World/AutoGen/<ProtoFileName>.gen.cpp 在初始化阶段调用 Register()
 * 注册自己的分发函数；WorldServer::OnMessage 统一调用 Dispatch()。
 * 与 Common/Network/MessageDispatcher 的定长数组设计保持一致，
 * 但分发目标是脚本而非 C++ Handler，故单独成表，不复用该类模板。
 */
#pragma once

#include <array>

#include "Common/Core/Types.h"

namespace MMO
{

    class WorldServer;

    /**
     * @brief 脚本分发函数签名——解析 protobuf + 转发到 daScript
     * @return 解析成功且已转发返回 true；解析失败返回 false
     */
    using ScriptDispatchFn = bool (*)(WorldServer &server, uint32 sessionID,
                                      const uint8 *body, size_t len);

    /**
     * @brief msgID → ScriptDispatchFn 定长表，O(1) 查表
     */
    class ScriptDispatchRegistry
    {
    public:
        /**
         * @brief 注册一个 msgID 的分发函数——由各 *.gen.cpp 在启动期调用
         */
        static void Register(uint32 msgID, ScriptDispatchFn fn);

        /**
         * @brief 按 msgID 查表并分发
         * @return 找到对应分发函数且分发成功返回 true；未注册该 msgID 返回 false
         */
        static bool Dispatch(WorldServer &server, uint32 sessionID, uint32 msgID,
                              const uint8 *body, size_t len);

    private:
        static std::array<ScriptDispatchFn, kMaxHandlers> &Table();
    };

} // namespace MMO
```

```cpp
/**
 * @file ScriptDispatchRegistry.cpp
 * @brief ScriptDispatchRegistry 实现
 */
#include "World/ScriptDispatchRegistry.h"

#include "Common/Log/Log.h"

namespace MMO
{

    std::array<ScriptDispatchFn, kMaxHandlers> &ScriptDispatchRegistry::Table()
    {
        static std::array<ScriptDispatchFn, kMaxHandlers> table{};
        return table;
    }

    void ScriptDispatchRegistry::Register(uint32 msgID, ScriptDispatchFn fn)
    {
        if (msgID >= kMaxHandlers)
        {
            Log::Error("ScriptDispatchRegistry: msgID {} exceeds kMaxHandlers", msgID);
            return;
        }
        Table()[msgID] = fn;
    }

    bool ScriptDispatchRegistry::Dispatch(WorldServer &server, uint32 sessionID, uint32 msgID,
                                            const uint8 *body, size_t len)
    {
        if (msgID >= kMaxHandlers || !Table()[msgID])
        {
            return false;
        }
        return Table()[msgID](server, sessionID, body, len);
    }

} // namespace MMO
```

`WorldServer::OnMessage`（`Src/World/WorldServer.cpp:404`）改为：

```cpp
void WorldServer::OnMessage(uint32 sessionID, WorldSession &ws, const LogicMessage &msg)
{
    (void)ws;

    if (ScriptDispatchRegistry::Dispatch(*this, sessionID, msg.msgID, msg.body.Data(), msg.body.Size()))
    {
        return;
    }

    auto dispatched = _dispatcher.Dispatch(sessionID, msg.msgID, msg.body.Data(), msg.body.Size());
    if (!dispatched)
    {
        Log::Debug("WorldServer: unhandled msgID={} from session={}", msg.msgID, sessionID);
    }
}
```

未注册进 `ScriptDispatchRegistry` 的消息（`kUserMsgIDStart` 以下的控制消息、尚未迁移的消息）自动落回既有 `_dispatcher`，与现有代码路径完全兼容，不需要额外的 feature flag。

`WorldServer` 仍需新增两个供各 `.gen.cpp` 调用的访问方法（手写，非生成）：

```cpp
// WorldServer.h 新增声明
das::Context     *GetScriptContext() const { return _scriptCtx.get(); }
das::SimFunction *GetDispatchMsgFunction() const { return _fnDispatchMsg; }
```

```cpp
// WorldServer.h 私有成员新增
das::SimFunction *_fnDispatchMsg = nullptr;  // 脚本 dispatch_msg() 函数缓存
```

`WorldServer::InitScriptEngine()`（`Src/World/WorldServer.cpp:648`）在既有 `_fnUpdate = _scriptCtx->findFunction("Update")` 之后追加：

```cpp
_fnDispatchMsg = _scriptCtx->findFunction("dispatch_msg");
if (_fnDispatchMsg)
{
    Log::Info("InitScriptEngine: dispatch_msg() cached");
    RegisterAllMsgDispatch();   // ← 来自 ProtoBindIndex.gen.h，填充 ScriptDispatchRegistry
}
```

### 5.3 `Src/World/AutoGen/ProtoBindIndex.gen.h` / `.gen.cpp`（汇总文件）

按 `.proto` 文件拆分后，`MassiveModule::BindFunctions()` 和 `WorldServer::InitScriptEngine()` 不应该因为"新增一个 `.proto` 文件"而需要手改调用列表。生成器额外产出一个小的汇总文件，列出当前依赖闭包内所有 `.proto` 文件对应的注册函数调用，按依赖顺序排好，同样把内部细节放进匿名命名空间。

```cpp
/**
 * @file ProtoBindIndex.gen.h
 * @brief 自动生成文件——汇总声明，供 MassiveModule.cpp / WorldServer.cpp 手写代码 include
 *
 * 生成工具: Tools/Script/GenMsgBindings.py
 * @warning 不要手动编辑
 */
#pragma once

namespace das
{
    class Module;
    class ModuleLibrary;
} // namespace das

namespace MMO
{

    /**
     * @brief 注册当前依赖闭包内全部 .proto 文件对应的 daScript 类型
     * @note 在 MassiveModule::BindFunctions() 内调用一次
     */
    void RegisterAllProtoMessageTypes(das::Module &mod, das::ModuleLibrary &lib);

    /**
     * @brief 注册当前依赖闭包内全部 .proto 文件对应的消息分发函数
     * @note 在 WorldServer::InitScriptEngine() 缓存 dispatch_msg 之后调用一次
     */
    void RegisterAllMsgDispatch();

} // namespace MMO
```

```cpp
/**
 * @file ProtoBindIndex.gen.cpp
 * @brief 自动生成文件——汇总调用各 <ProtoFileName>.gen.cpp 里定义的注册函数
 *
 * 生成工具: Tools/Script/GenMsgBindings.py
 * 来源: Src/Proto/ 下依赖闭包内的全部 .proto 文件 + MsgID.proto 枚举值
 * @warning 不要手动编辑——新增/删除 .proto 文件后重新生成本文件
 * @note 调用顺序已按依赖关系排好（被引用类型所在文件先注册）；
 *       EMsgID 枚举绑定在此文件产出，替代手写的 MsgIDConstants.das。
 */
#include "World/AutoGen/ProtoBindIndex.gen.h"

#include <daScript/daScriptModule.h>
#include <daScript/ast/ast.h>
#include <daScript/simulate/simulate.h>
#include <daScript/simulate/bind_enum.h>
#include <MsgID.pb.h>

namespace MMO
{
    // ── 各 .gen.cpp 对外开放的注册函数（声明，供本文件调用）──
    extern void RegisterCommonProtoBindings(das::Module &mod, das::ModuleLibrary &lib);
    extern void RegisterLoginProtoBindings(das::Module &mod, das::ModuleLibrary &lib);
    extern void RegisterMoveProtoBindings(das::Module &mod, das::ModuleLibrary &lib);

    extern void RegisterLoginMsgDispatch();
    extern void RegisterMoveMsgDispatch();
} // namespace MMO

namespace
{
    using namespace das;

    // EMsgID 枚举绑定——替代手写的 MsgIDConstants.das
    struct EnumerationEMsgID : das::Enumeration
    {
        EnumerationEMsgID() : das::Enumeration("EMsgID")
        {
            external = true;
            cppName = "MMO::Proto::EMsgID";
            baseType = das::Type::tInt;
            addI("MSG_NONE", 0, das::LineInfo());
            addI("MSG_HEARTBEAT_REQ", 1, das::LineInfo());
            addI("MSG_HEARTBEAT_RSP", 2, das::LineInfo());
            addI("MSG_LOGIN_AUTH_REQ", 3, das::LineInfo());
            addI("MSG_LOGIN_AUTH_RSP", 4, das::LineInfo());
            addI("MSG_LOGIN_ENTER_WORLD_REQ", 5, das::LineInfo());
            addI("MSG_LOGIN_ENTER_WORLD_RSP", 6, das::LineInfo());
            addI("MSG_MOVE_REQ", 7, das::LineInfo());
            addI("MSG_MOVE_RSP", 8, das::LineInfo());
            addI("MSG_ENTITY_SPAWN_NTF", 9, das::LineInfo());
            addI("MSG_ENTITY_UPDATE_NTF", 10, das::LineInfo());
            addI("MSG_ENTITY_DESPAWN_NTF", 11, das::LineInfo());
            addI("MSG_ENTITY_REPLICATE_NTF", 12, das::LineInfo());
        }
    };

} // namespace

namespace MMO
{

    void RegisterAllProtoMessageTypes(das::Module &mod, das::ModuleLibrary &lib)
    {
        RegisterCommonProtoBindings(mod, lib);   // 依赖类型先注册（Vector3 等）
        RegisterLoginProtoBindings(mod, lib);
        RegisterMoveProtoBindings(mod, lib);
        mod.addEnumeration(new EnumerationEMsgID());  // EMsgID 枚举绑定
    }

    void RegisterAllMsgDispatch()
    {
        RegisterLoginMsgDispatch();
        RegisterMoveMsgDispatch();
    }

} // namespace MMO

DAS_BIND_ENUM_CAST(MMO::Proto::EMsgID)
```
> **关于枚举值过滤**：生成器解析 `MsgID.proto` 时只提取业务消息枚举值，过滤掉 protobuf 编译器自动生成的哨兵值（名字包含 `SENTINEL` 或 `_MIN_` 或 `_MAX_`）。这些哨兵值不在 `.proto` 源文件中，只在 `MsgID.pb.h` 的编译产物里存在，所以解析源 `.proto` 天然过滤。
> **关于 `addAnnotation` vs `new`**：实际落地使用 `mod.addAnnotation(new XxxAnnotation(lib))` 风格（更简洁），而非文档早期示例中的 `das::make_smart<...>()` 风格，两者语义等价。

> **注意**：`RegisterXxxProtoBindings`/`RegisterXxxMsgDispatch` 这几个函数在各自的 `<ProtoFileName>.gen.cpp` 里定义在 `namespace MMO { }`（外部可见，供本文件链接期调用），在本汇总文件里用匿名命名空间内的前置声明转发调用——两者签名必须完全一致（`MMO::` 限定 + 匿名命名空间声明会分别解析到同一个外部符号），这是 C++ 里"跨翻译单元调用其它文件里 `namespace MMO` 下函数"的标准写法，不依赖头文件也能工作，但为了 IDE 可读性和避免手误，实际生成时建议由生成器额外产出一份不对外暴露的内部头文件（`ProtoBindIndex_internal.gen.h`，同样 `@warning 不要手动编辑`）来源集中放这些前置声明，而不是在 `.cpp` 里手写——这是生成器实现时的内部细节，不影响本文档描述的外部行为。

`MassiveModule::BindFunctions()` 只需 include 这一个头文件、调用一个函数，不随消息数量增长而改动：

```cpp
// Src/World/MassiveModule.cpp
#include "World/AutoGen/ProtoBindIndex.gen.h"

void MassiveModule::BindFunctions()
{
    g_massiveMod = this;

    ModuleLibrary lib(this);
    lib.addBuiltInModule();
    // ...既有 15 个 addExtern（EntityPosition / IsDead / CreateEntity 等，不变）...

    RegisterAllProtoMessageTypes(*this, lib);   // ← 新增，来自 ProtoBindIndex.gen.h
}
```

### 5.4 `Script/AutoGen/HandlerRegistry.das`（不按 .proto 文件拆分）

daScript 侧的 `HandlerRegistry.das` **保持单文件**，不做拆分。原因：

- `[msg_handler]` 注解类（`MsgHandlerAnnotation`）是共享的编译期逻辑，只能定义一次，拆分没有意义。
- 消息名 → msgID 映射表（`g_msg_name_to_id`）即使消息数量增长到几百条，也只是几百行的 `table` 字面量，不存在 C++ 那种"单个编译单元膨胀拖慢增量编译"的问题——daScript 编译速度对文件行数不敏感到那个量级，真正的瓶颈是 C++ 端每次改动重新编译整个 `.cpp` 翻译单元（含 protobuf 生成的重量级头文件），这才是本次按 `.proto` 文件拆分要解决的问题。
- 保持单文件也让 `g_expected_handler_count` 完整性校验逻辑保持简单——不需要跨多个 `.das` 文件汇总计数。

```das
// ════════════════════════════════════════════════════
// 自动生成 — GenMsgBindings.py
//   生成时间: 2026-07-28
//   来源: Src/Proto/*.proto 中的 *Req 消息 + MsgID.proto
// ════════════════════════════════════════════════════

options gen2
options indenting = 4

module HandlerRegistry

require daslib/ast
require daslib/ast_boost
require daslib/templates_boost

require massive

// ── 消息名 → msgID 编译期映射（apply() 内查表用）──
// EMsgID 枚举值由 C++ 侧 EnumerationEMsgID 绑定提供（ProtoBindIndex.gen.cpp），
// 不再依赖手写的 MsgIDConstants.das。
let g_msg_name_to_id : table<string; uint> <- {
    "HeartbeatReq" => uint(EMsgID.MSG_HEARTBEAT_REQ),
    "LoginAuthReq" => uint(EMsgID.MSG_LOGIN_AUTH_REQ),
    "LoginEnterWorldReq" => uint(EMsgID.MSG_LOGIN_ENTER_WORLD_REQ),
    "MoveReq" => uint(EMsgID.MSG_MOVE_REQ),
}

// ── 期望注册的 handler 数量（等于上表条目数）──
let g_expected_handler_count = 4

// ── 运行期分发表：msgID → handler function ──
// @@(...) { ... } 在此处没有捕获任何外部变量（$c/$v 都是编译期展开），
// 推断出的类型是 function<...>，不是 lambda<...>——两者不能混用赋值。
var g_handler_registry : table<uint; function<(sessionID : uint; msgPtr : void?) : void>>
var g_registered_count : int = 0

// ── [msg_handler] 注解 ──
[function_macro(name="msg_handler")]
class MsgHandlerAnnotation : AstFunctionAnnotation {
    def override apply(var func : FunctionPtr; var group : ModuleGroup;
                       args : AnnotationArgumentList; var errors : das_string) : bool {
        // 步骤 1: msg 参数必须是字符串（消息名，如 "MoveReq"）——
        //         daScript 语法上 msg=Xxx 的裸标识符会被解析成字符串本身，
        //         此处显式要求写成 msg="Xxx"，与语法行为保持一致。
        let msgArg = find_arg(args, "msg")
        if (!(msgArg is tString)) {
            errors := "[msg_handler] 需要 msg=\"<MessageName>\" 参数（如 msg=\"MoveReq\"）"
            return false
        }
        let msgName = msgArg as tString

        // 步骤 2: 消息名必须在生成器扫描出的表里——防止拼错
        var msgID : uint
        var found = false
        g_msg_name_to_id |> get(msgName) $(id) {
            msgID = id
            found = true
        }
        if (!found) {
            errors := "[msg_handler] 未知消息名 \"{msgName}\"——检查 .proto 是否存在对应 *Req 消息，或重跑 xmake 触发生成"
            return false
        }

        // 步骤 3: 函数签名至少两个参数 (sessionID: uint; req: <MessageName>)
        if (length(func.arguments) < 2) {
            errors := "[msg_handler(msg=\"{msgName}\")] 函数至少需要两个参数: (sessionID : uint; req : {msgName})"
            return false
        }
        if (func.arguments[0]._type.baseType != Type.tUInt || func.arguments[0]._type.fixedDim != 0) {
            errors := "[msg_handler(msg=\"{msgName}\")] 第一个参数必须是 sessionID : uint"
            return false
        }

        // 步骤 4: 第二个参数类型名必须严格等于消息名——按约定推导
        // MoveReq 等消息类型通过 ManagedStructureAnnotation 绑定，是 handled type
        // （baseType == Type.tHandle），类型名要从 annotation.name 取，不是 structType
        let msgType = func.arguments[1]._type
        if (msgType.baseType != Type.tHandle || msgType.annotation == null) {
            errors := "[msg_handler(msg=\"{msgName}\")] 第二个参数类型应为 {msgName}，实际不是消息类型"
            return false
        }
        if (msgType.annotation.name != msgName) {
            errors := "[msg_handler(msg=\"{msgName}\")] 第二个参数类型应为 {msgName}，实际是 {msgType.annotation.name}"
            return false
        }

        // 步骤 5: 注入注册代码——把 (msgID → 转发 block) 写进全局表
        // apply() 本身运行在编译期的宏 context 里（Program::makeMacroModule 为
        // thisModule 单独开了一个 macroContext），这里直接写 g_handler_registry[...] = ...
        // 和 g_registered_count += 1 只会改到宏 context 里的那份全局变量，运行时
        // _scriptCtx 是另一个独立 context，看到的仍是初值。所以必须用 qmacro_expr
        // 生成语句，塞进 [init] 函数体（通过 setup_call_list(isInit=true) 拿到），
        // 靠 daScript runtime 的 init-function 机制在 Context::simulate() 时真正执行。
        var initBlk <- setup_call_list("msg_handler`init", func.at, true, true)
        initBlk.list |> push(qmacro_expr(${
            g_handler_registry[$v(msgID)] = @@(sessionID : uint; msgPtr : void?) {
                let typedMsg = unsafe(reinterpret<$t(msgType)?> msgPtr)
                $c("_::{func.name}")(sessionID, *typedMsg)
            }
        }))
        initBlk.list |> push(qmacro_expr(${ g_registered_count += 1 }))
        func.flags.privateFunction = true
        return true
    }
}

// ── C++ 唯一调用入口 ──
[export]
def dispatch_msg(msgID : uint; sessionID : uint; msgPtr : void?) {
    g_handler_registry |> get(msgID) $(handler) {
        invoke(handler, sessionID, msgPtr)
    }
}

// ── 完整性校验：handler 数量必须等于生成器扫描出的期望数量 ──
[export]
def validate_handler_registry {
    if (g_registered_count != g_expected_handler_count) {
        panic("handler 完整性校验失败: 期望 {g_expected_handler_count} 个 [msg_handler]，实际注册 {g_registered_count} 个——检查 Handlers.das 是否有消息漏写或漏加注解")
    }
}
```

> **关于宏 context 陷阱**：这是落地验证中发现的一个隐蔽问题，值得专门强调。`[msg_handler].apply()` 执行时所在的**宏 context**（`thisModule->macroContext`，由 `makeMacroModule()` 创建）与服务器运行时 `_scriptCtx` 是**两个完全独立的 `das::Context` 实例**，各自的全局变量存储互不共享。如果 `apply()` 里直接写 `g_handler_registry` 的赋值语句（`g_handler_registry[...] = ...`），它只写入宏 context 的变量区；运行时 context 里 `g_handler_registry` 仍是空表。编译无 error，`validate_handler_registry()` 才在启动后 panic "expected 4, got 0"。正确做法是像本文步骤 5 那样，把赋值语句生成成 **`qmacro_expr` AST 节点、push 进 `[init]` 函数体**（`setup_call_list(isInit=true)`），利用 daScript runtime 的 init-function 机制在 `Context::restart()` 时对**运行时 context** 执行。同理，`g_registered_count += 1` 也必须走同样的 injection 路径，不能直接在 `apply()` 里写。

> **关于 string/repeated 追加参数的签名校验**：当前 `apply()` 只校验前两个参数（`sessionID`、消息对象），第三个及以后的参数（对应额外标量）不做类型强校验，交由普通类型检查（拼错类型 → `undeclared identifier` 编译错误）与集成测试兜底。这是当前方案唯一未做到编译期 100% 覆盖的角落，覆盖表见 §7。

## 6. 开发者视角——业务逻辑 `Script/Handlers.das`

```das
// Handlers.das — 消息处理业务逻辑
// 所属: WorldServer
// Phase 3 (23_ProtoScriptBinding.md): [msg_handler] 注解 + protobuf 消息直接绑定

options gen2
options indenting = 4

require AutoGen/HandlerRegistry public
require massive

// ── MoveReq ──

[msg_handler(msg="MoveReq")]
def handle_move(sessionID : uint; req : MoveReq)
{
    if (req.speed < 0.0 || req.speed > 50.0) {
        LogWarn("handle_move: invalid speed={req.speed} session={sessionID}")
        return
    }

    let fullEid = FindEntityBySession(sessionID)
    if (fullEid == uint64(0)) {
        return
    }

    // 避免局部变量持有 req.position：Vector3 是 handled type 非 POD，
    // `let pos = req.position` 需要 unsafe（isLocal()==false）。
    // 实战中直接内联字段访问：
    LogInfo("handle_move: seq={req.sequence} pos=({req.position.x},{req.position.y},{req.position.z})")
}

// ── HeartbeatReq —— 简单回包，脚本处理 ──

[msg_handler(msg="HeartbeatReq")]
def handle_heartbeat(sessionID : uint; req : HeartbeatReq)
{
    // 心跳仅记录，不需要业务逻辑
    LogInfo("handle_heartbeat: session={sessionID} client_time={req.client_time}")
}

// ── LoginAuthReq —— 含 string/bytes 字段验证 ──

[msg_handler(msg="LoginAuthReq")]
def handle_login_auth(sessionID : uint; req : LoginAuthReq)
{
    // username / password 都是 addExternProperty 注册的属性——不带括号，
    // 与标量字段语法完全一致
    let user = req.username
    let pw   = req.password
    LogInfo("handle_login_auth: session={sessionID} user={user}")
}

// ── LoginEnterWorldReq ──

[msg_handler(msg="LoginEnterWorldReq")]
def handle_login_enter_world(sessionID : uint; req : LoginEnterWorldReq)
{
    LogInfo("handle_login_enter_world: session={sessionID} nonce={req.nonce}")
}
```
> **关于 `let pos = req.position` 的避让**：`Vector3` 作为 `ManagedStructureAnnotation` 绑定的 handled type，`isLocal()==false`（非 POD 类型），直接声明局部变量 `let pos = req.position` 需要 `unsafe`。实战中由脚本作者自行选择：对于简单访问直接内联 `req.position.x`，对需要多次使用的场景在函数内手动 `let pos = unsafe(req.position)`（由 code review 把关该项目 unsafe 用法）。这不是生成器的问题——是 daScript 安全模型对非 trivial 类型的普遍约束，与本文档的绑定方案无关。

## 7. 编译期错误捕获覆盖表

| 错误场景 | 捕获方式 | 阶段 |
|---|---|---|
| 消息名拼错（`msg="MovRe"`） | `apply()` 查 `g_msg_name_to_id` 未命中 | 编译期 |
| 第二参数类型名拼错 | 普通类型引用解析——`undeclared identifier` | 编译期 |
| msg 与第二参数类型不匹配（`msg="MoveReq"` 但 `req: LoginAuthReq`） | `apply()` 比对 `msgType.annotation.name == msgName` | 编译期 |
| 参数数量不足（< 2） | `length(func.arguments) < 2` | 编译期 |
| 第一个参数不是 `sessionID: uint` | `func.arguments[0]._type.baseType != Type.tUInt \|\| func.arguments[0]._type.fixedDim != 0` | 编译期 |
| **`req` 被赋值给全局变量/结构体字段（生命周期逃逸）** | `XxxAnnotation::canCopy()`/`canClone()` 均为 `false` | **编译期（§2.4）** |
| handler 数量与生成器扫描数量不符（漏写/漏注解） | `validate_handler_registry()` 在 `Init()` 里 panic | 启动期 |
| 第三个及以后参数（额外标量参数）个数或顺序错误 | 无编译期校验——交由普通类型检查（拼错类型→`undeclared identifier`）兜底 | 编译期/集成测试 |
| `req` 通过 `unsafe(addr(req))` 故意绕过类型系统存指针 | 无自动防护——已越过项目 `unsafe` 使用规范的边界 | code review |
| handler 全部注册但无一条生效（宏 context vs 运行时 context 隔离） | 如果步骤 5 错误地直接在 `apply()` 里写赋值而非走 `initBlk.list`，`validate_handler_registry()` 在启动后 panic "expected N, got 0" | 启动期 |

## 8. 复杂类型规则总表

| 字段类型 | 生成方式 | 脚本侧语法 | 自动化程度 |
|---|---|---|---|
| 标量（int/float/bool/enum） | `addProperty` 直接绑定 protobuf getter | `req.speed`（无括号） | 全自动 |
| 嵌套 message（如 `position: Vector3`） | 嵌套类型单独注册 `ManagedStructureAnnotation`，按依赖顺序注册，外层 `addProperty` 直接返回引用 | `req.position`（无括号） | 全自动 |
| `string`/`bytes` | 生成自由函数 + `addExternProperty` 注册（不是 `addExtern`） | `req.username`（无括号，与标量字段语法一致） | 全自动 |
| `repeated <标量\|message>` 的长度 | 生成自由函数 + `addExternProperty` 注册 | `req.moves_size`（无括号） | 全自动 |
| `repeated <标量\|message>` 的索引访问 | 生成自由函数 + `addExtern` 注册（**唯一保留括号的情况**，因为需要传 index 参数，是语法结构决定的，不是复杂度决定的） | `req.moves(i)`（带括号） | 全自动 |
| `map<K,V>` | 生成器跳过，输出 `// TODO` 注释 | 不支持 | 人工介入（唯一例外） |

## 9. xmake 集成

```lua
-- Src/World/xmake.lua 新增
-- @note 用 on_load（而非 on_config）：add_files 的通配符在 target 描述解析阶段
--       就已经固化成 sourcebatch，on_config 执行时新落盘的 .gen.cpp 不会被
--       重新纳入编译——干净 checkout 后首次构建必然缺少 .gen.cpp 里的符号导致
--       链接失败。on_load 运行在 sourcebatch 收集之前，此时显式 target:add("files", ...)
--       才能让刚生成的文件进入本次构建。新增/删除含 *Req 的 .proto 文件后无需手动
--       操作——生成器重新产出 .gen.cpp 后，on_load 里的 os.files 通配符 + target:add
--       自动把新的文件集纳入本次编译。
rule("gen_msg_bindings")
    on_load(function (target)
        local protoDir   = path.join(os.projectdir(), "Src/Proto")
        local autogenDir = path.join(os.projectdir(), "Src/World/AutoGen")
        local genScript  = path.join(os.projectdir(), "Tools/Script/GenMsgBindings.py")

        local indexFile = path.join(autogenDir, "ProtoBindIndex.gen.cpp")
        local dirty = not os.isfile(indexFile)
        if not dirty then
            local indexMtime = os.mtime(indexFile)
            for _, f in ipairs(os.files(path.join(protoDir, "*.proto"))) do
                if os.mtime(f) > indexMtime then
                    dirty = true
                    break
                end
            end
        end
        if dirty then
            os.vrunv("python", {genScript, "--proto-dir", protoDir, "--cpp-out", autogenDir,
                                "--das-out", path.join(os.projectdir(), "Script/AutoGen")})
            cprint("${color.success}[msgbind] handler bindings 已更新")
        end

        for _, f in ipairs(os.files(path.join(autogenDir, "*.gen.cpp"))) do
            target:add("files", f)
        end
    end)

target("WorldServer")
    add_rules("gen_msg_bindings")
    -- 不再需要 add_files("AutoGen/*.gen.cpp")——on_load 里的 target:add 已负责
    add_deps("Proto")   -- 已有依赖，确保 MsgID.proto 先生成
    -- ...既有配置不变...
```

## 10. 编码规范对齐核对（对照 `.claude/CodingStandard.md`）

| 规范条目 | 落地情况 |
|---|---|
| §1.5 文件命名大驼峰 `.gen.h`/`.gen.cpp` | `Common.gen.cpp`/`Login.gen.cpp`/`Move.gen.cpp`/`ProtoBindIndex.gen.h`/`ProtoBindIndex.gen.cpp` ✓；手写基础设施 `ScriptDispatchRegistry.h`/`.cpp` 按普通文件命名（非 `.gen.`），符合"自动生成文件才用 `.gen.` 后缀"的隐含规则 ✓ |
| §1.2 `Id`→`ID` | `sessionID`/`msgID`/`entityID` 全部大写 ID ✓ |
| §1.6 目录结构 | 生成文件落 `Src/World/AutoGen/`、`Script/AutoGen/`，手写基础设施落 `Src/World/`（与 `WorldServer.h` 同级），生成器落 `Tools/Script/`，与既有 `Tools/Proto/GenMsgID.py` 同构 ✓ |
| §2.1 Allman 大括号 | C++ 生成文件及 `ScriptDispatchRegistry` 严格换行开括号 ✓ |
| §3.3 文件头注释 + `@warning 不要手动编辑` | 每个 `.gen.cpp`/`.gen.h` 均带；`ScriptDispatchRegistry` 作为手写文件按 §3.4 普通类注释规范 ✓ |
| §3.5「匿名命名空间中的类」注释规则 | 生成代码的匿名命名空间内类前加 `/** @brief */`，内部函数用 `//`（本文档示例遵循，见 §5.1/§5.3 代码块）✓ |
| §4.2 定宽整数类型 | 生成代码与 `ScriptDispatchRegistry` 全用 `uint32`（项目别名）/`uint32_t`（daScript ABI 边界处），未引入裸 `int`/`long` 的新用法 ✓ |
| §7.1 `_Req`/`_Rsp`/`_Ntf` 后缀 | 只扫描 `*Req` 生成 handler，符合协议消息命名规则 ✓ |
| §7.2 proto snake_case → C++ PascalCase | 直接复用 protobuf 生成的 accessor（`sequence()`/`position()`），未引入额外命名转换层 ✓ |
| §8.1/§8.2 DasLang 文件组织/命名 | `Handlers.das` 大驼峰、`require` 独占一行、导出函数大驼峰（`dispatch_msg`/`Init`/`validate_handler_registry` 为 `[export]` 入口，`handle_move` 等为内部私有函数）✓ |
| §5.2 日志用 `Log::`/桥接 `LogWarn`/`LogInfo` | 脚本侧走 `MassiveModule` 既有日志桥接函数；C++ 生成代码与 `ScriptDispatchRegistry` 走 `Log::Error` 等既有通道，不新增日志通道 ✓ |

## 11. 落地顺序

1. 写 `Tools/Script/GenMsgBindings.py`，先手工跑一次，对照 §5.1/§5.3 的模板核对产出的 `Common.gen.cpp`/`Login.gen.cpp`/`Move.gen.cpp`/`ProtoBindIndex.gen.{h,cpp}` 五个文件，重点检查每个生成的 `XxxAnnotation` 类是否都正确覆写了 `canCopy`/`canClone`（除 `Vector3` 这类值类型辅助类型外）。
2. 新增手写基础设施 `Src/World/ScriptDispatchRegistry.h`/`.cpp`（§5.2）。
3. `Src/World/MassiveModule.cpp` 的 `BindFunctions()` include `ProtoBindIndex.gen.h` 并调用 `RegisterAllProtoMessageTypes`。
4. `WorldServer.h`/`WorldServer.cpp` 新增 `GetScriptContext()`/`GetDispatchMsgFunction()`/`_fnDispatchMsg` 缓存逻辑，在缓存 `dispatch_msg` 后调用 `RegisterAllMsgDispatch()`。
5. `WorldServer::OnMessage` 接入 `ScriptDispatchRegistry::Dispatch`，未命中回落到既有 `_dispatcher`。
6. 写 `Script/Handlers.das`，先迁移 `MoveReq` 一条链路验证（现有 `Move.proto`/`MoveHandler` 已有基础设施，风险最低）。验证时补一条反例测试：故意在脚本里写 `g_last_move : MoveReq?` 全局变量并尝试 `g_last_move = req`，确认编译报错（验证 §2.4 的防护确实生效，不是文档空谈）。
7. 跑通后迁移 `LoginAuthReq`（含 string 字段验证，确认 `req.username`/`req.password` 无括号可用）。
8. `Src/World/xmake.lua` 接入 `gen_msg_bindings` 规则（使用 `on_load` + `target:add("files", ...)` 模式，无需手动 `xmake f -c`）。
9. 新增一个此前不存在的 `.proto` 文件（含 `*Req` 消息）验证"新文件被正确识别、汇总文件自动加入新的调用行"。
10. 如后续出现 `repeated` 消息（当前 `.proto` 文件尚无），按 §5.1 的 repeated 模板补充验证，重点确认 `_size` 属性（无括号）与索引访问（带括号）两种语法都按预期工作。验证 string/bytes 字段的 `.`` 属性名前缀机制是否被生成器正确应用。

## 12. 未决问题（留待审核/后续迭代）

1. **`canCopy`/`canClone` 覆写为 `false` 是否会影响脚本把 `req` 作为参数传递给同一调用链内的其它函数？** 按 daScript 引用语义，函数参数传递是按引用/指针传递，不触发 `canCopy`/`canClone`（那两者只在赋值/克隆操作符路径上被检查）。已通过实际运行验证不受影响——`[msg_handler]` 宏生成的转发代码 `$c("_::{func.name}")(sessionID, *typedMsg)` 正是把 `*typedMsg` 作为引用参数传给实际 handler 函数，编译通过、运行时正常。
2. **`map<K,V>` 字段**：当前项目 `.proto` 文件尚未出现该类型，本文档给出的规则（跳过 + TODO 注释）足够。真正出现需求时再详细设计手写绑定的模板。
3. **`HandlerRegistry.das` 中的 `require massive`**：第 16 行的 `require massive` 是因为 `EMsgID` 枚举值通过 `MassiveModule` 注册（`ProtoBindIndex.gen.cpp` -> `RegisterAllProtoMessageTypes` -> `MassiveModule::BindFunctions`），脚本侧需要通过 `massive` 模块的导出可见链访问到这些类型。该 require 由生成器自动产出，无需手写维护。
