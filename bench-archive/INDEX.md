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
| 20260913-003956 | psram_worker | b483a9e+dirty | 275 | reflash after the board stopped answering mid-soak |
| 20260913-004134 | psram_worker | b483a9e+dirty | 277 | USB echo off by default; it wedged the board twice |
| 20260913-005234 | psram_worker | 2d8366b+dirty | 277 | timing table extended down to zero no-ops |
| 20260927-023139 | psram_llm | adbea54+dirty | 4316 | first run: 8 chips found, 6 banks (48 MiB) proven, SD 18.06 MB/s; HALTED at tl_open: string 87272 too long (CS0 at the edge setting write 0 read 13) |
| 20260927-023701 | psram_llm | adbea54+dirty | 11934 | run 2 (margin + self-check): 7 banks 56 MiB, SD 18.07 MB/s, France: all 21 tokens = PC reference, max logit delta 0.111, 127.7 s/pass, 0 PSRAM corrections |
| 20260927-032432 | psram_llm | adbea54+dirty | 1011 | suite flash: v3 overnight: bench x3 + code_prime, arith, chat, france x3 each |
| 20260927-033629 | psram_llm | adbea54+dirty | 797 | suite flash: v3b overnight (SD init retried + named): bench x3 + code_prime, arith, chat, france x3 |
| 20260927-034136 | psram_llm | adbea54+dirty | 1011 | suite flash: v3c overnight: extended ACMD41 recovery + ::stop; bench x3 + code_prime, arith, chat, france x3 |
| 20260927-032432 | psram_llm-suite | adbea54+dirty | 4246 | suite v3: 8 banks 64 MB proven; HALTED: SD begin failed (card hung by the mid-read reflash) |
| 20260927-033629 | psram_llm-suite | adbea54+dirty | 5016 | suite v3b: SdFat error 0x17 (ACMD41) x8, card hung |
| 20260927-034136 | psram_llm-suite | adbea54+dirty | 7020 | suite v3c: 0x17 x8; extended ACMD41 instrument flawed (stale CMDRSP0), removed; needs USB power cycle |
| 20260927-040009 | psram_llm | adbea54+dirty | 5102 | v4 flash (tl_prefill batched prompt, 8 positions per pass; bench buffers borrow the core arena); card still hung, expect SD halt until USB power cycle |
| 20260927-060317 | psram_llm | adbea54+dirty | 4852 | v5 flash while halted: tl_step/::multi (8 prompts per pass), Qwen2 pre-tokenizer + special-token split + span BPE (= llama.cpp on 47k records), q4k stage bench; card still hung until USB power cycle |
| 20260927-042423 | psram_llm-survey | adbea54+dirty | - | PC survey v1, 25 prompts x 40 tokens on tl_ref (OLD tokenizer, hand-written chat prompts without the system turn): 24/25 first answers right, PSRAM = "Phase Change RAM" wrong; top-2 margins per prompt |
| 20260927-053239 | psram_llm-pcverify | adbea54+dirty | - | PC verify of v5 (verify_pc.py): tokenizer core == reference == llama.cpp on 27,291 lines + 20,000 fuzz records (1.37 M tokens), 0 round-trip failures; 8 prompts per-token, batched and together all IDENTICAL to tl_ref |
| 20260927-053310 | psram_llm | adbea54+dirty | 0 | v5 upload FAILED, board untouched: "Unable find Teensy Loader (p)" while every CPU core ran tl_ref/tl_host; loader was up and listening. Hex kept, never flashed |
| 20260927-053402 | psram_llm | adbea54+dirty | 0 | v5 upload FAILED again, same cause, CPU still saturated; nothing reached the board. 20260927-060317 flashed the same image with the CPU idle |
| 20260927-060624 | psram_llm-survey | adbea54+dirty | - | PC survey v2, 60 prompts x 40 tokens, Qwen2 tokenizer + file's chat template: 51 of 56 gradable first answers right (code 10/10), 5 wrong (DNA near tie, ZIP regex, PSRAM, Spanish, feathers), 4 cut off; vs_survey1.md: 13 of 25 shared answers changed |
| 20260927-083324 | psram_llm-pcverify | adbea54+dirty | - | PC verify after the v6 core (shared prompt openings): tokenizer core == reference == llama.cpp on 27,559 lines + 20,000 fuzz records; 8 prompts per-token, batched, together with sharing off (45 passes) and on (36 passes, 3 x 24 positions copied) all IDENTICAL; driver smoke/dist/graphics/quality unchanged (5.429/5.421) |
| 20260927-090924 | psram_llm-survey | adbea54+dirty | - | PC long answers, 3 prompts x 200 tokens: Stack class all correct; palindrome correct to spec but its docstring example is wrong (comma kept); microSD explanation wrong ("metal contacts called pins") |
| 20260927-063135 | psram_llm-suite | adbea54+dirty | - | armed and never ran (waited for the power cycle); stopped at 09:16 to re-arm with --ref tl_ref_v5.exe before the reference's math changed |
| 20260927-093734 | psram_llm-pcverify | adbea54+dirty | - | PC verify after tl_math.h (shared exp/sin/cos/RoPE freq) + fp-contract off in the kernels: all IDENTICAL (tokenizer = llama.cpp on 27,915 lines + 20,000 records; 8 prompts per-token, batched, together, sharing on/off); new-math tl_ref byte-identical to the libm build on all 60 survey prompts (3,494 lines); perplexity unchanged 5.429/5.421 |
| 20260927-101735 | psram_llm-pcverify | adbea54+dirty | - | PC verify after the batched kernels (one nibble unpack for up to 8 vectors, q/k/v on presum): all IDENTICAL; dot_verify plain and M7-emulated (-DGD_EMULATE_M7) bit-identical, batched = single on 1/2/5/8 vectors; emulated M7 forward pass (tl_host_m7) per token, batched and 8 prompts together = tl_ref; driver smoke/dist/graphics/quality unchanged |
| 20260927-105333 | psram_llm-pcverify | adbea54+dirty | - | full PC verify of the v6 source incl. section 4: dot_verify_m7 (single, presum, batched M7 kernels = scalar) and tl_host_m7 (M7 kernels emulated) per token, batched and 8 prompts together with shared openings = tl_ref; tokenizer = llama.cpp; ALL IDENTICAL |
| 20260927-091641 | psram_llm-suite | adbea54+dirty | 12k | v5 on the board after the USB power cycle: card up on attempt 1 (17.5-18.1 MB/s sequential), 7 banks (Y4 failed qualification), bench: Q4_K presum 115.5 Mw/s (5.19 cyc/w), dot_q 106.9, stages 5.70/4.41/3.58 cyc/w, Q6_K 77.9-79.3 Mw/s, random 4 KB read 40.2 ms. France batched pass 0..4: 1144.1 s -- compute 148.4 s as projected, but SD 1833.9 MB in 992.9 s (1.85 MB/s), ffn stage 1081 s: FAT32 backward seeks walk the cluster chain (gate/up alternation). Answer "Paris" correct. Stopped at 12:20 to flash the fix |
| 20260927-122126 | psram_llm | adbea54+dirty | 1011 | suite flash: v6 + seek fix: contiguousRange (O(1) seeks) or forward-only handle pool; sharing, batched kernels, exact math; rounds x3 france batched, france_tok per-token, together8 |
| 20260927-122504 | psram_llm | adbea54+dirty | 4441 | v6 + seek fix + card-idle fix (syncDevice/CMD12 after every command and at boot): flashed while halted on the card hung at 12:22 |
| 20260927-122126 | psram_llm | adbea54+dirty | 1011 | v6 + seek fix (contiguousRange / forward-only handle pool) flashed after a clean ::stop -- and the card hung again: SdFat FIFO_SDIO leaves every read an open CMD18 transfer, so "between two reads" is still mid-transfer |
| 20260927-122126 | psram_llm-suite | adbea54+dirty | - | HALTED at boot, SdFat 0x17 x8 (card hung by the 12:21 flash) |
| 20260927-122504 | psram_llm | adbea54+dirty | 4441 | v6 + seek fix + card-idle fix (syncDevice = CMD12 after every command and before READY) flashed while halted; needs the USB power cycle |
| 20260927-162754 | psram_llm | adbea54+dirty | 1011 | suite flash: v7: card by DMA while computing, whole-sector reads; A/B slice/align/overlap/FIFO; night-2 tests + together8b; rounds x3 |
| 20260927-164610 | psram_llm | adbea54+dirty | 1011 | suite flash: v7b: PSRAM edge probed with four kinds of data (Y1 fault), SD clock logged, card bench in both modes; A/B + night-2 + together8b; rounds x3 |
| 20260927-122709 | psram_llm-suite | adbea54+dirty | - | v6 round 1 (FIFO), all exact to the digit: france 21/21 2058.9 s/17 passes (batched prompt 233.5 s); france_tok 21/21 2395.1 s/21 passes 114.05 s each; together8 8/8 = PC, 39 passes for 211 positions, shared=72, 9461.7 s (8-slot pass 302 s). Stopped at round 1's end for v7 |
| 20260927-132407 | psram_llm-pcverify | adbea54+dirty | - | v7 core (overlap pipeline): tokenizer, forward, together, M7-emulated -- all IDENTICAL |
| 20260927-141518 | psram_llm-pcverify | adbea54+dirty | - | v7 core with whole-sector reads (spare sectors spoiled on every read): all IDENTICAL; driver smoke/dist/graphics/quality pass, perplexity 5.429/5.421 |
| 20260927-162754 | psram_llm-suite | adbea54+dirty | - | v7 first boot: 8 banks; DMA sd_seq 7.26 MB/s at 4/16/64 KB; ab_default and ab_slice200 both failed at PSRAM 17,656,080 (bank Y1, edge 6 by a lucky probe; v6 measured 11). Stopped |
| 20260927-164610 | psram_llm-suite | adbea54+dirty | - | v7b boot: Y1/Y2 0x03 edges 11 with four kinds of probe data (were 6 and 8), all decoder banks 0x0B; bench DMA 7.26 vs FIFO 18.5 MB/s at 4/16/64 KB, same 49.5 MHz clock, OCRAM and DTCM alike. Stopped to run the tests after the A/B in FIFO |
| 20260927-164924 | psram_llm-suite | adbea54+dirty | - | v7b rounds x3 (restarted on the running board): A/B DMA arms, then night-2 + together8b in FIFO. ab_default (from the stopped suite's prompt): 312.2 s batched / 298.4 s one position, 98-99% of arithmetic hidden under DMA reads |
| 20260927-183958 | psram_llm-suite | adbea54+dirty | - | v7b realistic use (FIFO): chat question, 45 tokens with the template: first answer token 1,707.9 s (6 passes of 294-298 s / 229 s), then 113.2 s a token; 49/49 = PC, delta 0.0; PSRAM +0.011 s per context position; 0 PSRAM corrections over 9,042 s of boot. docs/55 |
| 20260927-211340 | psram_llm | 256abba+dirty | 1042 | suite flash: v8: the whole model on the card (tokenizer tables in qwen3b.tok, norms/biases read from the model file), PSRAM = attention cache only (3072 positions); realistic chat test |
| 20260927-210350 | psram_llm-pcverify | adbea54+dirty | - | v8 core, quick: tokenizer through the card store (28,497 lines + 2,000 fuzz records) = tokenizer.c; forward per token, batched, together, M7-emulated: all IDENTICAL |
| 20260927-211340 | psram_llm | adbea54+dirty | - | v8 flash: the whole model on the card (tokenizer tables in qwen3b.tok, norms/biases from the model file), PSRAM = attention cache only |
| 20260927-211340 | psram_llm-suite | adbea54+dirty | - | v8 boot built qwen3b.tok (8,301,328 B, contiguous), open 13.5 s; PSRAM 58.4 MB = cache, 3,072 positions, nothing of the model. Chat test: first answer token 1,706.4 s, 113.0 s a token, 49/49 = PC, 0 PSRAM corrections |
| 20260927-222806 | psram_llm | 256abba+dirty | 1042 | suite flash: v8 second boot: qwen3b.tok found on the card, checksum-verified and reused; France 2 tokens |
| 20260927-211937 | psram_llm-pcverify | adbea54+dirty | - | v8 core, full: tokenizer through the card store on 28,497 lines + 20,000 fuzz records = tokenizer.c; forward, together, M7-emulated all IDENTICAL; overlap proof 12/12 IDENTICAL; driver smoke/dist/graphics/quality pass |
| 20260927-222806 | psram_llm-suite | adbea54+dirty | - | v8 second boot (reflash, same image): qwen3b.tok found contiguous, checksum-verified, reused; model open 3.4 s; France 7/7 = PC, delta 0.0; 226.0 s batched, 112.5 s a token |
| 20260927-232030 | psram_llm | 511d0b3+dirty | 1042 | v8b: ::sdsweep (DMA watermark/burst sweep, every setting checked byte for byte against FIFO), ::sdcfg, ::sdregs; cold code in FLASHMEM |
| 20260927-234148 | psram_llm | 511d0b3+dirty | 1042 | v8b: ::sdclksweep (card clock 66/99 MHz, 8 MB checked byte for byte), ::sdclk |
| 20260927-234517 | psram_llm | 511d0b3+dirty | 1042 | suite flash: v8b: card clock chosen at boot by a verified sweep (by measured rate), fallback to 49.5 MHz on a failed read; realistic chat test |
| 20260927-232030 | psram_llm | 511d0b3+dirty | - | v8b flash: ::sdsweep / ::sdcfg / ::sdregs (DMA read watermark x burst length x burst enables, each 2 MB checked byte for byte against FIFO); 17 cold functions moved to FLASHMEM (RAM1 stack was -25.5 KB with sscanf, now 34.9 KB free) |
| 20260927-232030 | psram_llm-sdsweep | 511d0b3+dirty | - | ::sdsweep: FIFO 22.91 MB/s reference; DMA 7.31-7.33 MB/s at EVERY setting (wml 16/32/64/128 x brst 4/8/16 x blen 1/3/7, all 36 data OK) -- the DMA cap is not the watermark or burst. Boot dropped Y0 (94 wrong) and Y4 (no setting survives): 6 banks |
| 20260927-234148 | psram_llm | 511d0b3+dirty | - | v8b flash: ::sdclksweep / ::sdclk (card clock 198 MHz / N; 8 MB in four places checked byte for byte at each) |
| 20260927-234148 | psram_llm-sdclk | 511d0b3+dirty | - | ::sdclksweep in FIFO: 49.5 MHz 22.92 MB/s (SdFat's); 66 MHz 24.03, 99 MHz 23.86 -- 16 MB each, 0 wrong, 0 failed. The card, not the clock, is the ceiling above 66 |
| 20260927-234517 | psram_llm | 511d0b3+dirty | - | v8b flash: the clock sweep runs at boot and keeps the fastest divider by MEASURED rate (66 MHz here); a failed read above 49.5 MHz drops back for good and retries |
| 20260927-234517 | psram_llm-suite | 511d0b3+dirty | - | v8b at 66 MHz (boot sweep: 49.5 -> 22.90, 66 -> 24.01, 99 -> 23.85 MB/s, kept 66): chat 45 tokens, first answer token 1,615.4 s (v8 1,706.4), 105.7 s a token (113.0), card 76.0 s/pass at 24.13 MB/s (83.3 at 22.03); 49/49 = PC, delta 0.0; 6 banks this boot, 0 PSRAM corrections |
| 20260928-013821 | psram_llm | f765f82+dirty | 1042 | suite flash: v9: one chip one job -- each layer's cache inside one PSRAM chip, checksum per row (single reads), spare layer slots + retire-and-rerun on a chip fault; realistic chat test + cache bench |
| 20260928-013821 | psram_llm | f765f82+dirty | - | v9 flash: one chip one job -- each layer's cache in one PSRAM chip (6 layers a chip, 2,608 positions, 12 spare slots), checksum per row + single reads, chip fault -> retire, move layers, re-run prompt; bench cache_layer + psram_plat |
| 20260928-013821 | psram_llm-suite | f765f82+dirty | - | v9 on the board: 8 chips, 6 layers a chip on CS0 CS1 Y1 Y5 Y4 Y3, Y0/Y2 spare, 2,608 positions. Chat 45 tokens: first token 1,589.9 s (v8b 1,615.4), 105.1 s a token, PSRAM 6.5 s over 10 passes (34.8), 59 chip selects, 1 write redone on Y4, 49/49 = PC. cache_layer 1,024 positions read in 0.058 s, 1 chip select |
| 20260928-045113 | psram_llm | fcafefb+dirty | 1042 | v9b: ::sdadma (card by ADMA2 descriptors, 2 MB byte-checked at 4 read sizes), ::sdpath adma (pipeline reads by ADMA2 with the overlap), cache row re-reads named |
| 20260928-022632 | psram_llm-suite | fcafefb+dirty | - | v9 together8: 8 prompts, 39 passes, 211 positions, 72 shared; 8,527.1 s (v6 9,461.7), 218.6 s/pass, PSRAM 27.0 s (v6 168.0), 252 chip selects, 7 rows re-read by checksum, 1 write redone, 0 unresolved; 8/8 = PC |
| 20260928-045546 | psram_llm | fcafefb+dirty | 1042 | v9b (fixed sweep sizes): ::sdadma, ::sdpath adma |
| 20260928-045113 | psram_llm-sdadma | fcafefb+dirty | - | v9b ::sdadma: FIFO 23.94 MB/s; SdFat simple DMA 7.56; ADMA2 8.31 / 14.20 / 17.28 MB/s at 4 / 16 / 64 KB, all bytes right (274 us a command + 18.6 MB/s asymptotic). The 256 KB step read into the 64 KB buffer (my bug) and the board reset itself; rerun bounded |
| 20260928-045113 | psram_llm | fcafefb+dirty | - | v9b flash: ::sdadma, ::sdpath adma (pipeline whole-sector reads by ADMA2 with the overlap hook), cache row re-reads named in the log |
| 20260928-045546 | psram_llm | fcafefb+dirty | - | v9b flash with the sweep bounded to its buffer |
| 20260928-045546 | psram_llm-sdadma | fcafefb+dirty | - | ::sdadma rerun, reproduced: FIFO 23.95, simple DMA 7.54, ADMA2 8.31 / 14.20 / 17.28 MB/s at 4 / 16 / 64 KB, all bytes right |
| 20260928-045737 | psram_llm-suite | fcafefb+dirty | - | v9b A/B, France 5+2, all 7/7 exact: FIFO 208.6 s batched / 104.8 s token; ADMA2+overlap 143.0 s batched (card wait 17.0 of 135) / 137.5 s token; ADMA2 plain 260.9 / 159.9. Rule: >= 3 slots ADMA2+overlap, else FIFO |
| 20260928-052543 | psram_llm | fcafefb+dirty | 1042 | suite flash: v9c: card path chosen per pass (>= 3 positions: ADMA2 with the arithmetic under the read; else FIFO); realistic chat test |
| 20260928-052543 | psram_llm | fcafefb+dirty | - | v9c flash: the card path chosen per pass (::admamin, default 3: >= 3 positions ADMA2 with the arithmetic under the read, else FIFO) |
| 20260928-052543 | psram_llm-suite | fcafefb+dirty | - | v9c chat 45 tokens: 8-position passes 193.7-195.8 s (v9 274.8), first answer token 1,118.4 s (v9 1,589.9; v7b 1,707.9), 105.1 s a token, 853.4 s of card hidden, total 1,538.6 s; 49/49 = PC; 0 PSRAM corrections |
| 20260928-055523 | psram_llm-suite | b65b86d | - | v9c together8: 6,530.5 s (v9 8,527; v6 9,462), 8-slot passes 193.5 s, 3,826.6 s of card hidden, 2 card-mode switches, PSRAM 24.3 s, 1 row re-read, 210 chip selects; 8/8 = PC |
| 20260928-074656 | psram_llm | b65b86d+dirty | 1042 | v9c + extended ::sdadma: descriptor size, 112 KB reads into the arena, watermark/burst under ADMA2 |
| 20260928-075009 | psram_llm | b65b86d+dirty | 1042 | v9c + ::sdadma summing exactly the 2 MB region at every read size |
| 20260928-074656 | psram_llm-sdadma | b65b86d+dirty | - | extended ::sdadma: ADMA2 17.3 MB/s at 64 KB with 1/2/4 descriptors, 17.1 at 112 KB, unchanged by watermark/burst; three sizes reported WRONG because the sweep summed past the 2 MB region (my bug). 6 banks this boot |
| 20260928-075009 | psram_llm-sdadma | b65b86d+dirty | - | ::sdadma summing exactly the region: every size and setting data OK; ADMA2 16.4-17.0 MB/s from 64 KB up (ceiling), FIFO 23.94, simple DMA 7.56 |
| 20260928-080231 | psram_llm | 97360d6+dirty | 1166 | suite flash: v9d: batched Q4_K and Q6_K kernels two vectors at a time in registers (bit-identical, dot_verify M7-emulated); bench + realistic chat test |
| 20260928-080231 | psram_llm | 97360d6+dirty | - | v9d flash: batched Q4_K and Q6_K kernels two vectors at a time in registers (dot_verify: bit-identical, M7-emulated and host) |
| 20260928-080231 | psram_llm-suite | 97360d6+dirty | - | v9d: kernel_x8 Q4_K 183.4 M MAC/s (v9 139.4), Q6_K 114.5 (121.5, worse -- lanes rebuilt per pair); chat: 8-position pass 164.3 s (v9c 193.7), first token 970.9 s, 105.1 s a token, 49/49 = PC. 5 chips qualified this boot (CS1 out), 9 layers a chip, 1,738 positions |
| 20260928-082914 | psram_llm | 97360d6+dirty | 1042 | suite flash: v9e: Q6_K batched kernel with shared lane words per offset and two-vector accumulators (bit-identical); bench + realistic chat test |
| 20260928-082914 | psram_llm | 97360d6+dirty | - | v9e flash: Q6_K batched kernel back to the shared lane build per offset, two-vector accumulators under it (bit-identical) |
| 20260928-082914 | psram_llm-suite | 97360d6+dirty | - | v9e: kernel_x8 Q4_K 183.4, Q6_K 120.7-121.0 M MAC/s; chat: 8-position passes 161.7-163.9 s, first token 957.9 s (16.0 min; yesterday 1,707.9), 105.1 s a token, 49/49 = PC. 7 chips this boot (Y4 dropped) |
| 20260928-090357 | psram_llm | 97360d6+dirty | 1042 | suite flash: v9f: one-token kernels take the activation vector pre-widened (gguf_widen_act; Q4_K presum_w, Q6_K q6k_w; bit-identical); bench + realistic chat test |
| 20260928-090356 | psram_llm | 97360d6+dirty | - | v9f flash: one-token kernels take the activation vector pre-widened (gguf_widen_act, presum_w, q6k_w; bit-identical on both PC builds) |
| 20260928-090356 | psram_llm-suite | 97360d6+dirty | - | v9f chat: one-token pass compute 28.7 s, UNCHANGED (qkv 6.13 wo 4.63 s as v9c) -- the widened kernels buy nothing on the M7; first token 956.3 s, 105.1 s a token, 49/49 = PC. 7 chips |
| 20260928-100627 | psram_llm | 97360d6+dirty | 1042 | v9f + BENCH path widened (the one-token kernels with the vector pre-widened, measured directly) |
| 20260928-100627 | psram_llm-bench | 97360d6+dirty | - | BENCH path widened vs the originals: Q4_K presum 114.2 vs widened 112.1 M MAC/s; Q6_K 77.0 vs 76.3 (11,008 cols), 78.5 vs 81.2 (2,048 cols) -- a wash; the one-token path reverted to the original kernels |
| 20260928-101322 | psram_llm | 97360d6+dirty | 1042 | suite flash: v9g: the one-token path back on its original kernels (the widened variants measured a wash and stay only in gguf_dot.c and the bench); batched Q4_K two vectors at a time; realistic chat test |
| 20260928-101322 | psram_llm-suite | 97360d6+dirty | - | v9g (final of the day): chat 45 tokens, 8-position passes 161.3 s, first token 955.5 s (15.9 min; yesterday 1,707.9), 105.1 s a token, 49/49 = PC; 7 chips |
| 20260929-215440 | psram_llm-pcverify | 5ae889f+dirty | - | the 0.5B Q8_0 (Qwen2.5-Coder-0.5B-Instruct, 675,710,848 B) through the exact core: tokenizer on 30,338 lines + 2,000 fuzz records IDENTICAL; forward per token / batched / together IDENTICAL; M7-emulated kernels and forward IDENTICAL -- ALL IDENTICAL. The fault test's bank 3 never reached its 200th write on this small model (no recovery exercised, rc 0). Run with MODEL= (verify_pc.py now takes it from the environment) |
