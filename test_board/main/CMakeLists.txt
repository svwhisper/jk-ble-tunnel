idf_component_register(
    SRCS
        "app_main.c"
        "ctl_server.c"
        "emu_app.c"
        "emu_bms.c"
        "synth_frames.c"
    INCLUDE_DIRS "."
    REQUIRES
        common secret net_util
        nvs_flash esp_wifi esp_event esp_netif esp_timer
        bt lwip
)
