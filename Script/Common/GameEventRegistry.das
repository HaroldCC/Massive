// Script/Common/GameEventRegistry.das
//
// [game_event] 宏——C++ 游戏事件 → daslang 类型化分发（ECS_06 决策 3）
//
// 用法：
//   [game_event]
//   def EntityDamaged(ev : EntityDamagedEvent)
//   {
//       LogInfo("entity {ev.target_id} damaged by {ev.attacker_id} for {ev.damage}")
//   }
//
// 宏从参数类型名推导事件枚举 ID（EntityDamagedEvent → GAME_EVENT_ENTITY_DAMAGED），
// 编译期 typo 防护（枚举不存在 → 编译报错），运行期零分配（payload 指针 reinterpret）。

options gen2
options indenting = 4

module GameEventRegistry

require daslib.ast
require daslib.ast_boost
require daslib.macro_boost
require daslib.strings_boost
require daslib.templates_boost

require Common
require world     // WorldScriptModule：EGameEventType 枚举 + 各 *Event 类型 + GameEventEnvelope

// 事件分发表：type → handler(ev : void?)，宏注入类型化转换
var g_EventRegistry : table<uint16, function<(ev : void?) : void>>

// ── 名称转换：EntityDamagedEvent → GAME_EVENT_ENTITY_DAMAGED ──

def private ToUpperChar(c : int) : uint8
{
    if (c >= int('a') && c <= int('z'))
    {
        return uint8(c - int('a') + int('A'));
    }
    return uint8(c);
}

def private IsUpper(c : int) : bool
{
    return c >= int('A') && c <= int('Z');
}

// EntityDamagedEvent → ENTITY_DAMAGED（驼峰 → 下划线大写）
def private EventNameToSnake(name : string) : string
{
    var out : array<uint8>;
    let n = length(name);
    for (i in range(n))
    {
        let c = character_at(name, i)
        if (i > 0 && IsUpper(c))
        {
            let prev = character_at(name, i - 1);
            var insert = false;
            if (!IsUpper(prev))
            {
                insert = true;
            }
            elif (i + 1 < n && !IsUpper(character_at(name, i + 1)))
            {
                insert = true;
            }
            if (insert)
            {
                out |> push(uint8('_'));
            }
        }
        out |> push(ToUpperChar(c));
    }
    return string(out);
}

// EntityDamagedEvent → GAME_EVENT_ENTITY_DAMAGED
def private EventTypeNameToEnum(typeName : string) : string
{
    // 去 "Event" 后缀
    var short = typeName
    if (length(typeName) >= 5 && typeName |> slice(length(typeName) - 5) == "Event")
    {
        short = typeName |> slice(0, length(typeName) - 5)
    }
    return "GAME_EVENT_" + EventNameToSnake(short)
}

[function_macro(name="game_event")]
class GameEventHandlerAnnotation : AstFunctionAnnotation
{
    def override apply(var func : FunctionPtr, var group : ModuleGroup,
                       args : AnnotationArgumentList, var errors : das_string) : bool
    {
        // 签名: (ev : <Type>Event)——第一个参数类型即事件类型
        if (length(func.arguments) < 1)
        {
            errors := "[game_event] 需要至少一个参数: (ev : <事件类型>)"
            return false
        }

        let evType = func.arguments[0]._type
        if (evType.baseType != Type.tHandle || evType.annotation == null)
        {
            errors := "[game_event] 参数必须是事件类型（如 ev : EntityDamagedEvent）"
            return false
        }

        // 从类型名推导枚举名
        let evTypeName = string(evType.annotation.name)   // "EntityDamagedEvent"
        let enumName   = EventTypeNameToEnum(evTypeName)  // "GAME_EVENT_ENTITY_DAMAGED"

        // 查 world 模块的 EGameEventType 枚举
        let cmod = find_compiling_module("world")
        if (cmod == null)
        {
            errors := "[game_event] 未找到 world 模块——handler 文件需 require world"
            return false
        }
        let enu = module_find_enumeration(cmod, "EGameEventType")
        if (enu == null)
        {
            errors := "[game_event] world 模块未绑定 EGameEventType 枚举"
            return false
        }

        // typo 防护：确认枚举成员存在
        var found = false
        for (e in enu.list)
        {
            if (e.name == enumName)
            {
                found = true
                break
            }
        }
        if (!found)
        {
            errors := "[game_event] 事件类型 {evTypeName} 推导出的 {enumName} 不在 EGameEventType 中——检查命名或 GameEvent.proto"
            return false
        }
        let evID = uint16(find_enum_value(enu, enumName))

        // 注入注册代码到 [init]：运行期从 void? payload reinterpret 出类型化事件再调用 handler
        var initBlk <- setup_call_list("game_event`init", func.at, true, true)
        initBlk.list |> push(qmacro_expr(${
            g_EventRegistry[$v(evID)] = @@(ev : void?) {
                let typed = unsafe(reinterpret<$t(evType)?> ev)
                $c("_::{func.name}")(*typed)
            }
        }))
        func.flags.privateFunction = true
        return true
    }
}

// C++ 每帧调用：事件逐个派发（避免 array<GameEventEnvelope> 非 trivial 问题）
// C++ 侧 for (env : bus.Drain()) 逐个调本函数
[export]
def DispatchGameEvent(evType : uint16; payload : void?)
{
    g_EventRegistry |> get(evType)$(handler)
    {
        // payload 指向序列化后的事件数据，宏内部 reinterpret 成具体事件类型
        invoke(handler, payload)
    }
}
