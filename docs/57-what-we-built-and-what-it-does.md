# 57 — What we built, what it does, and how we know (plain English)

Every number on this page was printed by the hardware itself. Beside each one is the name of the folder
in `bench-archive/` that holds the log it came from (`serial.log`: every line the board printed, with the
time). Where a figure is arithmetic on measured numbers rather than a measurement, it says so. The work
took place on 27 and 28 September 2026.

## 1. What this is, in one paragraph

We made a hobby microcontroller board costing about $30 — a **Teensy 4.1**, the kind of chip found in a
keyboard or a 3D printer — run a **3-billion-parameter language model**, the same kind of AI that answers
questions in a chat window (this one is Qwen2.5-Coder-3B, made by Alibaba). The board has one megabyte of
its own memory. The model is 1.93 gigabytes. It works anyway, because the whole model stays on a microSD
card and is read off the card, start to finish, for every single word the model produces; eight cheap
memory chips wired to the board by hand hold the model's notes about the conversation so far. It is slow —
about a quarter of an hour to start answering a question, then a word every couple of minutes — but its
answers are **exactly** the answers a desktop PC gives running the same model, checked digit for digit at
every step, and this page records precisely what that looks like.

## 2. The parts, and what each one does

| part | what it is | what it does here |
|---|---|---|
| **Teensy 4.1** | a microcontroller board: one ARM Cortex-M7 processor at 600 MHz, 1 MB of memory | does all the arithmetic |
| **microSD card** (in the board's own slot) | an ordinary camera card | holds **the whole model**: `qwen3b.gguf`, 1,929,903,072 bytes, plus `qwen3b.tok`, 8,301,328 bytes of the model's vocabulary tables that the board builds from the model the first time |
| **PSRAM chips** (ESP-PSRAM64H, 8 MB each, eight of them) | cheap memory chips, wired to the board by hand; two on their own select wires, six behind a small address-decoder chip | hold the model's **working memory**: what it has already read of the conversation. Nothing of the model itself lives here |
| **a PC** | | only to check the board's answers against the same model, and to hold the logs. The PC does none of the work |

The model is 3,085,938,688 numbers. To produce one word (a "token"), the board reads all of them off the
card and multiplies each one into the current state: 1,922,981,520 bytes — 1.92 GB — of reading per word,
every word.

## 3. What it can do today

The test question, sent exactly the way a chat app sends it (with the model's own system prompt and
formatting, 45 tokens in all): *"What is the difference between SPI and I2C? Answer in two sentences."*
The model's answer begins *"SPI (Serial Peripheral Interface"* — the same words, in the same order, with the
same internal scores to nine decimal places, as the PC produces.

| | Sept 27, first runs | **Sept 28, now** | logs (Sept 27 / Sept 28) |
|---|---|---|---|
| time until the first word of the answer appears | 28.5 minutes (1,707.9 s) | **15.9 minutes (955.5 s)** | `20260927-183958` / `20260928-101322` |
| each further word | 113.2 seconds | **105.1 seconds** | same |
| reading a question | 97 words an hour | **177 words an hour** | same |
| writing an answer, one person at a time | 32 words an hour | **34 words an hour** | same |
| **eight people's questions answered together** (16 words each) | 2 h 38 min (9,461.7 s) | **1 h 49 min (6,530.5 s)** | `20260927-122709` / `20260928-055523` |
| how much conversation it can hold | 2,048 words | **2,608 words**, and any one memory chip can fail mid-run without losing the answer | |
| answers equal to the PC's | every step | **every step** | |

Eight people at once is the sweet spot: a pass over the model costs 193.5 s when it is feeding eight
answers and 105 s when it is feeding one, so eight questions together get **4.3 times the words per hour**
of one at a time. It is a working, exact 3-billion-parameter assistant for **queued work** — questions
left with it and answered overnight — not for a live conversation.

## 4. How we know it is right

- **Every step is compared with a PC** running the same model file through a reference program. For each
  word the board prints the word it chose, its runner-up, and both their scores to nine decimal places; the
  PC prints the same; they must be identical. Since the run of Sept 27 12:27 they have been identical on
  every run: nine runs of the chat test (49 steps each, 49 of 49 every time), three runs of the
  eight-question group (8 of 8 every time), and every run of the shorter "capital of France" test. Before
  that run the words matched but the scores were off by up to 0.111 (section 5 says what was fixed).
- **The word-splitting is checked against llama.cpp**, the standard program people use to run these
  models: identical on 48,497 test texts (28,497 lines of this repository plus 20,000 random ones).
- **The memory checks itself.** At every start-up the board fills every memory chip with its own tagged
  data and only then reads them all back, so two select wires reaching one chip, or one chip forgetting,
  would show; a chip that returns one wrong byte is left out. During a run, every value written is read
  back and compared, and every row of the model's notes carries a checksum verified when it is read. Over
  the Sept 28 runs: 8 rows caught wrong and re-read right, 2 writes redone, 0 unresolved.
- **A chip dying mid-run was tested on the PC** by killing a simulated chip on its 200th write: the board's
  logic retired it, moved its work to spare room on the other chips, re-ran the question, and produced
  output identical to a run with no fault.
- **The full PC test suite ran after every change** (word-splitting, every kind of pass, the board's own
  arithmetic instructions emulated on the PC, the fault recovery): all identical, every time.

## 5. What changed, step by step, and what each step was worth

Times are for the same 45-token chat question unless stated. Each row names its log folder.

| when | what changed | before → after | log |
|---|---|---|---|
| Sept 27, 02:37–03:24 | **First answer ever.** "The capital of France is Paris. Paris is the largest city in France and is known for its rich history," — 21 of 21 words the PC's, 127.7 s a word, 7 chips | | `20260927-023701-psram_llm` |
| Sept 27, morning; on the board 12:27 | **Math made bit-exact with the PC.** The board's math library replaced with one that adds up the same way (`tl_math.h`) and the compiler stopped from fusing multiplies | scores off by up to 0.111 → **identical to the last digit** | `20260927-093734-psram_llm-pcverify`, `20260927-122709` |
| Sept 27, 12:21–12:27 | **Two card bugs.** Every backward seek re-walked the file's table of contents (a 4 KB read cost 40 ms), and a "stopped" board left the card mid-transfer, hanging it until it was unplugged | a batched prompt pass 1,144 s → 233.5 s | `20260927-122126`, `122504`, `122709` |
| Sept 27, 16:28–16:46 | **A memory chip on the edge of failure.** Chip Y1 had passed a lucky timing test and then failed two prompts at the same address; the test now demands four kinds of data read back right | 2 failed prompts → no chip accepted since has failed a prompt | `20260927-162754`, `164610` |
| Sept 27, 18:40 | The chat question first run, as a real user would send it | 1,707.9 s to the first answer word, 113.2 s a word | `20260927-183958` |
| Sept 27, 21:13–22:28 | **The whole model moved to the card.** The vocabulary tables and the small per-layer numbers had been copied into PSRAM at start-up; now they stay on the card and PSRAM holds only the conversation | conversation memory 2,048 → 3,072 words; speed unchanged (1,706.4 s, 113.0 s a word); start-up 6.8 → 3.4 s once the table file exists | `20260927-211340`, `222806` |
| Sept 27, 23:41–23:45 | **Card clock raised** from 49.5 to 66 MHz, after reading 16 MB at each speed and checking every byte; the board now runs that check at every start-up and keeps the fastest speed that reads right | card 22.03 → 24.13 MB/s; first word 1,706.4 → 1,615.4 s; a word 113.0 → 105.7 s | `20260927-234148`, `234517` |
| Sept 28, 01:38 | **PSRAM treated as separate chips, not one big memory.** Each layer's notes live inside one chip with a checksum per row; a chip that fails is retired and its work moves to spare slots; the bus stops hopping between chips | PSRAM time in an 8-word pass 4.08 → 0.25 s; chip switches per pass ~290 → 7; PSRAM over the whole test 34.8 → 6.5 s; first word 1,615.4 → 1,589.9 s; conversation 3,072 → 2,608 words, by choice, to keep a spare chip's worth of room | `20260928-013821` |
| Sept 28, 02:26 | the eight-question group on that layout | 9,461.7 → 8,527.1 s; PSRAM time in it 168.0 → 27.0 s | `20260928-022632` |
| Sept 28, 04:51–04:57 | **A faster way to read the card while the processor computes.** The card's ordinary fast mode ties up the processor; the library's DMA mode frees it but only reaches 7.56 MB/s; the controller's other DMA mode (ADMA2) reaches 17.28 MB/s. A test on one boot: a 5-word pass 208.6 → 143.0 s with the arithmetic run under the read; a single word 104.8 s the old way against 137.5 the new — so the new way pays only when 3+ words are in flight | | `20260928-045113`, `045546`, `045737` |
| Sept 28, 05:25 | **That rule live**: 3+ words in a pass → ADMA2 with the arithmetic under the read; fewer → the old mode | an 8-word pass 274.8 → 193.7 s; first word 1,589.9 → **1,118.4 s (18.6 min)**; 853.4 s of card reading hidden under arithmetic over the test | `20260928-052543` |
| Sept 28, 05:55 | eight questions under that rule | 8,527.1 → **6,530.5 s (1.81 h)**; 3,826.6 s of card reading hidden | `20260928-055523` |
| Sept 28, 08:02–10:13 | **The main multiply routine rewritten** to keep its numbers in the processor's registers instead of on the stack (the 4-bit-weight routine; the 6-bit one measured no gain and was left) | that routine 139.4 → 183.4 million multiply-adds a second (+32%); an 8-word pass 193.7 → 161.3 s; first word 1,118.4 → **955.5 s (15.9 min)** | `20260928-080231`, `082914`, `101322` |

## 6. What was tried and did not work (kept so it is not tried twice)

| idea | result | log |
|---|---|---|
| Read the card by the library's DMA mode with the arithmetic underneath (Sept 27) | 98–99% of the arithmetic hid under the read, bit-exact — but that DMA path is capped at 7.26 MB/s against 18.5 the ordinary way, so a word took 298.4 s instead of 112.6 | `20260927-164924` |
| Tune that DMA path's registers (36 settings, each checked byte for byte) | 7.31–7.33 MB/s at every one | `20260927-232030-psram_llm-sdsweep` |
| Raise the card clock further, to 99 MHz | reads every byte right but no faster than 66 MHz (23.86 vs 24.03 MB/s): the card, not the board, is the limit | `20260927-234148-psram_llm-sdclk` |
| Tune the ADMA2 read (descriptor sizes, 112 KB reads, register settings) | 16.4–17.3 MB/s whatever is set: a ceiling | `20260928-074656`, `075009` |
| Hide the arithmetic behind ADMA2 for single words too | the slower read costs 16 ns more per byte than the fast mode; a single word has 15.7 ns of arithmetic per byte to hide: nothing to gain, and measured 137.5 s against 104.8 | `20260928-045737` |
| Give the 6-bit-weight multiply routine the same register treatment | first attempt 6% slower (121.5 → 114.5 M/s), second attempt level (120.7–121.0): its cost is elsewhere | `20260928-080231`, `082914` |
| Pre-compute part of the single-word arithmetic once per matrix instead of once per row | no change at all: 28.7 s of arithmetic per word before and after; the routines measured directly, 114.2 vs 112.1 M/s | `20260928-090356`, `100627` |

Two mistakes of mine are also in the archive: one test read 256 KB into a 64 KB buffer and the board reset
itself (`20260928-045113`); another summed past the end of its 2 MB test region and reported three good
results as wrong (`20260928-074656`). Both were fixed and re-run (`045546`, `075009`).

## 7. What limits it now

Where the time goes in the current firmware (`20260928-101322`, and the stage breakdown in docs/55):

| pass | total | reading the card | arithmetic | memory chips |
|---|---|---|---|---|
| producing one word | 105.1 s | 76.0 s (72%, at 24.13 MB/s) | 28.8 s (27%) | 0.22 s |
| **reading 8 words of a question** | **161–164 s** | 1.9 s left showing; ~150 s hidden under the arithmetic | 160 s (99%) | 0.26–1.55 s |

A single word is limited by how fast the card can be read at all (24 MB/s; the 99 MHz test showed the card
sets that). A batch of words is limited by the processor's arithmetic. Both limits are the card interface:
the mode that reads fast needs the processor, and the mode that frees the processor reads 30% slower.

**What would move it** (arithmetic on the numbers above, not a measurement): a second storage device read
by its own DMA — the Teensy 4.1 has a USB host port, 480 megabits a second, with its own DMA engine —
carrying half the model, so both halves stream at once and the arithmetic runs underneath. Beyond that,
the same layout on an FPGA: independent memories each owning whole layers, a checksum on every row, spare
capacity that absorbs a failed memory, nothing spanning two. That is the pattern this work found, and it
does not care what the memory is made of.

## 8. The memory chips, boot by boot

The owner counts seven chips on the board. The board finds **eight** select lines that each take and
return their own 8 MB of distinctly tagged data — on seven start-ups all eight did — so eight separate
memories answer. Which of them pass the start-up test changes from one boot to the next. Over the 24
start-ups with a test in the archive:

| chip | how it reached the board | passed | dropped, and why |
|---|---|---|---|
| CS0, Y2, Y3, Y5 | CS0 direct; the others through the decoder | 24 of 24 | never |
| Y1 | decoder | 23 of 24 | once, Sept 27 03:41: 1 wrong byte in 8 MB |
| CS1 | direct | 22 of 24 | Sept 28 08:02 failed the timing test; 09:03 read back 64 wrong bytes |
| Y0 | decoder, output 0 | 12 of 24 | 8 times wrong bytes (32, 52, 94, 64, 40, 63, 72, 70 in 8 MB); 4 times failed the timing test. When it passes, only in its slower mode: 1.7–2.4 MB/s against the others' 2.8–3.1 |
| Y4 | decoder, output 4 | 12 of 24 | 12 times failed the timing test; read back right every time it passed |

Boots by chip count: 8 chips on 7 boots, 7 on 8, 6 on 8, 5 on 1. Y0 and Y4 are the decoder's outputs 0
and 4, which differ only in its highest address input; that is an observation about the wiring, not a
diagnosis. What a dropped chip costs is conversation length, never a wrong answer: with eight or seven
chips the layout holds 2,608 words and a spare chip's worth of room; with six, 1,956; with five, 1,738
(the one-big-memory layout of Sept 27 held 3,072 with eight and 2,647 with six, and had no spare).

**Heat.** The die temperature in the 77 archived readings was 41.9–50.9 °C (Sept 27: 42.5–50.9 in 27
readings; Sept 28: 41.9–43.8 in 50), with a heatsink and fan on; docs/55 recorded 52.2 °C seen live on Sept
27, which no log captured. The chip's own shutdown point is 90 °C.

## 9. Where everything is

- `bench-archive/<date>-<name>/serial.log` — every line the board printed, with the time; `INDEX.md` there
  lists every run in one line each.
- `docs/55` — the usable numbers, in detail. `docs/56` — the memory redesign, the card read, the multiply
  routines, with every measurement. `docs/54` — the full story from the first run, including everything
  fixed on the way.
- `firmware/bench-one/tests/psram_llm/README.md` — how to build, flash and run the tests yourself.
- The whole repository is public: github.com/prestonericson7-lang/bench-one.

## Glossary

**token / word** — the model works in tokens, pieces of text about the size of a short word; "word" above
means token. **pass** — one complete read of the model off the card, producing one token for every
position in flight. **prompt / question** — the text given to the model. **PSRAM** — pseudo-static RAM, a
cheap memory chip. **decoder** — a small chip that turns three wires into eight select lines, so six
memory chips can share the board's pins. **DMA** — a way for a controller to move data into memory
without the processor's help. **checksum** — a small number computed from a block of data and stored with
it; if the data changes, the checksum no longer matches. **exact / bit-identical** — the same number to
the last binary digit, not merely close.
