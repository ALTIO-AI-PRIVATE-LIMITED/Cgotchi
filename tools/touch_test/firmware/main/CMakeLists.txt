idf_component_register(
    SRCS "touch_diag.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_timer esp_hw_support esp_system esp_lcd_jd9853 esp_lvgl_port lvgl nvs_flash
)
