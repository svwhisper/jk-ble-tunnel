# Header-only component holding all local secrets in ONE git-ignored file,
# shared by node_a, node_b and test_board (every device joins the same AP).
# secret.h itself is git-ignored; only secret.h.example is tracked.
idf_component_register(INCLUDE_DIRS ".")
