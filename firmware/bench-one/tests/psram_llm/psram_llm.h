/* psram_llm.h -- types the sketch's functions take, in a header because the Arduino builder writes a
 * prototype for every function near the top of the .ino, before any type defined further down exists. */
#ifndef PSRAM_LLM_H
#define PSRAM_LLM_H
#include <stdint.h>

struct bank_t {
    uint8_t kind, y, mode, wi, ri, nb, su;   /* route, bus mode, write/read no-op index, burst, cs setup */
    uint8_t ewi, eri;                        /* the edge the sweep found; wi/ri run MARGIN steps slower */
    float   rate;                            /* MB/s read over the confirmation span                  */
    char    name[6];
};
#endif
