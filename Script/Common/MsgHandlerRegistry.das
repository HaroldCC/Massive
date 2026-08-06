options gen2
options indenting = 4

module MsgHandlerRegistry

require daslib.ast
require daslib.ast_boost
require daslib.macro_boost
require daslib.strings_boost
require daslib.templates_boost // $c 模板名解析需要

require Common
require MacroCommon

var g_HandlerRegistry : table<uint, function<(sessionID : uint, msgPtr : void?) : void>>

[function_macro(name="msg_handler")]
class MsgHandlerAnnotation : AstFunctionAnnotation
{
    def override apply(var func : FunctionPtr, var group : ModuleGroup,
                       args : AnnotationArgumentList, var errors : das_string) : bool
    {
        // 步骤 1: 签名至少 (sessionID : uint; req : <Msg>)
        if (!MacroCommon::CheckArgCount(func, 2)) {
            errors := "[msg_handler] 函数至少需要两个参数: (sessionID : uint; req : <消息类型>)"
            return false
        }

        if (!MacroCommon::CheckFirstArgIsUInt(func)) {
            errors := "[msg_handler] 第一个参数必须是 sessionID : uint"
            return false
        }

        // 步骤 2: 第二个参数必须是消息类型（handled type），取类型名
        //   返回空串表示非 handle（类型名不可能为空）
        let msgTypeName = MacroCommon::GetHandleArgType(func, 1)
        if (empty(msgTypeName)) {
            errors := "[msg_handler] 第二个参数必须是消息类型（如 req : MoveReq），实际不是 handled type"
            return false
        }
        // msgType：用于 $t() 类型 splice 的 TypeDecl（取原始第 1 参类型）
        let msgType = func.arguments[1]._type

        // 步骤 3: 查表（命名约定单一真相源）——MsgTypeToID 由生成器从 proto 产出，
        //   -1 = 类型名拼错或 proto 未定义（替代手写 MsgNameToMsgID + 枚举遍历 typo 防护）。
        //   绑定在 Common 模块（MsgTypeRegistry.gen.cpp），宏 apply 直接调用。
        let id = MsgTypeToID(msgTypeName)
        if (id < 0) {
            errors := "[msg_handler] 消息类型 {msgTypeName} 不在 MsgID.proto——检查命名或重新生成绑定"
            return false
        }
        let msgID = uint(id)   // ⚠️ 只有 id >= 0 才到这里（哨兵 -1 已拦截）

        // 步骤 4: 编译期查重——同一消息 ID 被二次注册 → 编译报错（替代运行期静默覆盖）
        if (!MacroCommon::RegisterID("msg", id, string(func.name))) {
            errors := "[msg_handler] 消息 {msgTypeName} 已被另一个 handler 注册——一个消息只能有一个脚本 handler"
            return false
        }

        // 步骤 5: 注入注册代码到 [init] 块——运行期 context 才会执行
        //   apply() 在编译期 macroContext 运行，直接改全局只影响宏 context；
        //   必须用 qmacro_expr 生成语句塞进 [init]，才会在运行时 _scriptCtx 执行。
        var initBlk <- setup_call_list("msg_handler`init", func.at, true, true)
        initBlk.list |> push(qmacro_expr(${
            g_HandlerRegistry[$v(msgID)] = @@(sessionID : uint; msgPtr : void?) {
                let typedMsg = unsafe(reinterpret<$t(msgType)?> msgPtr)
                $c("_::{func.name}")(sessionID, *typedMsg)
            }
        }))
        func.flags.privateFunction = true
        return true
    }
}

[export]
def DispatchMsg(msgID : uint, sessionID : uint, msgPtr : void?) : bool
{
    var handled = false
    g_HandlerRegistry |> get(msgID)$(handler)
    {
        invoke(handler, sessionID, msgPtr)
        handled = true
    }
    return handled
}