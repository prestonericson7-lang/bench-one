"""hy_layout in tl_core.c, simulated: 36 linear regions then 12 attention caches, first-fit in bank order."""
import sys
banks = [8 << 20] * (int(sys.argv[1]) if len(sys.argv) > 1 else 8)
layers = ["L" if (l + 1) % 4 else "A" for l in range(48)]
lin_bytes = 1163296
per_pos = 2 * 512 + 2 * 4 * 2 + 8
s_lim = (16 * 3072 - 4 * 4096) // 16

def place(layer_bytes):
    used = [0] * len(banks); where = {}
    for kind in ("L", "A"):
        for l, k in enumerate(layers):
            if k != kind: continue
            need = lin_bytes if k == "L" else layer_bytes
            b = next((b for b in range(len(banks)) if used[b] + need <= banks[b]), None)
            if b is None: return None, False
            where[l] = b; used[b] += need
    spare = True
    for d in range(len(banks)):
        if not used[d]: continue
        u2 = list(used)
        for l, b0 in where.items():
            if b0 != d: continue
            need = lin_bytes if layers[l] == "L" else layer_bytes
            b = next((b for b in range(len(banks)) if b != d and u2[b] + need <= banks[b]), None)
            if b is None: spare = False; break
            u2[b] += need
        if not spare: break
    return (where, used), spare

s_max = max(S for S in range(16, s_lim + 1) if place(per_pos * S)[0])
s_red = next((S for S in range(s_max, 1023, -1) if place(per_pos * S)[1]), 0)
S = s_red or s_max
(where, used), spare = place(per_pos * S)
print("banks %d: s_max %d, spare S %d -> max_seq %d (spare %s)" % (len(banks), s_max, s_red, S, spare))
print("per bank used MiB:", [round(u / 2**20, 2) for u in used])
print("layer -> bank:", "".join(str(where[l]) for l in range(48)))
