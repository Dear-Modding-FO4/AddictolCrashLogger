-- set minimum xmake version
set_xmakever("2.8.2")

-- includes
includes("lib/commonlibf4")

-- set project
set_project("AddictolCrashLogger")
set_license("GPL-3.0")

-- project name
local name = "AddictolCrashLogger"

-- project version
local version = "1.3.0"
local major, minor, patch = version:match("^(%d+)%.(%d+)%.(%d+)$")
set_version(version)

-- set defaults
set_languages("c++23")
set_toolchains("msvc")
set_warnings("allextra")

-- set policies
set_policy("package.requires_lock", true)
set_policy("build.optimization.lto", true)

-- add rules
add_rules("mode.release", "mode.releasedbg", "mode.debug")
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

-- targets
target("AddictolCrashLogger")
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
    add_includedirs("lib/vmaware/src")

    -- add dependencies to target
    add_deps("commonlibf4")

    -- add commonlibsse plugin
    add_rules("commonlibf4.plugin", {
        name = name,
        author = "DearModdingFO4",
        description = "Addictol's Crash Logger",
        plugin_file_data = [[
#include <F4SE/F4SE.h>

F4SE_EXPORT constinit auto F4SEPlugin_Version = []() noexcept {
    F4SE::PluginVersionData v{};
    v.PluginVersion({ ${PLUGIN_VERSION_MAJOR}, ${PLUGIN_VERSION_MINOR}, ${PLUGIN_VERSION_PATCH}, 0 });
    v.PluginName("${PLUGIN_NAME}");
    v.AuthorName("${PLUGIN_AUTHOR}");
    v.UsesAddressLibrary(false);
    v.UsesSigScanning(false);
    v.IsLayoutDependent(false);
    v.HasNoStructUse(false);
    v.CompatibleVersions({});

    std::uint32_t addressFlags = 0;
    std::uint32_t structFlags  = 0;

    // NG Support
    addressFlags |= (1u << 1);
    structFlags  |= (1u << 1);

    // AE Support
    addressFlags |= (1u << 2);
    structFlags  |= (1u << 2);

    v.addressIndependence = addressFlags;
    v.structureIndependence = structFlags;

    return v;
}();
        ]]
    })

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

    -- pass name and version
    add_defines(
        'PLUGIN_NAME="' .. name .. '"',
        "PLUGIN_VERSION_MAJOR=" .. major,
        "PLUGIN_VERSION_MINOR=" .. minor,
        "PLUGIN_VERSION_PATCH=" .. patch
    )
