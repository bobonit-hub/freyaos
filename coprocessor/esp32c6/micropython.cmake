add_library(usermod_freya_link INTERFACE)

target_sources(usermod_freya_link INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/freya_link.c
)

target_include_directories(usermod_freya_link INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usermod_freya_link INTERFACE
    idf::esp-tls
    idf::mbedtls
    idf::esp_netif
)

target_link_libraries(usermod INTERFACE usermod_freya_link)
