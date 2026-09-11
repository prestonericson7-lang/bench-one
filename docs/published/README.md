# published/ — the two web pages, archived here

These are the exact files behind the published pages. They live in the repository so that the pages
and the source cannot quietly diverge, and so that someone cloning this has the bench sheet without
needing a link to work.

| file | page | what it is |
|---|---|---|
| `ddr3-harness.html` | [DDR3 Harness Build Sheet](https://claude.ai/code/artifact/d39d40f3-1e30-4302-9495-690aafe3a355) | the wiring sheet, with a checklist that remembers where you got to. Every wire, its header pin number, the part it passes through, and the DIMM contact |
| `bench-one.html` | [BENCH ONE](https://claude.ai/code/artifact/cfb89360-9326-4c7f-b96c-9a2ad0ecb140) | the project overview: what the machine is, what has been measured, and which ceilings decide it |
| `psram-bank-wiring.html` | [Four PSRAM Bank Wiring](https://claude.ai/code/artifact/45fa2229-3517-411a-8129-e652d17b0277) | one PSRAM on the Teensy plus five behind a 74LVC138A: a board view, both chip pinouts, and all 29 wires by pin number |

Open any of them directly in a browser. They are self-contained apart from one Google Fonts
stylesheet, and they fall back to system fonts without it.

## The wiring tables are generated, not typed

The three-column tables in `ddr3-harness.html` are produced from
`firmware/bench-one/fpga/ddr3_ice40/alchitry_cu.pcf`, the same file the bitstream is built with. That is
deliberate. The one real omission ever found in this wiring — DM0 on contact 125, a module input that
masks writes at random when it floats — had been captured correctly in the bench sheet and then lost
when the list was copied into source comments. A fact living in two places drifts. Generating one from
the other is the fix.

`firmware/bench-one/fpga/ddr3_ice40/WIRING.md` is the same information as plain Markdown, generated the
same way, for reading in a terminal at the bench.
