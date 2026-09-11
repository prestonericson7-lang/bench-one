# Before you power anything

This repository describes wiring a 1.5 V DDR3 module to 3.3 V logic by hand, and running parts
outside the configurations their manufacturers test. Four things will destroy hardware, and all four
have been reasoned about rather than learned the hard way, which means they are worth stating plainly
rather than leaving in a comment.

**Find pin 1 of each FPGA header with a meter first.** The Alchitry banks are 50 positions in two rows
of 25, and every third position counting from the end of a row is power or ground. Getting the row
orientation backwards mirrors all 32 signals on that bank, so a 3.3 V output lands where a 1.5 V data
line was meant to be. Probe for continuity to ground and confirm that positions 1, 4, 7 and 10 are all
power or ground before a single wire is cut.

**A divider built wrong reads 3.3 V.** Two resistors in series from a logic pin, with the tap taken
after both, divides nothing. It will sit at the full logic level and apply 3.3 V to a 1.5 V input the
instant the module is inserted. Measure every divider output with the driving pin held high **before**
the module goes in the socket. The bring-up order in the bench sheet exists for this reason.

**Bring 1.5 V up before or with 3.3 V.** The output lines are dividers fed from logic. If logic is
live while the module is not, those dividers inject current into unpowered inputs. One switch for
both regulators.

**1.8 V is not close enough to 1.5 V.** DDR3's absolute maximum supply is 1.975 V and its recommended
window is 1.425 to 1.575 V. Running it at 1.8 V is inside the absolute maximum and outside the
specification, which is exactly the kind of shortcut that works on the bench and fails in a month.

**The DLL disable mode this design relies on is not warranted.** It is in the standard, its clock
range is published, and Micron states that it does not warrant normal-mode timings or functionality
there. Use a module you are willing to lose. That is not a formality: the whole point of starting with
a scrapped stick is that the experiment is allowed to fail.

Two smaller ones. An SN74LS245 is not a level translator and cannot read or write 1.5 V, despite
looking like a bus buffer. And a TXB0108 has deliberately weak output drive, so it works at a slow
clock and not a fast one — see [docs/38](docs/38-ddr3-on-a-microcontroller.md).

## Three that waste a day rather than destroy a part

These do not damage anything. They are here because each one fails in a way that points at the wrong
place, and the wiring sheet is the only thing that will save you.

**Without VREF the module is simply dead.** Two references at 0.750 V, contacts 1 and 67, one
independent divider each, decoupled at the contact. Every input comparator in every chip measures
against them. Forget them and the DIMM does not respond at all and looks like a bad solder job.

**Without DM0 grounded, writes are masked at random.** Contact 125 is the write data mask for the byte
lane in use, and it is an input. Floating, reads come back as whatever was there before, and the
failure presents as a broken write path.

**Without DQS0# driven, write data lands at random.** The strobe is differential. Drive one half and
the module's write receiver, a comparator, has no reference. Both halves, adjacent translator channels.

The complete list, with both ends of every wire, is
[WIRING.md](firmware/bench-one/fpga/ddr3_ice40/WIRING.md).

Nothing in this repository has been run on hardware. Every figure is simulated, synthesised, or
traced to a datasheet page. Treat it as a design to check, not a design to trust.

---

Build videos, including the failures: **[Little Brains Big Mess](https://www.youtube.com/@littlebrainsbigmess)**
