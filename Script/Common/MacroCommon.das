// Script/Common/MacroCommon.das
//
// 三宏（[msg_handler]/[game_event]/[game_system]）共享工具层（ScriptLayer_06 §3.2）。
//
// 职责：
//   1. 编译期查重表——同一 ID/系统名被二次注册 → 编译报错（替代运行期静默覆盖）
//   2. handler 签名校验助手（参数个数、首参类型、handle 类型名提取）
//   3. 注册条目注入模板（EmitRegistration）
//
// 约束：
//   - 本模块被三个注册宏 require，所有助手必须 def public（独立模块 private 不跨模块）
//   - 目标模块一律 compiling_module() 动态取，不硬编码
//   - 查重表只被宏期代码引用；若运行期引用会进 AOT surface

module MacroCommon public
options gen2
options indenting = 4
options no_global_variables = false    // 宏期查重表需要（linq_fold 同款范式）

require daslib.ast
require daslib.ast_boost
require daslib.templates_boost
require daslib.strings_boost

// ── 编译期查重表 ──
// 键："msg:<ID>" / "evt:<ID>" / "sys:<name>" → handler 名
// ⚠️ 仅宏期读写；禁止运行期引用（否则进 AOT surface，remove_unused_symbols 才裁剪）
var private g_registered : table<string, string>

/**
 * @brief 注册 ID 并查重。重复注册返回 false（调用方报编译错）。
 * @param nameSpace   键空间（"msg" / "evt" / "sys"）
 * @param id          消息/事件枚举值（int64）
 * @param handlerName 当前 handler 名（用于报错提示）
 */
def public RegisterID(nameSpace : string, id : int64, handlerName : string) : bool
{
    let key = "{nameSpace}:{id}";
    if (key_exists(g_registered, key))
    {
        return false;
    }

    g_registered[key] = handlerName;
    return true;
}

/**
 * @brief 注册字符串键并查重（game_system 的系统名用）。重复返回 false。
 */
def public RegisterSys(nameSpace : string, name : string) : bool
{
    let key = "{nameSpace}:{name}";
    if (key_exists(g_registered, key))
    {
        return false;
    }

    g_registered[key] = name;
    return true;
}

/**
 * @brief 取 func 第 index 参的类型；校验是 handle（tHandle 且 annotation 非空）。
 * @return 类型名（如 "MoveReq"）；非 handle / 越界返回空串（类型名不可能为空）
 */
def public GetHandleArgType(func : FunctionPtr, index : int) : string
{
    if (length(func.arguments) <= index)
    {
        return "";
    }

    let t = func.arguments[index]._type
    if (t.baseType != Type.tHandle || t.annotation == null)
    {
        return "";
    }

    var s = string(t.annotation.name)
    return s
}

/**
 * @brief 校验 func 首参是 uint（sessionID 用）
 */
def public CheckFirstArgIsUInt(func : FunctionPtr) : bool
{
    if (length(func.arguments) < 1)
    {
        return false;
    }

    let t = func.arguments[0]._type
    return t.baseType == Type.tUInt && t.fixedDim == 0
}

/**
 * @brief 校验 func 首参是 float（game_system 的 dt 用）
 */
def public CheckFirstArgIsFloat(func : FunctionPtr) : bool
{
    if (length(func.arguments) < 1)
    {
        return false;
    }

    return func.arguments[0]._type.baseType == Type.tFloat
}

/**
 * @brief 校验 func 参数个数下限
 */
def public CheckArgCount(func : FunctionPtr, minArgs : int) : bool
{
    return length(func.arguments) >= minArgs
}

/**
 * @brief 注入一条「注册条目」到 init 块：regTable[keyExpr] = regExpr
 * @param initBlk  setup_call_list 得到的 init 块
 * @param regTable 目标注册表名（脚本侧全局，如 "g_HandlerRegistry"）
 * @param keyExpr  注册键表达式（ExpressionPtr，如 $v(msgID)）
 * @param regExpr  注册值表达式（ExpressionPtr，如匿名 lambda 或 @wrapper 地址）
 */
def public EmitRegistration(var initBlk : ExprBlock?, regTable : string,
                            keyExpr : ExpressionPtr, regExpr : ExpressionPtr) : bool
{
    if (initBlk == null || keyExpr == null || regExpr == null)
    {
        return false;
    }

    let assign = qmacro_expr(${
        $i(regTable)[$e(keyExpr)] = $e(regExpr)
    });
    initBlk.list |> push(assign);
    return true;
}
