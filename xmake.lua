
rule("dpdk")
    before_build(function (target)
        -- Call external tool to generate flags
        local output = os.iorunv("pkg-config", {"--static", "--libs", "libdpdk"})
        if output then
            target:set("ldflags",  output:trim())
        end
        
        local cflags = os.iorunv("pkg-config", {"--cflags", "libdpdk"})
        if cflags then
            target:add("cxxflags", cflags:trim(), {force = true}) 
            target:add("cflags", cflags:trim(), {force = true}) 
        end
    end)

rule_end()


add_requires("argparse")

add_rules("mode.debug", "mode.release", "mode.releasedbg")


add_cflags("-g")
add_cxxflags("-g")


add_includedirs("include")

add_requires("pkgconfig::libdpdk", {system = true, configs = {
    shared = false,
}})

set_languages("c++23")

if is_arch("arm64") then
    add_rules("dpdk")
else
    set_policy("build.sanitizer.address", true)
    add_packages("pkgconfig::libdpdk")
end


target("client")
    set_kind("binary")
    add_files("client-src/*.cpp", "common/*.cpp")
    add_packages("argparse")

target("server")
    set_kind("binary")
    add_files("server-src/*.cpp", "common/*.cpp")
    add_packages("argparse")