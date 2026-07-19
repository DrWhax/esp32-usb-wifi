idf_component_register(SRCS "tusb_ncm_main.c" "console.c" "led.c"
                       INCLUDE_DIRS ""
                       PRIV_REQUIRES esp_wifi nvs_flash esp_timer
                       )
