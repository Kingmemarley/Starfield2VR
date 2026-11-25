# Building the mod for another Starfield version (fix188)

The mod is written for Starfield **1.16.236** (Steam). The same code also builds for **1.16.244** (Steam's latest, June 2026) as a
second dll. Every place in the game the mod uses is written in the code as its 1.16.236 address. The 1.16.244 build looks each
one up in a port table, `src/CreationEngine/memory/GameCode_1_16_244.inc`, made by `tools/port/bake.py` from the two versions'
`Starfield.exe`.

## The two builds

| Game version | Preset (configure / build) | The dll |
| - | - | - |
| 1.16.236 | `build-release-no-debug-msvc` / `release-no-debug-msvc` | `build/build-release-no-debug-msvc/dxgi.dll` (and `artifact/ModRelease/sfvr`) |
| 1.16.244 | `build-release-no-debug-msvc-1.16.244` / `release-no-debug-msvc-1.16.244` | `build/build-release-no-debug-msvc-1.16.244/dxgi.dll` (and `artifact/ModRelease-1.16.244/sfvr`) |

```
cmake --preset build-release-no-debug-msvc-1.16.244
cmake --build --preset release-no-debug-msvc-1.16.244
```

The 1.16.244 build is never copied into a game folder.

Each dll reads the running game's version from `Starfield.exe`'s version resource. It logs that version at the start
(`[GameCode] (fix188) Game version: ...`) and shows it in the F11 window. If the game is another version, the dll uses nothing of
the game's through GameCode.

## How the code asks for the game's places

Use `src/CreationEngine/memory/GameCode.h`. Never add the game's base to an rva yourself.

- `GameCode::At(rva)`: the address of a place, given by its 1.16.236 rva. It is 0 when this build's game doesn't know it.
- `GameCode::Code(rva, bytes)` / `GameCode::Same(rva, bytes)`: a run of the game's code the mod checks before using it.
  - In 1.16.236 the bytes must be the ones given.
  - In a ported build, the table's bytes for that run must match, since its calls and data references are other numbers.
- `GameCode::Known({rva, ...})`: whether every one of these is known.
  - Use it in a feature's install for places it uses without checking their code.
  - In a ported build a missing place is 0, so the feature must stay off rather than call 0.

The port tool finds these calls itself. In converted files a local `At(rva)` or `SameCode(rva, bytes, n)` that wraps GameCode
counts too. Each call's first argument must be a constant of that file (`kSomethingRva`, or `kSomethingRva + 7`). For a check,
the second argument is the name of a `constexpr uint8_t k...[]` array. Anything else is listed as "not understood".

A check made at a place found at run time (a call's target, say) is written as the target's own place:

```
g_fn != At(kFnRva) || !SameCode(kFnRva, kFnStart, sizeof(kFnStart))
```

That way the tool knows the place.

## Making the 1.16.244 table

1. Get 1.16.244's `Starfield.exe` without updating your game. Steam owners can do this.
   - Press Win+R and run `steam://open/console`.
   - Type `download_depot 1716740 1716741 6026043903923231328`. That is depot 1716741, the exe and dlls, at its 1.16.244
     manifest; it downloads about 116 MB.
   - The files land in `<Steam>\steamapps\content\app_1716740\depot_1716741\`. The installed game is not touched.
   - If that folder is already there (an earlier `download_depot`, say of 1.16.236 for a downgrade), rename it first: the
     download writes over it.
   - Or, if your Steam install is already 1.16.244 and you keep 1.16.236 elsewhere (a copy of the whole game folder that Mod
     Organizer 2 runs), use the two folders' exes directly.
   - The manifest id comes from Steam's public app data as of Nov 2026. Check the downloaded exe's version: right-click it,
     choose Properties, then Details, and it should say 1.16.244.0.
2. Optional, but it makes the port surer: Address Library for SFSE Plugins (Nexus mod 3256). Its files
   `versionlib-1-16-236-0.bin` (v20) and `versionlib-1-16-244-0.bin` (v22) give each function the same id in both versions.
3. Run the tool. It needs Python 3.9+ with `pip install capstone numpy`.

   ```
   python tools/port/bake.py --old "<1.16.236>\Starfield.exe" --new "<1.16.244>\Starfield.exe" ^
          --old-al versionlib-1-16-236-0.bin --new-al versionlib-1-16-244-0.bin
   ```

   It writes `src/CreationEngine/memory/GameCode_1_16_244.inc` and `docs/PORT_REPORT_1_16_244.md`. The report lists every
   place, how it was found, or why it was not.
4. Read the report.
   - A place not found leaves its feature off in the 1.16.244 build. That is safe.
   - Fix it by hand only after looking at the code in both exes.
   - (fix190) A place the update changed, but which still does what the mod needs, goes in `tools/port/manual_1_16_244.txt`: its
     1.16.236 rva, its 1.16.244 rva, its bytes there (covering the code's longest checked run there) and the start of the 1.16.244
     function it is in, with the reason. The tool checks the bytes and the function against the 1.16.244 exe (a few bytes alone are
     found in many places) and then uses them. It warns when it found the place itself somewhere else. Write the reason so someone
     else can check it.
   - (fix190) The report and `--inventory` also list the places a file not converted yet still adds the game's base to by hand, as
     far as the tool recognises them: it looks for the usual ways of writing it (`base + kSomethingRva`, `MemoryScan::mod + 0x...`).
     Other forms, such as `base + k.table_rva`, are missed, so check a file by eye when converting it.
5. Build the 1.16.244 preset.

How the tool finds a place:
- **A function.** By its Address Library id, or by the same instructions where only the jump distances and data references
  differ. When several functions look the same, it takes the one nearest to where the functions around it moved.
  - (fix190) Small functions with no unwind entry (`.pdata`) are cut out of the code between them at the int3 (0xCC) padding the
    compiler puts before each function, and matched the same way.
  - (fix190) A switch's jump tables, which sit at the end of their function, are compared as tables (their entries as distances
    into the function), so such functions can be matched at all. A table in the game's data that the code reads by its rva from
    the image's base (`[reg + index * 8 + rva]`) counts as a data reference, like a rip-relative one.
  - (fix190) When several functions look the same and the nearest one is not clear: by its place between the nearest functions
    before and after it that are one of a kind in both exes. The new exe must have as many functions between those two, each the
    same length and alike. If its twins are among them too, what it refers to (functions matched for certain, imports, strings,
    type tables) must fit it and not those twins. When both ways give an answer, they must agree.
- **A place inside a function.** The same distance in, if the body is alike. If the body changed, it looks for a checked run of
  code inside the new function.
- **A global or a type table.** Through an instruction that refers to it in a matched function.
  - (fix190) Every reference to it exactly must agree.
  - (fix190) When the only references are to places just before it: two or more different places must agree, and the data from
    the nearest through it must be the same in both when the file has those bytes. With one place only, the data from there through
    it must be the same in both (pointers may differ only where they go to the ports of their old targets), and the place itself
    must hold something to check (a string, or a pointer to a function matched for certain): zeros prove nothing.
  - (fix190) A type table's slot that holds the only pointer to a function matched for certain: the only slot in the new exe
    pointing to that function's port, and the same slot number of its table in both. It is also a check on the other answers.
  - (fix192) A type table (vtable), or a slot in one, is decided by its type alone. The word before a table points to the
    compiler's type information (RTTI), which names the class. The tool takes the one table of that class (and the same part of
    the object, for a class with several tables) in the new exe, with as many slots, and the same slot in it. The linker may put
    tables in another order: 1.16.244 swapped two blur passes' tables, and going by the tables around it gave the wrong one. When
    another rule gives a different answer, the type wins and the report says so (a WARNING). Two exceptions: a slot past the start
    whose function is not matched for certain does not overrule another answer (the place is then left out, to check by hand), and
    a lambda's or an anonymous namespace's class not found under its name is left to the other rules (the compiler numbers those
    names, and the numbers change between builds). Wherever data is compared, a pointer to a type's information must go to the
    same class, and a table code refers to must be of the same class; names differing only in those numbers count as unknown.

Every checked run must be alike in both versions, apart from distances. Otherwise it is left out. (fix190) The one exception is a
place in the manual list: its bytes are the ones written there, checked against the new exe.

## Testing the tool without another version

```
python tools/port/bake.py --old "<1.16.236>\Starfield.exe" --inventory
python tools/port/bake.py --old "<1.16.236>\Starfield.exe" --new "<the same exe>" --selftest
```

- `--inventory` lists what the code asks for and checks every run's bytes against 1.16.236. A "WRONG" line means the code or the
  tool is wrong.
- `--selftest` must find everything exactly where it is.

## What is ported so far

- **fix188:**
  - the core's own places: Mutar's 19, with the 1.16.244 values from the upstream port (github.com/mutars/starfield2vr pull
    request 9). With `SIGNATURE_SCAN` they are mostly found by their scans;
  - that port's main-menu camera fix, in the 1.16.244 build only;
  - GameCode, with GrenadeHand.cpp (the grenade and the left-hand weapon) using it.
- **fix190:**
  - every feature file converted to GameCode (26 files, 441 places, 230 checked runs; `--inventory` clean). Left: the unused
    upstream `CreationEngineWeaponModule.cpp` (never constructed);
  - in a ported build a place not in the table is 0, and each feature stays off rather than use it: every call, hook, patch and
    write is behind a check of its own bytes, a `GameCode::Known({...})`, or an `!= 0` (two independent reviews of the change);
  - the tool: places matched by hand (`manual_1_16_244.txt`), code without unwind entries (cut into pieces at int3 padding), twin
    functions placed between their one-of-a-kind neighbours, switch tables compared as distances, more data rules, and a listing
    of places still added to the base by hand (by pattern: other spellings can slip past it). The self-test finds every place;
  - **the 1.16.244 table: 413 of 441 places** (no Address Library). The 28 not found are in 11 functions 1.16.244 really changed;
    their features stay off in that build: the left-hand weapon's equip route (LeftWeapon's worker 0x15E9090 and the quick keys'
    0x1EC2040, also the favorites wheel's use), the VR tab in Settings (the game's string functions 0x28CFB80 and 0x28CEF30, also
    Physical's), Physical's seat (0x12D5100) and setting read (0x1A51A70), GloveHighlight's scanner mark (0x17D7630), HandGrab's
    writer call (0x23781C0), and two PhysicsProbe entries. The report: `docs/PORT_REPORT_1_16_244.md`.
- **fix191: the 1.16.244 table has all 441 places**; every feature is on in that build (not yet run in 1.16.244: checked by reading
  both exes).
  - Five of the 28 were never changed: capstone 5 gives `vpor`/`vpand`/`vpxor [rip+...]` a disp_size of 2, so their distance was not
    masked and whole functions looked changed. The tool now masks a rip operand's 4 bytes whatever disp_size says (GloveHighlight's
    scanner, HandGrab's collision update, Physical's setting read).
  - The rest are in `manual_1_16_244.txt`, each read in both exes: LeftWeapon's worker, UseQuickkey and Physical's seat queue differ
    only by a per-thread counter's offset (the block's +0x278 is +0x27C in 1.16.244: two dwords swapped; the mod never uses it); the
    game's string release (0x28CEF30 -> 0x28CAEA0) only by a register and its lock's wait, the make (0x28CFB80 -> 0x28CBA80) by a
    smaller frame, both found by hundreds of matched callers; PhysicsProbe's equip entry by the counter too.
  - A layout check of the two exes (304,637 functions paired, every changed field displacement or immediate listed): 1.16.244 moved
    only the per-thread block's +0x278/+0x27C and +0x2C48/+0x2C4C, two small parts of the ship builder's data (+4), BSFaceGenManager
    from +0x5BA0 (-0x10) and a scale controller's fields. The mod uses none of them; every object field offset it uses has unchanged
    uses in the game's code. Its type-table slots hold the ports of their functions.
- **fix192: 449 of 449.** EyeShare (full stereo's shared render-pass results) converted: a 1.16.244 test showed it off there
  ("not as in Starfield 1.16.236"), its places written as `base + k.table_rva` (a form the hand-added listing does not catch). Its
  type tables showed a precision hole: the near-reference data rule mapped the vertical blur pass's table to the horizontal one's
  (the linker reordered them). The type-table rule (class name, locator offset, slot) now decides every vtable place; an audit by
  class of all 43 vtable answers found no other wrong one. HavokModule's and CreationEngineWeaponModule's fixed addresses are dead
  code (commented out; never constructed).

  The tool also lists (with --inventory; the first also in the report):
  - a converted file that still adds the game's base to a place;
  - calls that pass a variable, which must sit inside a wrapper.
