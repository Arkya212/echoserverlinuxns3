# ---- linux echo server internal includes ----
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/tasks
)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# --- FMT Support via CPM ---
set(NS3_WITH_FMT "" CACHE PATH "Build with FMT support")
set(NS3_FMT "OFF" CACHE INTERNAL "ON if FMT is successfully found or built")

include(CPM)
cpmaddpackage("gh:fmtlib/fmt#11.2.0")

if(TARGET fmt::fmt)
    set(NS3_FMT "ON" CACHE INTERNAL "ON if FMT is successfully found or built")
else()
    message(STATUS "Skipping examples requiring FMT")
endif()

# ---- linux echo server module ----
build_lib(
    LIBNAME lxechoserver

    SOURCE_FILES

    # library public sources (keep module implementation separate from examples)
    empty.cc
    LIBRARIES_TO_LINK
    ${libcore}
    ${libnetwork}
    ${libinternet}
    ${libapplications}
    ${libstats}
    fmt::fmt
)
add_subdirectory(tasks)
