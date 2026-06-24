--- @file fmt.lua
--- @brief fmt — C++ formatting library（内联编译）

target("fmt")
    set_kind("static")
    set_warnings("none")
    add_rules("Rules.ThirdParty")
    add_files("fmt/src/format.cc")
    add_sysincludedirs(path.join(os.projectdir(), "ThirdParty/fmt/include"), {public = true})
