/* ===========================================================================================
 *  tokenizer.h -- byte-level BPE, read out of the model file itself
 * ===========================================================================================
 *
 *  A runtime that cannot turn text into token ids cannot be fed a prompt, and one that cannot turn
 *  ids back into text cannot be checked by a human reading its output. Both halves are needed before
 *  any claim about tokens per second means anything, because a wrong tokenizer produces fluent
 *  nonsense at exactly the right speed.
 *
 *  The vocabulary and the merge list are already in the GGUF file -- 151,936 tokens and 151,387
 *  merges for this model -- so nothing is downloaded and nothing is hardcoded.
 *
 *
 *  WHAT BYTE-LEVEL MEANS AND WHY THE STRINGS LOOK WRONG
 *  -----------------------------------------------------
 *  Print the vocabulary and you get entries like "Ġthe" and "ĊĊ". Those are not typos and not a
 *  broken encoding. GPT-2 style tokenizers map all 256 byte values onto printable Unicode codepoints
 *  first, so that every token is a valid printable string and a space becomes U+0120. The mapping is
 *  a fixed permutation with no meaning of its own: printable ASCII maps to itself, and the 68 bytes
 *  that are not printable get pushed up to codepoints 256 and above.
 *
 *  So decoding is two steps, and skipping the second is the most common way to get output that is
 *  almost right: look up the token string, then map each codepoint back to the byte it stands for.
 *
 *
 *  THE PRE-TOKENIZER IS THE ONE THE FILE NAMES
 *  -------------------------------------------
 *  Before merging, the text is cut into chunks by a regex, and which regex is part of the model: the
 *  file names it in tokenizer.ggml.pre. For "qwen2" (every model in this machine) the cut is
 *  pretok_qwen2.h, that regex implemented directly with Unicode letter/number classes, and it is checked
 *  against llama.cpp on the same file. Special tokens (the file's control and user-defined types) are
 *  split out of the text first, wherever they appear, and each fragment between them is cut on its own.
 *
 *  Until 2026-09-27 every model was cut with GPT-2's rules instead, stated here as "identical on
 *  ASCII". It was not: 18,765 of 27,055 lines of this repository's own docs and code tokenized
 *  differently from llama.cpp (a punctuation run keeps its newline in Qwen2, any one symbol can lead a
 *  word). A differing split is still a valid tokenization -- decode(encode(s)) == s held throughout,
 *  which is why nothing looked wrong -- but it is not the one the model was trained on. Other values of
 *  tokenizer.ggml.pre still get the GPT-2 cut, chunk_len() in tokenizer.c.
 * ===========================================================================================
 */

#ifndef TOKENIZER_H
#define TOKENIZER_H

#include "gguf.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tok_map tok_map;   /* opaque hash table */

typedef struct {
    uint32_t   n_vocab;
    char     **piece;        /* byte-level-encoded token strings, owned by the gguf_t           */
    int32_t   *type;         /* GGUF token types; 3 means a control token like <|im_start|>      */
    uint32_t   n_merges;
    char     **merge;        /* "left right", owned by the gguf_t                                */
    tok_map   *vmap;         /* piece -> id                                                      */
    tok_map   *mmap_;        /* "left right" -> rank                                             */
    int32_t    bos, eos;
    int        pre_qwen2;    /* tokenizer.ggml.pre is "qwen2": chunks by pretok_qwen2.h           */
    /* The byte permutation, both directions. cp_byte is indexed by codepoint and 0xFFFF means
     * "not part of the mapping", which is how a corrupt vocabulary entry gets caught. */
    uint16_t   byte_cp[256];
    uint16_t   cp_byte[512];
} tokenizer_t;

/* Build from an open GGUF file. Borrows the string arrays, so the gguf_t must outlive this.
 * Returns 0 on success, and on failure writes the reason into g->err. */
int  tokenizer_init(tokenizer_t *tk, gguf_t *g);
void tokenizer_free(tokenizer_t *tk);

/* Encode UTF-8 text. Writes at most max ids, returns the count, or -1 if it would overflow.
 * Control tokens written literally in the text (for example "<|im_start|>") are matched whole. */
int tokenizer_encode(const tokenizer_t *tk, const char *text, int32_t *out, int max);

/* Append one token's bytes to a buffer. Returns bytes written. Not null-terminated by itself;
 * pass a buffer with room and terminate after the loop. */
int tokenizer_decode(const tokenizer_t *tk, int32_t id, char *out, int max);

/* decode(encode(s)) == s, byte for byte. Returns 0 if it holds. */
int tokenizer_roundtrip(const tokenizer_t *tk, const char *text);

#ifdef __cplusplus
}
#endif
#endif /* TOKENIZER_H */
