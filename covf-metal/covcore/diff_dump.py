#!/usr/bin/env python3
# diff_dump.py a.txt b.txt — compare deux dumps refcore à tolérance relative.
# Critère du plan : concordance à ~1e-5 relatif (libm x86/MSVC ≠ arm64/clang),
# JAMAIS au bit près. Valeurs exactes (0, entiers) comparées telles quelles.
import sys

TOL = 1e-5

def load(path):
    d = {}
    order = []
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            key, _, val = line.partition("\t")
            if key in d:
                sys.exit(f"clé dupliquée dans {path} : {key}")
            d[key] = val
            order.append(key)
    return d, order

a, order_a = load(sys.argv[1])
b, order_b = load(sys.argv[2])
if order_a != order_b:
    only_a = [k for k in order_a if k not in b]
    only_b = [k for k in order_b if k not in a]
    sys.exit(f"clés différentes : seulement-A={only_a[:5]} seulement-B={only_b[:5]}")

worst, worst_key, bad = 0.0, "", []
for k in order_a:
    va, vb = a[k], b[k]
    if va == vb:
        continue
    try:
        fa, fb = float(va), float(vb)
    except ValueError:
        bad.append((k, va, vb, None))
        continue
    if fa == fb:
        continue
    denom = max(abs(fa), abs(fb), 1e-30)
    rel = abs(fa - fb) / denom
    if rel > TOL:
        bad.append((k, va, vb, rel))
    if rel > worst:
        worst, worst_key = rel, k

print(f"clés={len(order_a)} pire-erreur-relative={worst:.3e} ({worst_key})")
if bad:
    print(f"{len(bad)} valeurs hors tolérance {TOL} :")
    for k, va, vb, rel in bad[:20]:
        print(f"  {k}: {va} vs {vb} (rel {rel if rel is not None else 'non-numérique'})")
    sys.exit(1)
print(f"CONCORDANCE ✓ (tolérance {TOL})")
