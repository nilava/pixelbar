# The device side of ui::Ports.
#
# Not called `hal`: ESP-IDF has a component of that name already, and a second
# one shadows its headers so that mbedtls stops being able to find sha_types.h.
idf_component_register(
    SRCS "src/board.cpp"
    INCLUDE_DIRS "include"
    REQUIRES ui esp_driver_gpio nvs_flash esp_timer
    PRIV_REQUIRES main)
