idf_component_register(
    SRCS "jk_proto.c"
    INCLUDE_DIRS "include"
    # `common` carries jk_ble_defs.h, shared with the tunnel headers.
    REQUIRES common
)

# Master safety gate. Keep at 0 until the write-frame format and login
# handshake are ported and bench-verified (spec O-2/O-5). Setting it to 1
# without the port is a hard #error, by design.
target_compile_definitions(${COMPONENT_LIB} PUBLIC JK_ENABLE_WRITES=0)
