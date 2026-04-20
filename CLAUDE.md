# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build / test / run

```
make            # builds ./cmd (compiles cmd.o + audio.o separately)
make media      # converts /System/Library/Sounds/Hero.aiff -> bin/media/tada.wav
make test       # runs the in-script unit tests (./cmd tictactoe.cmd /test)
make play       # plays interactively
make clean      # rm's cmd, *.o, bin/tmp, bin/media
```

Running a single test: tests live **inside** `bin/main.cmd` as labels named
`:test_*` and run via `call :test_foo`. There's no framework. To run one
interactively without the harness, edit the `:tests` dispatcher or call
directly via `./cmd tictactoe.cmd /test` and read the section of
`tmp/test_output.txt` that the harness writes per test.

macOS-only: the Makefile links CoreFoundation / CoreAudio / AudioToolbox and
uses `afconvert` for media generation. Don't assume Linux unless you wire
the Linux audio backend too.

## The load-bearing constraint

**The `.cmd` files in `bin/` must still run on real `cmd.exe` on Windows.**
They are the authoritative game; the C emulator exists to run them without
booting Windows. When cleaning up or simplifying, do not change batch
idioms (e.g., `c:\windows\media\NAME.wav`, `sndrec32 /play /close /embedding`,
`ping -w N` for sleep, `reply` for keyboard input) — the C emulator is
expected to translate / fake those, not the scripts.

If a feature's only user is a specific `.cmd` file, the emulator can drop
support for the unused bits and still ship the scripts untouched.

## Architecture

### Two halves

1. **`bin/*.cmd`** — the game, in NT batch. `tictactoe.cmd` → `bin/main.cmd`
   contains ~800 lines of game logic + AI + a test harness. Calls small
   helpers (`drawgrid.cmd`, `sleep.cmd`, `play_sound.cmd`) and the
   `reply.com` DOS stub for keyboard reads.

2. **`cmd.c` + `audio.c`** — the host-side emulator. `cmd.c` parses and
   interprets batch; `audio.c` wraps miniaudio for WAV playback.

### Emulator structure (`cmd.c`)

- `script_load` slurps a `.cmd` file and splits it into paren-aware
  statements. Results live in a per-`Script` bump arena that owns the
  struct, the slurp text, every statement string, label names, and the
  pointer arrays. `script_free` is a single `free`.
- `execute_stmt` / `execute_seq` are a recursive walker. On entry each
  statement takes an arena mark; on exit it resets. This gives
  call-stack-shaped lifetimes to all transient strings (expansion
  results, tokens, etc.).
- `run_simple` dispatches builtins (echo, set, if, for, goto, call,
  exit, pushd, popd, cd, type, setlocal, endlocal). Unknown names fall
  through to `call_internal_or_external`, which handles faked externals
  (`reply`, `ping`, `start`, `sndrec32`, `debug`, `cmd`) before looking
  for a `.cmd`/`.bat` on disk.
- Windows-media path rewrite: `map_media_path` rewrites
  `c:\windows\media\X.wav` → `media/X.wav` (relative to cwd). Used by
  both `file_exists` (so `if exist` works) and `sndrec32` (so audio
  plays).

### Arenas — the memory discipline

The emulator deliberately minimises `malloc`/`free`. Three long-lived
arenas cover everything; direct `malloc` is an exception worth
justifying.

| arena        | purpose                                            | size (init) |
|--------------|----------------------------------------------------|-------------|
| `g_scratch`  | per-statement transients (mark/reset per stmt)     | 4 MB        |
| `g_var_arena`| `Var` structs + `name_lc` strings (freelist-recyc) | 64 KB       |
| per-Script   | one per loaded `.cmd`; text + arrays               | `len*8 + 64K` |

`Var` has a 56-byte inline value buffer; short values never hit the heap.
Heap is only used when a value exceeds 56 bytes, and the heap buffer is
reused across updates once grown.

Benchmarks worth defending: `/test` runs in **4 mallocs / 2 frees** total;
a full game is ~17 / 15 (1 malloc per sub-script `script_load`). If you
add a feature that multiplies this, look for an arena that fits first.

### Audio

`audio.c` is a tiny miniaudio wrapper: lazy `ma_engine_init` on first
play, synchronous playback (blocks until the clip ends so short SFX
don't cut off on exit), `audio_shutdown` via `atexit`. It's a separate
TU because `miniaudio.h` is 4 MB and shouldn't be in cmd.c's compile
loop. Prototypes are `extern`'d inline in `cmd.c` — there's no
`audio.h`.

### Builtins that are intentionally missing

Removed because no shipped `.cmd` uses them:

- `set /p VAR=PROMPT` (was only used by a deleted `write.cmd`)
- `for /f "tokens=..." / "usebackq"` option parsing (only `play_song.cmd`
  used this; the plain `for /f %%a in (file)` form still works)
- Dispatcher stubs for `pause`, `cls`, `title`, `color`, `ver`

If you add a new `.cmd` that needs any of these, re-implement the
matching feature in `cmd.c` rather than working around it in batch.

### Tests

`bin/main.cmd`'s `:tests` label iterates the script itself (`for /f %%a
in (%0) do ...`) and calls every label starting with `:test_`. Test
output is captured to `bin/tmp/test_output.txt`. Assertions are
`:assert_equal` / `:assert_*` helpers defined lower in `main.cmd`.
Adding a test = add another `:test_foo` label; no registration needed.

## House style (from `~/.claude/CLAUDE.md`)

- Simplicity over abstraction. Plain structs and direct functions.
- Data layout and allocation patterns matter more than code structure.
- Easy to delete beats easy to extend. Don't add defensive features or
  future-proofing — if nothing calls it, delete it.
- Correctness → Simplicity → Performance, in that order.
