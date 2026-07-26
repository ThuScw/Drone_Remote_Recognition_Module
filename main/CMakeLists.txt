idf_component_register(
    SRCS "flight_data.cpp" "main.cpp" "rid_messages.cpp" "ble_rid_broadcaster.cpp"
         "indicators.cpp" "flight_log.cpp"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash freertos esp_timer driver spi_flash wear_leveling
)
