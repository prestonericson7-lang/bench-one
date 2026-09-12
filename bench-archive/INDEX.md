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
