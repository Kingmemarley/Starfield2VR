# Starfield.exe analysis helpers

Small Python helpers used to read `Starfield.exe` (1.16.236) while working out the game's internals (menu flags, the
world-draw check, render graph types). Python 3 with numpy; for disassembly `pip install capstone` (or objdump /
llvm-objdump on the PATH).

Set `STARFIELD_EXE` to the exe (default: `D:\Program Files\Steam\steamapps\common\Starfield\Starfield.exe`); run from this
folder. Addresses are RVAs (offsets from the image base).

- `pe.py`: sections; `r2o`/`o2r` (RVA <-> file offset), `u32`/`u64`/`cstr` reads.
- `rtti.py`: MSVC RTTI: type names (`tds`), complete object locators (`cols`), vtables (`vts`); `vtables_of('.?AV...@@')`,
  `bases(col)`. The first run builds `rtti.pkl` (a few minutes).
- `xref.py`: `refs(rva)` finds rel32 references to an address (calls, jumps, rip-relative loads; the first run builds
  `rel.npy`, about 500 MB); `dis(rva, length)`; `func_of(rva)` / `func_start(rva)` from `.pdata`.
- `util.py`: `dfun(rva)` disassembles the whole function around an address, with vtable and string annotations.

Example:

    python -c "from util import *; print(dfun(0x188dea0))"
    python -c "from rtti import *; print(vtables_of('.?AVTemporalAA_idTech7RenderPass@CreationRendererPrivate@@'))"
