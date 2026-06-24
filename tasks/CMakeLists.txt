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
    
    target_link_libraries(ns3_echo_server_example fmt::fmt)
else()
    message(STATUS "Skipping ns3_echo_server_example: fmt not found")
endif()
