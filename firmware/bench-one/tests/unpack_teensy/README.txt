unpack_teensy -- how fast a Teensy 4.1 turns 4-bit weights into arithmetic.

BUILD AND FLASH WITH build.bat, not with the IDE's compile button.

gguf.h, gguf_bits.c and gguf_dot.c in this folder are COPIES. build.bat refreshes them from
..\..\shared every time it runs, so the sketch always measures the same kernel the desktop and the
Luckfox measured. Do not edit them here -- edit the originals in shared\ or the numbers stop comparing.

Arduino compiles every .c in a sketch folder by itself, so no wrapper is needed; and because the
Teensy build recipe accepts no extra include path and copies sources to a temp directory anyway,
copying is the only way to keep one master.
