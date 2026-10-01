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

    # build_lib_example(
    #     NAME curl_simulation
    #     SOURCE_FILES curl-tap-emulation.cpp
    #     LIBRARIES_TO_LINK
    #         ${libnetwork}
    #         ${libtap-bridge}
    #         ${libcore}
    #         ${libpoint-to-point}
    #         ${libinternet}
    #         ${libapplications}
    # )

    # Real socket server (no root needed)
    build_lib_example(
        NAME curl_real_socket
        SOURCE_FILES curl-real-socket.cpp
        LIBRARIES_TO_LINK
            ${libcore}
            ${libpoint-to-point}
            ${libinternet}
            ${libapplications}
    )

    # Bridged socket: Real TCP socket → NS-3 node → Response
    build_lib_example(
        NAME curl_bridged_socket
        SOURCE_FILES curl-bridged-socket.cpp
        LIBRARIES_TO_LINK
            ${libcore}
            ${libpoint-to-point}
            ${libinternet}
            ${libapplications}
    )
    
    build_lib_example(
        NAME curl_qkd_bridge
        SOURCE_FILES curl-qkd-bridge.cpp
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
