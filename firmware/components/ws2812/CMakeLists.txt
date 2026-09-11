idf_component_register(
    SRCS "src/ws2812.c"
         "src/ws2812_spi.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_rmt esp_driver_spi esp_mm)
