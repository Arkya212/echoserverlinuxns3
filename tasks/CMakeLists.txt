find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Build example executable from Ns3Code.cpp
if(TARGET fmt::fmt)
    build_lib_example(
        NAME ns3_echo_server_example
        SOURCE_FILES Ns3Code.cpp
        LIBRARIES_TO_LINK
            ${libechoserverlinuxns3}
            ${libcore}
            ${libpoint-to-point}
            ${libinternet}
            ${libapplications}
    )

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
    target_link_libraries(curl_qkd_bridge
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
    )

    # cpp-httplib standalone HTTP server example (header-only)
    build_lib_example(
        NAME cpp_httplib_server
        SOURCE_FILES ../app/server.cpp
        LIBRARIES_TO_LINK
            ${libcore}
    )

    build_lib_example(
        NAME tlsserver
        SOURCE_FILES ../app/tlsserver.cpp
        LIBRARIES_TO_LINK
            ${libcore}
    )

    target_link_libraries(ns3_echo_server_example fmt::fmt)
    # target_link_libraries(curl_simulation fmt::fmt)
    target_link_libraries(curl_bridged_socket fmt::fmt)
    target_link_libraries(curl_real_socket fmt::fmt)
    target_link_libraries(cpp_httplib_server Threads::Threads)
    target_link_libraries(tlsserver 
                    OpenSSL::SSL
                    OpenSSL::Crypto
                    Threads::Threads
    )
else()
    message(STATUS "Skipping ns3_echo_server_example: fmt not found")
endif()
