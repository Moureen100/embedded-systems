idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash driver esp_driver_uart fatfs sdmmc esp_wifi esp_netif esp_event)