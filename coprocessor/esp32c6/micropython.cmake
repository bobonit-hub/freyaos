add_library(usermod_freya_link INTERFACE)

target_sources(usermod_freya_link INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/freya_link.c
)

target_include_directories(usermod_freya_link INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${IDF_PATH}/components/esp-tls
)

# Add the extra ESP-IDF component to MicroPython's own component
# requirements. Linking idf::* targets through a usermod interface leaks
# generator-expression include paths into idf_component_register().
list(APPEND IDF_COMPONENTS esp-tls)

target_link_libraries(usermod INTERFACE usermod_freya_link)
