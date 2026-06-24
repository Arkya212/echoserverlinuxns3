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

    build_lib_example(
        NAME curl_simulation
        SOURCE_FILES curl-tap-emulation.cpp
        LIBRARIES_TO_LINK
            ${libnetwork}
            ${libtap-bridge}
            ${libcore}
            ${libpoint-to-point}
            ${libinternet}
            ${libapplications}
    )

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
    
    target_link_libraries(ns3_echo_server_example fmt::fmt)
    target_link_libraries(curl_simulation fmt::fmt)
else()
    message(STATUS "Skipping ns3_echo_server_example: fmt not found")
endif()
