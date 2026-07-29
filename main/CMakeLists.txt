# Recursive source discovery across all subdirectories
file(GLOB_RECURSE MAIN_SRCS "${CMAKE_CURRENT_LIST_DIR}/*.cpp")

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "." "broadcast" "protocol" "broadcaster" "data" "indicators" "logging"
    REQUIRES bt nvs_flash freertos esp_timer driver spi_flash wear_levelling usb
)
