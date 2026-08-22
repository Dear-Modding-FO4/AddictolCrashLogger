-- include subprojects
includes("lib/commonlibf4")

-- name and version
local plugin_name = "AddictolCrashLogger"
local plugin_version = "1.6.0"
local plugin_version_major, plugin_version_minor, plugin_version_patch = plugin_version:match("^(%d+)%.(%d+)%.(%d+)$")

-- set project constants
set_project(plugin_name)
set_version(plugin_version)
set_license("GPL-3.0")
set_languages("c++23")
set_toolchains("msvc")
set_warnings("allextra")

-- set policies
set_policy("build.optimization.lto", true)

-- add common rules
add_rules("mode.release", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- add options
set_config("commonlib_toml", true)
set_config("commonlib_xbyak", true)

-- add requires
add_requires("boost", {
    configs = {
        stacktrace = true
    }
})
add_requires("fmt")
add_requires("frozen")
add_requires("infoware", {
    configs = {
        d3d = true
    }
})
add_requires("magic_enum")
add_requires("rapidcsv")
add_requires("rsm-binary-io")
add_requires("zydis")

-- override runtime count
add_defines("COMMONLIB_RUNTIMECOUNT=3")

-- define targets
target(plugin_name)
    add_cxxflags("/permissive-", "/EHa", "/Zc:preprocessor", { public = true })

    -- add packages
    add_packages("boost")
    add_packages("fmt")
    add_packages("frozen")
    add_packages("infoware")
    add_packages("magic_enum")
    add_packages("rapidcsv")
    add_packages("rsm-binary-io")
    add_packages("zydis")

    -- add DIA SDK Includes
    add_includedirs(os.getenv("VSINSTALLDIR") .. "/DIA SDK/include")
    add_linkdirs(os.getenv("VSINSTALLDIR") .. "/DIA SDK/lib/amd64")
    add_links("diaguids", "ole32", "uuid")

    -- add dbghelp
    add_links("dbghelp")

    -- add winhttp
    add_links("winhttp")

    -- add vmaware
    -- add_includedirs("lib/vmaware/src")

    -- add commonlibsse plugin
    add_rules("commonlibf4.plugin", {
        name = plugin_name,
        author = "DearModdingFO4",
        description = "Addictol's Crash Logger"
    })

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

    -- pass name and version
    add_defines(
        'PLUGIN_NAME="' .. plugin_name .. '"',
        "PLUGIN_VERSION_MAJOR=" .. plugin_version_major,
        "PLUGIN_VERSION_MINOR=" .. plugin_version_minor,
        "PLUGIN_VERSION_PATCH=" .. plugin_version_patch
    )
