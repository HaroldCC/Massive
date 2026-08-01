#pragma once

#include "Common/Core/Types.h"
#include "daScript/ast/ast.h"
#include "daScript/simulate/debug_info.h"
#include <vector>

namespace MMO
{

    static constexpr uint32 kDasbinFormatVersion = 1;
    static constexpr uint32 kDasbinFlagEncrypted = 0x1;

    class DasLangSerializer
    {
    public:
        static bool Save(const std::string                                &outPath,
                         das::ProgramPtr                                   program,
                         das::ModuleGroup                                 &libGroup,
                         const std::vector<std::pair<std::string, int64>> &deps,
                         const std::string                                &keyHex);

        static das::ProgramPtr Load(const std::string &inPath,
                                    das::ModuleGroup  &libGroup,
                                    das::FileAccess   *fAccess,
                                    const std::string &keyHex,
                                    std::string       &outErrors);
    };

} // namespace MMO