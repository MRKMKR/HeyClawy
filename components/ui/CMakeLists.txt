idf_component_register(
    SRCS "ui.c" "ui_tasks.c" "lv_font_hebrew_28.c" "lv_font_hebrew_36.c"
    INCLUDE_DIRS "include"
    REQUIRES board openclaw lvgl settings
    PRIV_REQUIRES esp_timer esp_lvgl_port
)
