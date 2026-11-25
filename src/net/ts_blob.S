// Встроенный Tailscale (сжатый gzip), см. tools/tsbridge/build.sh
    .section .rodata.sacoop_ts,"a"
    .balign 16
    .global sacoop_ts_blob
    .global sacoop_ts_blob_end
    .hidden sacoop_ts_blob
    .hidden sacoop_ts_blob_end
sacoop_ts_blob:
    .incbin "sacoopts.gz"
sacoop_ts_blob_end:
    .byte 0
