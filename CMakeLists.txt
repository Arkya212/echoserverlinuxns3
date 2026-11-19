# ---- linux echo server internal includes ----
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/tasks)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ── FMT via CPM ──────────────────────────────────────────────────────────────
set(NS3_FMT "OFF" CACHE INTERNAL "ON if FMT is found or built")
include(CPM)
cpmaddpackage("gh:fmtlib/fmt#11.2.0")

if(TARGET fmt::fmt)
    set(NS3_FMT "ON" CACHE INTERNAL "ON if FMT is found or built")
else()
    message(STATUS "Skipping examples requiring FMT")
endif()

# ── Module library ────────────────────────────────────────────────────────────
build_lib(
    LIBNAME echoserverlinuxns3

    SOURCE_FILES
        empty.cc

    HEADER_FILES
        empty.h

    LIBRARIES_TO_LINK
        ns3::core
        ns3::network
        ns3::internet
        ns3::applications
        ns3::stats
        fmt::fmt
)

# ── Examples in tasks/ ────────────────────────────────────────────────────────
add_subdirectory(tasks)