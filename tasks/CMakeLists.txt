# ─────────────────────────────────────────────────────────────────────────────
# BLIB_LIBNAME must be set explicitly here.
# build_lib() is a CMake *function* — BLIB_LIBNAME it parses is scoped to
# that function and is NOT visible here. build_lib_example reads it from the
# *caller's* scope, so we inject it manually.
# ─────────────────────────────────────────────────────────────────────────────
set(BLIB_LIBNAME echoserverlinuxns3)

set(COMMON_LIBS_TO_LINK
    ns3::core
    ns3::point-to-point
    ns3::internet
    ns3::applications
    ${libechoserverlinuxns3}
    fmt::fmt
)

macro(build_echoserver_code name source)
    build_lib_example(
        NAME         ${name}
        SOURCE_FILES ${source}
        LIBRARIES_TO_LINK ${COMMON_LIBS_TO_LINK}
    )
endmacro()

build_echoserver_code(ns3_echo_server_example Ns3Code.cpp)
build_echoserver_code(curlrealsocket         curlrealsocket.cpp)