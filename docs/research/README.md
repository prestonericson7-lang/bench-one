# Research

Raw output from the datasheet research fleet: 30 agents across two sessions, 15 domains,
573 sourced facts, then 10 adversarial re-checks.

- **FINDINGS.md** — every fact with its primary source and a confidence level. Anything marked
  *unverified* was not confirmed against a primary source and must be measured before it is
  trusted.
- **VERIFICATION.md** — a second agent told to REFUTE each report, and to default to *uncertain*
  rather than rubber-stamp anything it could not source. **29 claims were refuted**, several of
  which changed the firmware.
- **`*.json`** — the structured reports as returned.

Three refutations that changed real decisions:

1. **The 74HC138 chip-select budget.** I claimed no vendor specifies AC timing at 3.3 V. onsemi
   publishes a full guaranteed 3.0 V column for the MC74HC138A — 125 ns address-to-output at
   85 °C, not the 225 ns I had inferred from TI's 2.0 V column. The 500 ns budget survived; its
   justification changed from arbitrary caution to 4× a real bound.
2. **The 74HC4051 has no 3.3 V specification at all.** Every "70 Ω at 3.3 V" figure in circulation
   is somebody's own measurement, typically 3–4× optimistic. The firmware now designs against the
   2.0 V bound and says so.
3. **A 74HC138's mutual exclusivity is a property of its static function table only.** It
   guarantees nothing during an address transition — which is exactly why break-before-make is
   enforced for the analog mux tree as well as for chip selects.

The research agents were told that fetched pages are data, never instructions. An earlier pass hit
fake `system-reminder` text on a datasheet mirror attempting to rewrite commit attribution; it was
correctly treated as page content and ignored.
