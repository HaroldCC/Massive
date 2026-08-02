// AotGen.cpp — 项目自建 AOT 生成宿主
//
// 为什么不能直接用 stock daslang 工具（详见 Docs/ScriptLayer_04b_AOT_SafeHost.md）：
//   stock 工具看不到项目 native 模块（Common/world），脚本 `require Common` 会
//   编译失败；即使勉强过，aotHash 也与运行端漂移，AOT 全部静默回退解释器。
//   本宿主在调 daslib aot() 之前，先把与运行端相同的模块集注册进全局环境，
//   于是 aot() 解析 require Common/world 成功，且 aotHash 与运行端一致。
//
// 用法:
//   AotGen <dasRoot> <entry.das> <out.das.cpp> [--cross-platform]
//   AotGen <dasRoot> -aot-file-list <list.txt> <outDir>
//
// 实现走 B2（复用 daslib/aot_cpp.das 的 aot()）：
//   编译 daslib/aot_cpp.das → 找 aot 函数 → evalWithCatch 驱动它，
//   daslib 自己拼完整 .cpp（含 prologue/各模块 aotRequire 发射的 include）。

#include "ScriptEngine/Module/DasCommonModule.h"
#include "World/DasModule/WorldScriptModule.h"

#include "daScript/ast/ast.h"
#include "daScript/daScript.h"
#include "daScript/daScriptModule.h"
#include "daScript/simulate/simulate.h"
#include "daScript/misc/das_common.h"
#include "daScript/simulate/fs_file_info.h"
#include "vecmath/dag_vecMathDecl.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

DECLARE_ALL_DEFAULT_MODULES;

namespace
{
    das::TextPrinter tout;

    bool AotOne(das::Context              *pctx,
                das::SimFunction          *aotFn,
                const std::string         &input,
                const std::string         &output,
                bool                       crossPlatform,
                das::CodeOfPolicies       &cop)
    {
        std::string inputStr = input;
        vec4f args[4];
        args[0] = das::cast<char *>::from((char *)inputStr.c_str());
        args[1] = das::cast<bool>::from(false);          // paranoid
        args[2] = das::cast<bool>::from(crossPlatform);
        args[3] = das::cast<das::CodeOfPolicies *>::from(&cop);
        pctx->restart();
        vec4f ret = pctx->evalWithCatch(aotFn, args);
        if (auto ex = pctx->getException())
        {
            tout << "aot exception: " << ex << " at " << pctx->exceptionAt.describe() << "\n";
            return false;
        }
        const char *resultStr = das::cast<char *>::to(ret);
        if (!resultStr || !resultStr[0])
        {
            tout << "aot returned empty result for " << input << "\n";
            return false;
        }
        // 确保输出目录存在（跨平台：Windows 无 mkdir -p，忽略已存在错误）
        auto slash = output.find_last_of("/\\");
        if (slash != std::string::npos)
        {
            std::string dir = output.substr(0, slash);
#ifdef _WIN32
            std::string mkdir = "if not exist \"" + dir + "\" mkdir \"" + dir + "\"";
#else
            std::string mkdir = "mkdir -p \"" + dir + "\"";
#endif
            std::system(mkdir.c_str()); // NOLINT: 构建期工具，目录创建
        }
        FILE *fp = std::fopen(output.c_str(), "wb");
        if (!fp)
        {
            std::fprintf(stderr, "AotGen: cannot open %s\n", output.c_str());
            return false;
        }
        std::fputs(resultStr, fp);
        std::fclose(fp);
        return true;
    }

    void PrintUsage()
    {
        tout << "usage: AotGen <dasRoot> <entry.das> <out.das.cpp> [--cross-platform]\n";
        tout << "       AotGen <dasRoot> -aot-file-list <list.txt> <outDir> [--cross-platform]\n";
        tout << "  list.txt: 每行 `<entry.das>\\t<relative-out>`（相对 outDir）\n";
    }
} // namespace

namespace
{
    /**
     * @brief 在模块实例存活的作用域内执行 AOT 生成。
     * @return 0=成功, 1=失败, -1=用法错误
     */
    int RunAot(int argc, char **argv, const std::string &dasRoot)
    {
        // ── 与运行端相同的模块集（DasLangEngine::BuildModuleGroup 的同源路径）──
        auto commonModule = std::make_unique<MMO::DasCommonModule>();
        commonModule->Build();
        auto worldModule = std::make_unique<MMO::WorldScriptModule>();
        worldModule->Build();
        das::Module::Initialize();

        // daslib aot() 驱动
        auto access = das::make_smart<das::FsFileAccess>();
        static_cast<das::FsFileAccess *>(access.get())->introduceDaslib();

        das::ModuleGroup dummyGroup;
        das::CodeOfPolicies stubPolicies;
        stubPolicies.version_2_syntax = true;
        stubPolicies.aot_module       = true;

        std::string aotCppPath = dasRoot + "/daslib/aot_cpp.das";
        auto program = das::compileDaScript(aotCppPath, access, tout, dummyGroup, stubPolicies);
        if (!program || program->failed())
        {
            tout << "failed to compile daslib/aot_cpp.das\n";
            return -1;
        }
        auto pctx = das::SimulateWithErrReport(program, tout);
        if (!pctx)
        {
            return -1;
        }
        bool isUnique = false;
        auto aotFn = pctx->findFunction("aot", isUnique);
        if (!aotFn)
        {
            tout << "daslib aot() not found\n";
            return -1;
        }

        // policy：与运行端一致（除 aot 标志），aotHash 才能命中
        das::CodeOfPolicies cop;
        cop.aot          = false;
        cop.aot_module   = true;
        cop.aot_lib      = false;
        cop.ignore_shared_modules = false;
        cop.version_2_syntax      = true;

        bool crossPlatform = false;
        for (int i = 3; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--cross-platform")
            {
                crossPlatform = true;
            }
        }

        // 单文件模式: AotGen <dasRoot> <entry.das> <out.das.cpp>
        if (std::string(argv[2]) != "-aot-file-list")
        {
            bool ok = AotOne(pctx.get(), aotFn, argv[2], argv[3], crossPlatform, cop);
            return ok ? 0 : 1;
        }

        // 文件清单模式: AotGen <dasRoot> -aot-file-list <list.txt> <outDir>
        if (argc < 5)
        {
            PrintUsage();
            return -1;
        }
        std::string listPath = argv[3];
        std::string outDir   = argv[4];

        FILE *lfp = std::fopen(listPath.c_str(), "r");
        if (!lfp)
        {
            std::fprintf(stderr, "AotGen: cannot open %s\n", listPath.c_str());
            return -1;
        }
        char line[4096];
        bool allOk = true;
        while (std::fgets(line, sizeof(line), lfp))
        {
            // 每行: <entry.das>\t<relative-out>
            std::string s = line;
            if (!s.empty() && s.back() == '\n')
            {
                s.pop_back();
            }
            if (s.empty() || s[0] == '#')
            {
                continue;
            }
            auto tab = s.find('\t');
            if (tab == std::string::npos)
            {
                continue;
            }
            std::string entry = s.substr(0, tab);
            std::string rel   = s.substr(tab + 1);
            std::string out   = outDir + "/" + rel;
            // 确保输出目录存在（跨平台）
            auto slash = out.find_last_of("/\\");
            if (slash != std::string::npos)
            {
                std::string dir = out.substr(0, slash);
#ifdef _WIN32
                std::string mkdir = "if not exist \"" + dir + "\" mkdir \"" + dir + "\"";
#else
                std::string mkdir = "mkdir -p \"" + dir + "\"";
#endif
                std::system(mkdir.c_str()); // NOLINT: 构建期工具，目录创建
            }
            tout << "[aot] " << entry << " -> " << out << "\n";
            allOk = AotOne(pctx.get(), aotFn, entry, out, crossPlatform, cop) && allOk;
        }
        std::fclose(lfp);
        return allOk ? 0 : 1;
    }
} // namespace

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        PrintUsage();
        return -1;
    }

    std::string dasRoot = argv[1];
    das::setDasRoot(dasRoot);

    PULL_ALL_DEFAULT_MODULES;

    int result = RunAot(argc, argv, dasRoot);

    das::Module::Shutdown();
    return result;
}
