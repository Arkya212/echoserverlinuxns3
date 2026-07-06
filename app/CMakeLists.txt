find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

set(MY_EXECUTABLES
    "main"
)

# Keep your include paths defined here
set(LIBRARY_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_BINARY_DIR}/configured_files"
    "${CMAKE_CURRENT_SOURCE_DIR}/../external/cpp-httplib-0.14.3"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)

# foreach(EXE_NAME IN LISTS MY_EXECUTABLES)
#     set(APP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${EXE_NAME}.cpp")
#     add_executable(${EXE_NAME} ${APP_SOURCES})

#     # Link actual libraries using PRIVATE scope
#     target_link_libraries(
#         ${EXE_NAME}
#         PRIVATE 
#         fmt::fmt
#     )

#     # correct method to link header file dirs to an executable
#     target_include_directories(
#         ${EXE_NAME}
#         PRIVATE ${LIBRARY_INCLUDES}
#     )

#     if(ENABLE_WARNINGS)
#         target_set_warnings(
#             TARGET ${EXE_NAME}
#             ENABLE ${ENABLE_WARNINGS}
#             AS_ERRORS ${ENABLE_WARNINGS_AS_ERRORS}
#         )
#     endif()

#     if(ENABLE_LTO)
#         target_enable_lto(
#             TARGET ${EXE_NAME}
#             ENABLE ON
#         )
#     endif()
# endforeach()

# ── QKD bridge using cpp-httplib SSLServer (HTTPS) ──
if(OpenSSL_FOUND)
    build_lib_example(
        NAME curl_qkd_bridge_httplib
        SOURCE_FILES curl-qkd-bridge-httplib.cpp
        LIBRARIES_TO_LINK
            ${libechoserverlinuxns3}
            ${libqkdnetsim}
            ${libcore}
            ${libnetwork}
            ${libpoint-to-point}
            ${libinternet}
            ${libapplications}
            ${libmobility}
    )
    target_link_libraries(curl_qkd_bridge_httplib
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
    )
else()
    message(STATUS "Skipping curl_qkd_bridge_httplib: OpenSSL not found")
endif()
