idf_component_register(
    SRCS
        "app_main.c"
        "ctl_server.c"
        "emu_app.c"
        "emu_bms.c"
        "synth_frames.c"
    INCLUDE_DIRS "."
    REQUIRES
        common
        nvs_flash bt
        driver vfs
)
