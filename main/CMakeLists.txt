idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES bt nvs_flash esp_driver_gpio esp_driver_uart driver esp_http_client mbedtls fatfs wear_levelling json esp_https_ota app_update)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    API_ENDPOINT=\"$ENV{API_ENDPOINT}\"
    API_HEARTBEAT_ENDPOINT=\"$ENV{API_HEARTBEAT_ENDPOINT}\"
    API_DEFINITIONS_ENDPOINT=\"$ENV{API_DEFINITIONS_ENDPOINT}\"
)
