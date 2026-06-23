# --- FMT Support via CPM ---
set(NS3_WITH_FMT "" CACHE PATH "Build with FMT support")
set(NS3_FMT "OFF" CACHE INTERNAL "ON if FMT is successfully found or built")

# Ensure the parent library name is available when this subdirectory is
# processed outside of the `build_lib` function scope.
set(BLIB_LIBNAME lxechoserver)

# Tracks if the target exists
if(TARGET fmt::fmt)
    set(NS3_FMT "ON" CACHE INTERNAL "ON if FMT is successfully found or built")
else()
    message(STATUS "Skipping examples requiring FMT")
endif()

set(EXAMPLES_COMMON_LIBS

    # ${lxechoserver}
    ${libcore}
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    fmt::fmt
)

macro(build_custom_library_example name source)
    build_lib_example(
        NAME custom_ns3_code
        SOURCE_FILES Ns3Code.cpp
        LIBRARIES_TO_LINK ${EXAMPLES_COMMON_LIBS}
    )

    if(TARGET custom_ns3_code AND TARGET fmt::fmt)
        # Use plain signature to match previous plain-style uses in build_exec
        target_link_libraries(custom_ns3_code fmt::fmt)
    endif()
endmacro()

# --- Build Examples ---
build_custom_library_example(Ns3Code Ns3Code.cpp)
