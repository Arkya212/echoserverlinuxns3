# ---- linux echo server internal includes ----
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/tasks
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/external/cpp-httplib-0.14.3
)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ── FMT via CPM ──────────────────────────────────────────────────────────────
set(NS3_FMT "OFF" CACHE INTERNAL "ON if FMT is found or built")
include(CPM)
cpmaddpackage("gh:fmtlib/fmt#11.2.0")
CPMAddPackage(
  NAME benchmark
  GITHUB_REPOSITORY google/benchmark
  GIT_TAG v1.9.0
  GIT_SHALLOW TRUE
  OPTIONS
    "BENCHMARK_ENABLE_TESTING OFF"
    "BENCHMARK_ENABLE_GTEST_TESTS OFF" #Avoiding gTest builds for benchmark library.
)


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

# Linking external lib separately, not with ns3 libs
if(NS3_FMT)
    target_link_libraries(${libechoserverlinuxns3} PUBLIC fmt::fmt)
endif()

add_subdirectory(tasks)
# add_subdirectory(https)
add_subdirectory(src)
add_subdirectory(app)
