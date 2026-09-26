set(IDF_TARGET esp32c6)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/sdkconfig.riscv
    boards/sdkconfig.c6
    boards/sdkconfig.ble
    ${CMAKE_CURRENT_LIST_DIR}/../sdkconfig.micropython
    ${CMAKE_CURRENT_LIST_DIR}/../sdkconfig.tls13
)
