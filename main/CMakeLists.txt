idf_component_register(
    SRCS "main.cpp" "rid_messages.cpp" "ble_rid_broadcaster.cpp"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash freertos esp_timer driver
)
