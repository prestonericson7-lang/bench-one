/* ===========================================================================================
 *  tl_plat.h -- everything the model core needs from a board, and nothing else
 * ===========================================================================================
 *
 *  The core (tl_core.c) runs a real GGUF transformer with the whole model STREAMED from storage and its
 *  working memory in an external store that is NOT memory mapped. Two boards implement this:
 *
 *    tests/psram_llm Teensy 4.1: the model on the built-in microSD (SdFat, SDIO), working memory in
 *                     the bit-banged PSRAM banks (driver copied verbatim from tests/psram_worker).
 *    tests/tl_host.c  the PC: weights from the same .gguf file with fread, the "PSRAM" a 48 MB
 *                     malloc. It exists to prove the core computes exactly what model_q.c does.
 *
 *  THE PSRAM IS NOT ADDRESSABLE MEMORY. Every byte goes through plat_ps_read / plat_ps_write. The
 *  address space is virtual and contiguous: the board concatenates whatever banks qualified, fastest
 *  first, and splits a transfer that crosses a bank boundary itself.
 *
 *  All functions return 0 on success, nonzero on failure. None of them may allocate.
 * ======================================================================================== */
#ifndef TL_PLAT_H
#define TL_PLAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read n bytes of the model file starting at absolute byte offset off. */
int      plat_sd_read(uint64_t off, void *dst, uint32_t n);

/* The same read, while doing arithmetic: work(ctx) runs one unit (one weight row) and returns nonzero while
 * units remain. A board that can move the card's bytes without the CPU (the Teensy: SDIO DMA) calls work
 * during the transfer, as many units as fit; a board that cannot may call it before the read, or not at all.
 * The core runs whatever units are left afterwards, so the result never depends on how many ran here.
 * work may be NULL. dst is one of the core's row buffers, placed so that the whole sectors around the read are
 * spare room in it: dst - (off % 512) is 32-aligned and writable, and so is everything up to the next multiple
 * of 512 past off + n. A board may therefore read [off - off % 512, that multiple) in one piece into
 * dst - (off % 512) -- the Teensy's DMA then makes one card command instead of a head, a middle and a tail. */
int      plat_sd_read_overlap(uint64_t off, void *dst, uint32_t n, int (*work)(void *), void *ctx);

/* Size of the model file in bytes. */
uint64_t plat_sd_size(void);

/* Usable PSRAM bytes, i.e. one past the highest valid virtual address. */
uint32_t plat_ps_size(void);

/* Transfer n bytes at virtual PSRAM address addr. addr + n <= plat_ps_size(). */
int      plat_ps_read (uint32_t addr, void *dst, uint32_t n);
int      plat_ps_write(uint32_t addr, const void *src, uint32_t n);

/* THE MODEL STAYS ON THE CARD; THE PSRAM IS ONLY THE MODEL'S WORKING MEMORY (its attention cache).
 * The tokenizer's lookup tables live in a second file on the card beside the model -- the Teensy's
 * "qwen3b.tok" -- built once from the model file and reused while it still matches it (the core checks).
 * plat_tok_open(0): open the existing store (nonzero if there is none); plat_tok_open(bytes): create it anew,
 * replacing any old one, with room for `bytes` in ONE CONTIGUOUS RUN on the card (the tables are read at
 * random, and a FAT32 seek in a fragmented file walks the cluster chain). plat_tok_read/_write: n bytes at
 * offset a, inside that room; plat_tok_flush: everything written is on the card. */
int      plat_tok_open (uint32_t bytes);
int      plat_tok_read (uint32_t a, void *dst, uint32_t n);
int      plat_tok_write(uint32_t a, const void *src, uint32_t n);
int      plat_tok_flush(void);

/* Monotonic seconds. */
double   plat_now(void);

/* Diagnostics, printf-style, one line per call (the board adds the newline). */
void     plat_log(const char *fmt, ...);

/* Generated text, exactly the bytes, no newline added. */
void     plat_emit(const char *s, int n);

#ifdef __cplusplus
}
#endif
#endif /* TL_PLAT_H */
