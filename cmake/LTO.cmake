# Enabling LTO significantly increases the time required for the linking phase because the compiler must perform complex global optimizations.

# Global Analysis: Allows the compiler to analyze the entire program at once rather than file-by-file.
# Cross-File Inlining: Enables functions to be inlined even if they are defined in a different source file.
# Dead Code Elimination: More effectively identifies and removes code that is never executed across the whole application.
# Performance & Size: Typically results in smaller binaries and faster execution speeds. 


function(target_enable_lto)
    set(oneValueArgs TARGET ENABLE)
    cmake_parse_arguments(
        LTO
        "${options}"
        "${oneValueArgs}"
        "${multiValueArgs}"
        ${ARGN})

    include(CheckIPOSupported)
    check_ipo_supported(RESULT result OUTPUT output)
    if(result)
        message(STATUS "IPO/LTO is supported: ${LTO_TARGET}")
        set_property(TARGET ${LTO_TARGET} PROPERTY INTERPROCEDURAL_OPTIMIZATION
                                                   ${LTO_ENABLE})
    else()
        message(WARNING "IPO/LTO is not supported: ${LTO_TARGET}")
    endif()
endfunction()
