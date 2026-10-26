# ARM64 → C generator

Copied from LASRecomp (`tools/`, MIT license, © 2026 Peiikai): `gen.py`, `lifter.py`, `analyze.py`, `nso.py`,
`passes.py`, `simd_c.py` unchanged. `patches.py` was emptied (its patches were Link's Awakening addresses).

Usage (from this folder, with the project's venv):

    ../../.venv/Scripts/python gen.py main    ../../extracted/exefs/main    0x7000000000 ../../generated/main --simd c
    ../../.venv/Scripts/python gen.py sdk     ../../extracted/exefs/sdk     0x7004000000 ../../generated/sdk  --simd c
