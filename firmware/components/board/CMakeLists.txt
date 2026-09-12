# The device side of ui::Ports.
#
# Not called `hal`: ESP-IDF has a component of that name already, and a second
# one shadows its headers so that mbedtls stops being able to find sha_types.h.
idf_component_register(
    SRCS "src/board.cpp"
    INCLUDE_DIRS "include"
    REQUIRES ui esp_driver_gpio nvs_flash esp_timer
    # hal and soc for gpio_ll_get_level, which the encoder ISR needs inlined:
    # see the comment on encoder_isr.
    PRIV_REQUIRES hal soc
    PRIV_REQUIRES main)
