#pragma once

#include "Common/Core/Types.h"
#include "Common/Log/Log.h"
#include "DasEngineConfig.h"
#include "DasFileWatcher.h"
#include "DasImage.h"
#include "ScriptEngine/IDasHost.h"
#include "ScriptEngine/ScriptDispatchRegistry.h"
// ⚠ 以下 das 头是 API 必需（CallScriptFunctionInR 模板用 das::Context/SimFunction/vec4f 完整类型做
// cast），非头泄漏。消费者（WorldServer/Bridge/生成代码）本就直接依赖 das 类型。
#include "daScript/ast/ast.h"
#include "daScript/simulate/simulate.h"
#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace MMO
{
    class IDasLangModuleProvider;
    class DasCommonModule;

    class DasLangEngine
    {
    public:
        static DasLangEngine &GetIns();

        /**
         * @brief 显式注册引擎实例（注册表单例）
         *
         * 由宿主（WorldServer）持有引擎实例并在 Initialize 前注册，
         * 替代 Meyers static 局部单例——初始化顺序可控、可测试（可注入 mock）。
         * 未注册时 GetIns() 返回默认实例（兼容旧路径）。
         */
        static void SetInstance(DasLangEngine &engine);

        bool Initialize(const DasLangEngineConfig &cfg,
                        IDasLangHost              *host,
                        IDasLangModuleProvider    *moduleProvider);
        void Shutdown();

        bool Load(const std::string &entryFile);

        void Tick(float dt);

        /**
         * @brief 重载补丁
         * @param dasbinPath 补丁脚本二进制文件
         */
        void RequestReload(const std::string &dasbinPath);

        /**
         * @brief 从脚本源码重载
         */
        void RequestReloadFromSource();

        das::Context *GetScriptContext() const
        {
            return _scriptImage.ctx.get();
        }

        das::SimFunction *GetDispatchFunc() const
        {
            return _scriptImage.funcDispatchMsg;
        }

        const std::string &GetLastErrors() const
        {
            return _scriptImage.errors;
        }

        /**
         * @brief 获取脚本宿主（Bridge_* 实体操作/事件总线访问用）
         */
        IDasLangHost *GetScriptHost() const
        {
            return _scriptHost;
        }

        static das::CodeOfPolicies MakePolicies(EScriptMode mode, bool forAotGen);

        ScriptDispatchRegistry &DispatchRegistry()
        {
            return _scriptDispatcher;
        }

        /**
         * @brief 调用脚本函数（带 arity 校验 + 异常捕获）
         *
         * daslang 的 evalWithCatch 不做 arity 校验（simulate_exceptions.cpp 直接
         * callWithCopyOnReturn），参数个数不匹配会静默错栈、无编译期报错。
         * 此 helper 在调用前断言 fn->debugInfo->count 与传入参数个数一致，
         * 签名漂移时立即报错而非静默损坏栈。
         *
         * @tparam Args 参数类型（das::cast<T>::from 可转换）
         * @param fn     已缓存的 SimFunction
         * @param args   参数
         * @param what   调用点描述（日志用）
         * @return 是否成功（无异常）
         */
        template <typename... Args>
        bool CallScriptFunction(das::SimFunction *fn, const char *what, Args &&...args)
        {
            return CallScriptFunctionIn(_scriptImage.ctx.get(), fn, what, std::forward<Args>(args)...);
        }

        /**
         * @brief 在指定 Context 上调用脚本函数（新 Image swap 前用）
         */
        template <typename... Args>
        static bool
        CallScriptFunctionIn(das::Context *ctx, das::SimFunction *fn, const char *what, Args &&...args)
        {
            return CallScriptFunctionInR<vec4f>(ctx, fn, what, std::forward<Args>(args)...).second;
        }

        /**
         * @brief 调用脚本函数并取返回值（pair<返回值, 是否成功>）
         *
         * 与 CallScriptFunctionIn 同语义，但额外返回脚本函数的结果值。
         * 用于生成代码里需要脚本 bool 返回值（如 DispatchMsg 是否命中 handler）的场景。
         *
         * ⚠ 线程环境绑定：脚本函数由 LogicThread 执行（Tick/TickGameSystems/Dispatch...），
         * 而 daScript 环境只在主线程 Initialize 时绑定（thread-local）。跨线程执行脚本时，
         * 任何触发 handle 类型 annotation 解析的操作（如字符串插值 {eid} walk 一个
         * ManagedValueAnnotation 值）都会调 TypeInfo::getAnnotation() →
         * Module::resolveAnnotation() → daScriptEnvironment::getBound()，此处为 null 即
         * DAS_VERIFYF 崩溃（官方 dasAudio data_callback 同款 exchangeBound 范式）。
         * 本 helper 在执行 evalWithCatch 前临时绑定已保存的环境（guard 析构恢复）。
         */
        template <typename Ret = vec4f, typename... Args>
        static std::pair<Ret, bool>
        CallScriptFunctionInR(das::Context *ctx, das::SimFunction *fn, const char *what, Args &&...args)
        {
            if (nullptr == fn || nullptr == ctx)
            {
                return {Ret {}, false};
            }
            constexpr size_t kArgCount = sizeof...(Args);
            if (nullptr != fn->debugInfo && fn->debugInfo->count != kArgCount)
            {
                Log::Error("DasEngine: arity mismatch calling {}: script expects {} args, C++ passes {}",
                           what,
                           fn->debugInfo->count,
                           kArgCount);
                return {Ret {}, false};
            }

            std::array<vec4f, sizeof...(Args)> callArgs {das::cast<std::decay_t<Args>>::from(args)...};

            // LogicThread 执行脚本需临时绑定主线程保存的 daScript 环境。
            // 未注册引擎（GetIns() 返回默认实例，_env 为 null）时跳过——兼容测试/无环境路径。
            // RAII guard：即使 evalWithCatch 内部抛 C++ 异常也保证环境恢复。
            auto             *env  = GetIns()._env;
            das::Context     *pCtx = ctx;
            das::SimFunction *pFn  = fn;
            vec4f             ret  = v_zero();
            if (env)
            {
                das::daScriptEnvironmentGuard guard(env);
                ret = pCtx->evalWithCatch(pFn, kArgCount > 0 ? callArgs.data() : nullptr);
            }
            else
            {
                ret = pCtx->evalWithCatch(pFn, kArgCount > 0 ? callArgs.data() : nullptr);
            }

            if (auto ex = pCtx->getException())
            {
                Log::Error("DasEngine: {} exception:{}", what, ex);
                return {Ret {}, false};
            }
            if constexpr (std::is_same_v<Ret, vec4f>)
            {
                return {ret, true};
            }
            else
            {
                return {das::cast<Ret>::to(ret), true};
            }
        }

        DasLangEngine()                                 = default;
        ~DasLangEngine()                                = default;
        DasLangEngine(const DasLangEngine &)            = delete;
        DasLangEngine &operator=(const DasLangEngine &) = delete;

    private:
        DasLangImage Compile(const std::string &enteryFile, const std::string &dasbinPath);

        /**
         * @brief 在绑定已初始化 daScript 环境的作用域内执行编译
         *
         * Module::Initialize() 只把 g_modulesInitialized=true 写到【主线程】的
         * thread-local daScriptEnvironment；而 PollReload 在 LogicThread 执行，
         * 那里 getBound() 为空/未初始化 → compileDaScript 的 DAS_ASSERTF 失败。
         * 此 helper 在主线程保存的环境上建 guard，临时绑定到当前线程。
         */
        template <typename Fn>
        auto WithDasEnvironment(Fn &&fn)
        {
            das::daScriptEnvironmentGuard guard(_env);
            return fn();
        }

        bool                     SimulateImage(DasLangImage &img);
        void                     RebindFunctions(DasLangImage &img);
        void                     LogAotCoverage(const DasLangImage &img) const;
        void                     DoSwap(DasLangImage &&img);
        void                     PollReload();
        std::vector<std::string> CollectDependencyFiles() const;

        void BuildModuleGroup(DasLangImage &img);

    private:
        DasLangEngineConfig       _cfg;
        IDasLangHost             *_scriptHost     = nullptr;
        IDasLangModuleProvider   *_moduleProvider = nullptr;
        das::daScriptEnvironment *_env            = nullptr; // Initialize 时保存的已初始化环境
        DasLangImage              _scriptImage;
        std::string               _entryFile;

        std::atomic<bool> _reloadPending {false};
        std::string       _pendingDasbin;
        std::mutex        _reloadMutex;

        std::unique_ptr<DasFileWatcher> _scriptFileWatcher;
        uint64                          _lastGCHeapSize {0};
        bool                            _initialized = false;

        ScriptDispatchRegistry _scriptDispatcher;
    };
} // namespace MMO