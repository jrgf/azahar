if (NINTENDO_SWITCH)
    add_library(network STATIC EXCLUDE_FROM_ALL
        artic_base/artic_base_client.h
        artic_base/artic_base_common.h
        network.h
        network_stub.cpp
        room.h
        room_member.h
        socket_manager.h
        verify_user.h
    )
else()
    add_library(network STATIC EXCLUDE_FROM_ALL
        announce_multiplayer_session.cpp
        announce_multiplayer_session.h
        artic_base/artic_base_client.cpp
        artic_base/artic_base_client.h
        artic_base/artic_base_common.h
        network.cpp
        network.h
        network_settings.cpp
        network_settings.h
        packet.cpp
        packet.h
        precompiled_headers.h
        room.cpp
        room.h
        room_member.cpp
        room_member.h
        socket_manager.cpp
        socket_manager.h
        verify_user.cpp
        verify_user.h
    )
endif()

create_target_directory_groups(network)

if (ENABLE_WEB_SERVICE AND NOT NINTENDO_SWITCH)
    target_link_libraries(network PRIVATE web_service)
endif()

if (NINTENDO_SWITCH)
    target_link_libraries(network PRIVATE citra_common)
else()
    target_link_libraries(network PRIVATE citra_common enet Boost::serialization httplib)
endif()

if (CITRA_USE_PRECOMPILED_HEADERS)
    target_precompile_headers(network PRIVATE precompiled_headers.h)
endif()
