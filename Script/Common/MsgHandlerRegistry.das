options gen2
options indenting = 4

module MsgHandlerRegistry

require daslib.ast
require daslib.ast_boost
require daslib.macro_boost
require daslib.strings_boost
require daslib.templates_boost // $c 模板名解析需要

require Common

var g_HandlerRegistry : table<uint, function<(sessionID : uint, msgPtr : void?) : void>>

// 由消息体转换消息ID
def private MsgNameToMsgID(name : string) : string
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

    return "MSG_" + string(out);
}

def private IsUpper(c : int) : bool
{
    return c >= int('A') && c <= int('Z');
}

def private ToUpperChar(c : int) : uint8
{
    if (c >= int('a') && c <= int('z'))
    {
        return uint8(c - int('a') + int('A'));
    }

    return uint8(c);
}

[function_macro(name="msg_handler")]
class MsgHandlerAnnotation : AstFunctionAnnotation
{
    def override apply(var func : FunctionPtr, var group : ModuleGroup,
                       args : AnnotationArgumentList, var errors : das_string) : bool
    {
        // 步骤 1: 签名至少 (sessionID : uint; req : <Msg>)
        if (length(func.arguments) < 2) {
            errors := "[msg_handler] 函数至少需要两个参数: (sessionID : uint; req : <消息类型>)"
            return false
        }

        if (func.arguments[0]._type.baseType != Type.tUInt || func.arguments[0]._type.fixedDim != 0) {
            errors := "[msg_handler] 第一个参数必须是 sessionID : uint"
            return false
        }

        // 步骤 2: 第二个参数必须是消息类型（handled type），取类型名
        let msgType = func.arguments[1]._type
        if (msgType.baseType != Type.tHandle || msgType.annotation == null) {
            errors := "[msg_handler] 第二个参数必须是消息类型（如 req : MoveReq），实际不是 handled type"
            return false
        }
        let msgTypeName = string(msgType.annotation.name)   // "MoveReq"
        let msgIdName   = MsgNameToMsgID(msgTypeName)       // "MSG_MOVE_REQ"

        // 步骤 3: 定位 C++ 绑定的 EMsgID 枚举
        //   EMsgID 是全局共享 ID 值空间，绑定在 Common 模块（DasCommonModule），
        //   所有服务都 require Common。宏侧按约定从 Common 取。
        let cmod = find_compiling_module("Common")
        if (cmod == null) {
            errors := "[msg_handler] 未找到 Common 模块——handler 文件需 require Common"
            return false
        }
        let enu = module_find_enumeration(cmod, "EMsgID")
        if (enu == null) {
            errors := "[msg_handler] Common 模块未绑定 EMsgID 枚举"
            return false
        }

        // 步骤 4: typo 防护——必须先确认成员存在（find_enum_value 未命中返回 0，与 MSG_NONE=0 无法区分）
        var found = false
        for (e in enu.list) {
            if (e.name == msgIdName) {
                found = true
                break
            }
        }

        if (!found) {
            errors := "[msg_handler] 消息类型 {msgTypeName} 推导出的 {msgIdName} 不在 EMsgID 中——检查命名或 .proto 是否存在对应 *Req"
            return false
        }
        let msgID = uint(find_enum_value(enu, msgIdName))

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