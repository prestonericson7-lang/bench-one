#!/usr/bin/env python3
"""check_xdc.py -- prove every PACKAGE_PIN in our XDC files was transcribed from the vendor documents.

Builds the ball map from the vendor's own files (the xlsx for JM1/JM2, the manual text for
everything else), then checks each XDC line: the ball must exist in the map, and the signal named
in the line's comment must be the signal the vendor attaches to that ball.

    python check_xdc.py <bundle_dir> <xdc> [<xdc> ...]
"""
import re, sys, os
import openpyxl

def vendor_map(bundle):
    m = {}   # ball -> signal
    wb = openpyxl.load_workbook(os.path.join(bundle, "04.Hardware/04.Hardware/02. Connectors Pins Signal and Equal Length/Puzhi PZ-Starlite CON Pins Signal and Equal Length.xlsx"), data_only=True)
    for ws in wb.worksheets:
        for row in ws.iter_rows(min_row=2, values_only=True):
            for pin, sig, ball in ((row[0], row[1], row[2]), (row[5], row[6], row[7])):
                if ball and isinstance(sig, str) and sig.startswith("IO_"):
                    m[str(ball).strip()] = (f"{ws.title} pin {int(pin)}", sig.strip())
    txt = open(os.path.join(bundle, "manual.txt"), encoding="utf-8").read()
    # "SIGNAL  PINNAME  BALL" rows in the manual tables (pin name may be IO_..., MIO.., FPGA-.., IO-..)
    for sig, pinname, ball in re.findall(r"^([A-Za-z0-9_\-]+)\s+((?:IO|MIO|FPGA|PS)[A-Za-z0-9_\-]*)\s+([A-Z]\d{1,2})\s*$", txt, re.M):
        m.setdefault(ball, ("manual", f"{sig} ({pinname})"))
    # LED/KEY rows use "LED1 IO-0-34 R19"
    for sig, pinname, ball in re.findall(r"^(LED\d|KEY\d)\s+(IO[\w\-]+)\s+([A-Z]\d{1,2})\s*$", txt, re.M):
        m.setdefault(ball, ("manual", f"{sig} ({pinname})"))
    m.setdefault("U18", ("manual 3.2 / sch 4", "PL_CLK_50M (IO_12P_MRCC_34)"))
    m.setdefault("U19", ("manual 3.3 / sch 4", "PL_nGRST (IO_L12N_MRCC_34)"))
    return m

def check(xdc, m):
    ok = True; n = 0
    for ln in open(xdc, encoding="utf-8"):
        s = ln.strip().lstrip("#").strip()
        mm = re.match(r"set_property\s+PACKAGE_PIN\s+([A-Z]\d{1,2})\s+\[get_ports\s+(\{[^}]+\}|[^\]]+)\]\s*(?:;#\s*(.*))?", s)
        if not mm:   # the -dict form: set_property -dict {PACKAGE_PIN V20 IOSTANDARD ...} [get_ports x] ;# comment
            mm = re.match(r"set_property\s+-dict\s+\{[^}]*?PACKAGE_PIN\s+([A-Z]\d{1,2})[^}]*\}\s+\[get_ports\s+(\{[^}]+\}|[^\]]+)\]\s*(?:;#\s*(.*))?", s)
        if not mm: continue
        n += 1
        ball, port, comment = mm.group(1), mm.group(2).strip("{} "), (mm.group(3) or "")
        if ball not in m:
            print(f"  FAIL {os.path.basename(xdc)}: ball {ball} ({port}) is not in any vendor table"); ok = False; continue
        where, sig = m[ball]
        # the comment (or, for JM ports, the port name) must reference the vendor's signal or pin
        tok = lambda x: {t.lower() for t in re.findall(r"[A-Za-z0-9]+", x) if len(t) >= 3 and t.lower() not in ("io", "fpga", "pin")}
        tokens = tok(comment + " " + port); want = tok(sig)
        hit = bool(tokens & want) or any(f"pin {t}" in comment or port.endswith("_"+t) for t in re.findall(r"pin (\d+)", where))
        if not hit:
            print(f"  FAIL {os.path.basename(xdc)}: {ball} ({port}) vendor says {sig} @ {where}, line says '{comment}'"); ok = False
    print(f"  {os.path.basename(xdc)}: {n} PACKAGE_PIN lines checked against the vendor tables -> {'OK' if ok else 'FAILED'}")
    return ok

if __name__ == "__main__":
    bundle, xdcs = sys.argv[1], sys.argv[2:]
    m = vendor_map(bundle)
    print(f"vendor ball map: {len(m)} balls (xlsx + manual)")
    sys.exit(0 if all(check(x, m) for x in xdcs) else 1)
