unpack_esp32 -- how fast an ESP32-S3 turns 4-bit weights into arithmetic.

BUILD AND FLASH WITH build.bat COMPORT   (defaults to COM27)

gguf.h, gguf_bits.c and gguf_dot.c here are COPIES, refreshed from ..\..\shared on every build so the
sketch always measures the same kernel the desktop, the Luckfox and the Teensy measured. Do not edit
them here -- edit the originals in shared\ or the numbers stop comparing.

PSRAM must be enabled in the board options or only the first half of the test runs. If your module is
quad rather than octal PSRAM, change PSRAM=opi to PSRAM=qspi in build.bat.
