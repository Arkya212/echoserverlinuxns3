# On "Single-Config" generators (like Makefiles or Ninja), if you don't specify a type, CMake defaults to an empty string, which usually means no optimization and no debug symbols.
# If you just run cmake .., it automatically sets the project to Debug mode so you get helpful info if the program crashes.

# It checks if your source directory (where your code lives) is the same as your binary directory (where the compiled files go).
# If they are the same, CMake generates build artifacts (like .o files, CMakeFiles/ folders, and Makefile) directly inside your source folders. This creates a mess that is hard to clean.

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
    message(
        FATAL_ERROR
            "In-source builds not allowed. Please make a build directory.")
endif()

if(NOT CMAKE_BUILD_TYPE)
    message(STATUS "No build type selected, default to Debug")
    set(CMAKE_BUILD_TYPE "Debug")
endif()
