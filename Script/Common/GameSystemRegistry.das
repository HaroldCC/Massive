// Script/Common/GameSystemRegistry.das
//
// [game_system] 宏——低频决策系统（ECS_06 §5）
//
// 用法：
//   [game_system(interval = 1.0)]   // 每 1 秒调一次
//   def ai_tick(dt : float)
//   {
//       // 低频 AI 决策
//   }
//
// 铁律 2：脚本调用量 ∝ 事件数/低频决策，绝不 ∝ 实体数 × tick。
// interval 控制调用频率（错峰），默认 0 = 每帧。

options gen2
options indenting = 4

module GameSystemRegistry

require daslib.ast
require daslib.ast_boost
require daslib.macro_boost
require daslib.strings_boost
require daslib.templates_boost

require Common

// 系统注册表：name → (dt)
var g_SystemRegistry : table<string, function<(dt : float) : void>>

// 分频错峰：每个系统注册自己的调用间隔（秒）
struct SystemSpec
{
    interval : float  // 调用间隔（0 = 每帧）
    lastTime : float  // 上次调用时间
}

var g_SystemSpecs : table<string, SystemSpec>

[function_macro(name="game_system")]
class GameSystemAnnotation : AstFunctionAnnotation
{
    def override apply(var func : FunctionPtr, var group : ModuleGroup,
                       args : AnnotationArgumentList, var errors : das_string) : bool
    {
        // 参数: [game_system(interval = 1.0)] — 1 秒调一次
        var interval = 0.0
        let arg = find_arg(args, "interval")
        if (arg is tFloat)
        {
            interval = arg as tFloat
        }

        // 签名: (dt : float)
        if (length(func.arguments) < 1 || func.arguments[0]._type.baseType != Type.tFloat)
        {
            errors := "[game_system] 需要参数: (dt : float)"
            return false
        }

        let name = string(func.name)
        // interval 是宏期 var float——直接构造 ExprConstFloat 表达式嵌入
        let intervalExpr = new ExprConstFloat(at = func.at, value = interval)
        var initBlk <- setup_call_list("game_system`init", func.at, true, true)
        // 注意：qmacro_expr 要求块内单条语句——分两次 push
        initBlk.list |> push(qmacro_expr(${
            g_SystemRegistry[$v(name)] = @@(dt : float) {
                $c("_::{func.name}")(dt)
            }
        }))
        initBlk.list |> push(qmacro_expr(${
            g_SystemSpecs[$v(name)] = SystemSpec(interval = $e(intervalExpr), lastTime = 0.0)
        }))
        func.flags.privateFunction = true
        return true
    }
}

// C++ 每帧调用：错峰调度（now = 引擎累计时间，保证确定性）
[export]
def TickGameSystems(dt : float; now : float)
{
    for (name, spec in keys(g_SystemSpecs), values(g_SystemSpecs))
    {
        if (spec.interval <= 0.0 || now - spec.lastTime >= spec.interval)
        {
            g_SystemRegistry |> get(name)$(sys)
            {
                invoke(sys, dt)
            }
            spec.lastTime = now
        }
    }
}
