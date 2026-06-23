function(add_git_submodule relative_dir git_url)
    find_package(Git REQUIRED)

    set(FULL_DIR "${PROJECT_SOURCE_DIR}/${relative_dir}")

    # Check if the submodule is tracked by Git yet
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/.gitmodules")
        set(NEED_ADD TRUE)
    else()
        # Check if this specific submodule is registered in .gitmodules
        file(READ "${PROJECT_SOURCE_DIR}/.gitmodules" GITMODULES_CONTENT)
        if(NOT GITMODULES_CONTENT MATCHES "${relative_dir}")
            set(NEED_ADD TRUE)
        else()
            set(NEED_ADD FALSE)
        endif()
    endif()

    # Case A: Submodule isn't registered at all
    if(NEED_ADD)
        message(STATUS "Registering new submodule: ${relative_dir}")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} submodule add ${git_url} ${relative_dir}
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            RESULT_VARIABLE GIT_RESULT
        )
    # Case B: Submodule is registered but files aren't downloaded yet
    else()
        file(GLOB SUBMODULE_FILES "${FULL_DIR}/*")
        list(LENGTH SUBMODULE_FILES SUBMODULE_COUNT)

        if(SUBMODULE_COUNT EQUAL 0)
            message(STATUS "Updating existing submodule: ${relative_dir}")
            execute_process(
                COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive -- "${relative_dir}"
                WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                RESULT_VARIABLE GIT_RESULT
            )
        endif()
    endif()
    
    if(DEFINED GIT_RESULT AND NOT GIT_RESULT EQUAL 0)
        message(FATAL_ERROR "Git submodule operation failed for ${relative_dir}")
    endif()
endfunction()

if(USE_GIT_SUBMODULES)
    message(STATUS "Using Git SubModules...")
    # Pass the repository URL as the second argument
    add_git_submodule("external/liburing" "https://github.com")
endif()
