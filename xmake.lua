
rule("dpdk_linker_flags")
    on_load(function (target)
        -- Call external tool to generate flags
        local output = os.iorunv("pkg-config", {"--static", "--libs", "libdpdk"})
        if output then
            target:add("ldflags", output:trim())
        end
    end)
    on_build(function (target)
        local includedir = os.iorunv("pkg-config", {"--cflags-only-I", "libdpdk"})
        if includedir then
            target:add("cflags", includedir:trim())
        end
    end)
rule_end()

add_rules("mode.debug", "mode.release")


add_includedirs("include")

add_requires("pkgconfig::libdpdk", {system = true, configs = {
    shared = false,
}})

set_languages("c++23")


target("client")
    set_kind("binary")
    add_files("client-src/*.cpp", "common/*.cpp")
    add_rules("dpdk_linker_flags")


target("server")
    set_kind("binary")
    add_files("server-src/*.cpp", "common/*.cpp")
    add_rules("dpdk_linker_flags")
