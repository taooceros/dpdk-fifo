add_rules("mode.debug", "mode.release")

add_includedirs("include")
add_requires("libdpdk", {system = true})


target("client")
    set_kind("binary")
    add_files("client-src/*.cpp")
    add_packages("libdpdk")


target("server")
    set_kind("binary")
    add_files("server-src/*.cpp")
    add_packages("libdpdk")
