add_rules("mode.debug", "mode.release")


add_includedirs("include")

add_requires("pkgconfig::libdpdk", {system = true})

set_languages("c++23")


target("client")
    set_kind("binary")
    add_files("client-src/*.cpp", "common/*.cpp")
    add_packages("pkgconfig::libdpdk")


target("server")
    set_kind("binary")
    add_files("server-src/*.cpp", "common/*.cpp")
    add_packages("pkgconfig::libdpdk")
