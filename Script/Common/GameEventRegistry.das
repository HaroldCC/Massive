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
require MacroCommon

// 事件分发表：type → handler(ev : void?)，宏注入类型化转换
var g_EventRegistry : table<uint16, function<(ev : void?) : void>>

[function_macro(name="game_event")]
class GameEventHandlerAnnotation : AstFunctionAnnotation
{
    def override apply(var func : FunctionPtr, var group : ModuleGroup,
                       args : AnnotationArgumentList, var errors : das_string) : bool
    {
        // 签名: (ev : <Type>Event)——第一个参数类型即事件类型
        if (!MacroCommon::CheckArgCount(func, 1))
        {
            errors := "[game_event] 需要至少一个参数: (ev : <事件类型>)"
            return false
        }

        // 取事件类型名（空串 = 非 handle）
        let evTypeName = MacroCommon::GetHandleArgType(func, 0)
        if (empty(evTypeName))
        {
            errors := "[game_event] 参数必须是事件类型（如 ev : EntityDamagedEvent）"
            return false
        }
        // evType：用于 $t() 类型 splice 的 TypeDecl（取原始第 0 参类型）
        let evType = func.arguments[0]._type

        // 查表（命名约定单一真相源）——EventTypeToID 由生成器从 GameEvent.proto 产出，
        //   -1 = 事件类型拼错或 proto 未定义（替代手写 EventTypeNameToEnum + 枚举遍历 typo 防护）。
        //   绑定在 world 模块（EventTypeRegistry.gen.cpp），宏 apply 直接调用。
        let id = EventTypeToID(evTypeName)
        if (id < 0)
        {
            errors := "[game_event] 事件类型 {evTypeName} 不在 GameEvent.proto——检查命名或重新生成绑定"
            return false
        }
        let evID = uint16(id)   // ⚠️ 只有 id >= 0 才到这里（哨兵 -1 已拦截）

        // 编译期查重——同一事件被二次注册 → 编译报错
        if (!MacroCommon::RegisterID("evt", id, string(func.name)))
        {
            errors := "[game_event] 事件 {evTypeName} 已被另一个 handler 注册——一个事件只能有一个脚本 handler"
            return false
        }

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
