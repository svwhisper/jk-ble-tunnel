idf_component_register(
    SRCS "jk_proto.c"
    INCLUDE_DIRS "include"
    # `common` carries jk_ble_defs.h, shared with the tunnel headers.
    REQUIRES common
)

# Master safety gate. Keep at 0 until the write-frame format and login
# handshake are ported and bench-verified (spec O-2/O-5). Setting it to 1
# without the port is a hard #error, by design.
# Balance-parameter writes: ENABLED 2026-08-30 after the settings-register map
# was decoded from the official app's own frames (regs 0x06/0x13/0x1F, no auth
# on the wire — captured live on JK-PB2A16S20P fw 19.31). The §10 whitelist +
# range clamp + one-key-per-command + readback pipeline gates every write.
target_compile_definitions(${COMPONENT_LIB} PUBLIC JK_ENABLE_WRITES=1)
