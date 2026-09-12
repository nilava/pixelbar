# WiFi and the web page.
#
# Deliberately does not REQUIRES panel or ui: nothing here may touch the model.
# Commands cross to the render loop through a queue and nothing crosses back
# except a status struct. If someone later includes a model header in here,
# this line is what fails.
idf_component_register(
    SRCS "src/net.c" "src/creds.c" "src/captive_dns.c" "src/ota.c" "src/discovery.c" "src/ble.c" "src/auth.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_http_server esp_timer
    # lwip for the captive-portal DNS socket, nvs_flash for the credentials.
    PRIV_REQUIRES nvs_flash lwip app_update esp_app_format bt
)

# The page is gzipped at build time and linked into the image. There is no
# filesystem partition to put it in: partitions.csv already spends 3.5 MB of a
# 4 MB part on two OTA slots, and moving those would mean a wired reflash of
# anything already in the field.
set(WEB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html")
set(WEB_GZ "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_custom_command(
    OUTPUT "${WEB_GZ}"
    # -n drops the timestamp, so the same page produces the same bytes and the
    # build stays reproducible.
    COMMAND gzip -9 -n -c "${WEB_SRC}" > "${WEB_GZ}"
    DEPENDS "${WEB_SRC}"
    COMMENT "gzip web/index.html")
add_custom_target(webui_gz DEPENDS "${WEB_GZ}")
add_dependencies(${COMPONENT_LIB} webui_gz)
target_add_binary_data(${COMPONENT_LIB} "${WEB_GZ}" BINARY)
