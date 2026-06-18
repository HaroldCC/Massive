--- @file xmake.lua
--- @brief Common — 共享库聚合入口
--- 本身不构建实体，仅 includes 子 target

includes("Core/xmake.lua")
includes("DB/xmake.lua")
includes("Network/xmake.lua")
includes("Queue/xmake.lua")
includes("Crypto/xmake.lua")
includes("ECS/xmake.lua")
includes("Timer/xmake.lua")
includes("Log/xmake.lua")
