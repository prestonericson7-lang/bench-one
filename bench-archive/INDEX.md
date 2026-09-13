# Bench archive

Every run's exact firmware image and full serial log, so any state the bench has
been in can be returned to by flashing one file. Newest last.

| stamp | sketch | git | log bytes | note |
|---|---|---|---|---|
| 20260912-022239 | psram_perfboard | 89b0acd+dirty | 5675 | harness verification |
| 20260912-023805 | psram_worker | 89b0acd+dirty | 283 | the Teensy stripped to a math engine; the Luckfox drives |
| 20260912-023844 | psram_worker | 89b0acd+dirty | 133 | the Teensy stripped to a math engine; the Luckfox drives |
| 20260912-024818 | psram_worker | 89b0acd+dirty | 133 | worker with the write-burst timer and the hex dump |
| 20260912-024836 | psram_worker | 89b0acd+dirty | 133 | worker with the write-burst timer and the hex dump |
| 20260912-025302 | psram_worker | 89b0acd+dirty | 133 | single-bit isolation test added |
| 20260912-025540 | psram_worker | 89b0acd+dirty | 133 | chip-select setup delay, sweepable from the host |
| 20260912-025858 | psram_worker | 89b0acd+dirty | 133 | single-bit read-only probe |
| 20260912-030050 | psram_worker | 89b0acd+dirty | 133 | per-bank tag, to see who answers a read |
| 20260912-030310 | psram_worker | 89b0acd+dirty | 133 | sweepable wait cycles and sample point |
| 20260912-030522 | psram_worker | 89b0acd+dirty | 133 | per-line bit error counts |
| 20260912-030845 | psram_worker | 89b0acd+dirty | 133 | timing table extended to 128 no-ops |
| 20260912-031151 | psram_worker | 89b0acd+dirty | 133 | canonical nibble loop restored |
| 20260912-031207 | psram_worker | 89b0acd+dirty | 133 | canonical nibble loop restored |
| 20260912-031413 | psram_worker | 89b0acd+dirty | 133 | switchable pad configuration on the data lines |
| 20260912-031646 | psram_worker | 89b0acd+dirty | 133 | single-bit read path for the external banks |
| 20260912-031843 | psram_worker | 89b0acd+dirty | 133 | full single-bit mode, both directions |
| 20260912-033143 | psram_worker | 4fe1332+dirty | 133 | 0x0B fast-read single-bit path, candidate optimisation |
| 20260912-033656 | psram_worker | 4fe1332+dirty | 133 | bus loops in ITCM so their timing stops depending on code layout |
| 20260912-034015 | psram_worker | 4fe1332+dirty | 133 | finer timing table, ITCM move reverted |
| 20260912-034712 | psram_worker | 4fe1332+dirty | 133 | diagnostic tail removed to restore the unrolled payload loop |
| 20260912-040339 | psram_worker | 9b66374+dirty | 133 | route-only bank selection, no reset per switch |
| 20260912-041041 | psram_worker | 9b66374+dirty | 133 | inter-burst refresh gap, sweepable |
| 20260912-073808 | psram_worker | 4c51aac+dirty | 133 | one-bit kernel: eight weights a byte |
| 20260912-074219 | psram_worker | 4c51aac+dirty | 133 | quad wait cycles sweepable to 96 |
| 20260912-075705 | psram_worker | 36f11b6+dirty | 133 | pad control fields fixed: hysteresis was never actually tested |
| 20260912-075722 | psram_worker | 36f11b6+dirty | 133 | pad control rebuilt from the core field macros |
| 20260912-080127 | psram_worker | 36f11b6+dirty | 133 | interrupts masked for the duration of every burst |
| 20260912-080354 | psram_worker | 36f11b6+dirty | 133 | interrupts off for the duration of a burst |
| 20260912-081340 | psram_worker | 36f11b6+dirty | 133 | batched 1-bit kernel: weights read once, scored many times |
| 20260912-081732 | psram_worker | 36f11b6+dirty | 133 | USB echo only when a host is listening; ports drained before masked bursts |
| 20260912-081830 | psram_worker | 36f11b6+dirty | 133 | batch cap 32 |
| 20260912-081953 | psram_worker | 36f11b6+dirty | 133 | USB echo guarded, no interrupt masking |
| 20260912-085159 | psram_worker | d753533+dirty | 133 | batch cap 64 |
| 20260912-095331 | psram_worker | 9900f25+dirty | 0 | USB accepted as a second command source |
| 20260912-102304 | psram_worker | fdb5088+dirty | 0 | chain firmware: non-blocking relay, leaf port, 4kB buffers |
| 20260913-003630 | psram_worker | 2a6917a+dirty | 275 | board returned on COM41; USB-driven run |
