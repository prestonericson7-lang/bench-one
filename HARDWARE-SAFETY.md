# Before you power anything

This repository describes wiring a 1.5 V DDR3 module to 3.3 V logic by hand, and running parts
outside the configurations their manufacturers test. Four things will destroy hardware, and all four
have been reasoned about rather than learned the hard way, which means they are worth stating plainly
rather than leaving in a comment.

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

Nothing in this repository has been run on hardware. Every figure is simulated, synthesised, or
traced to a datasheet page. Treat it as a design to check, not a design to trust.

---

Build videos, including the failures: **[Little Brains Big Mess](https://www.youtube.com/@littlebrainsbigmess)**
