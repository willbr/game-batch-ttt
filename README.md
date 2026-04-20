# game-batch-ttt

Tic-tac-toe written in Windows NT `.cmd` batch script.

The game itself (`bin/main.cmd` and friends) is real `cmd.exe` batch; on
Windows you can double-click `tictactoe.cmd` and play. For development on
macOS / Linux, `cmd.c` is a small `cmd.exe` emulator that runs the same
scripts unmodified.

## Layout

```
tictactoe.cmd     top-level launcher (pushd bin && main.cmd)
bin/
  main.cmd        game + :tests target
  drawgrid.cmd    render the 3x3 board
  play_sound.cmd  invokes sndrec32 with a path under c:\windows\media
  sleep.cmd       ping-based sleep
  reply.com       14-byte DOS .COM that reads one keystroke into errorlevel
  reply.debug_asm / build_reply_com.cmd  -- regenerate reply.com via DOS debug
  test_reply.cmd  interactive input test
cmd.c             cmd.exe emulator (host side)
audio.c           miniaudio wrapper, plays wavs when sndrec32 is invoked
miniaudio.h       vendored single-header audio lib
```

## Build and run (macOS)

```
make             # builds ./cmd
make media       # converts /System/Library/Sounds/Hero.aiff -> bin/media/tada.wav
make play        # plays interactively
make test        # runs the in-script unit tests
```

Linker needs CoreFoundation / CoreAudio / AudioToolbox; the Makefile picks
those up on Darwin automatically.

## Controls

The key map matches the 3x3 physical layout on a QWERTY keyboard:

```
q w e
a s d
z x c
```

`Esc` or `Ctrl-C` quits.

## How the emulator works

`cmd.c` parses the `.cmd` source into statements (paren-aware), then
interprets them in a recursive walker. Variables, labels, `call`, `for`,
`if`, `set /a`, redirection, and delayed expansion all work well enough
for this game.

Externals it fakes:

| command    | behaviour                                                    |
|------------|--------------------------------------------------------------|
| `reply`    | reads one key, sets `errorlevel` to the keycode              |
| `ping -w`  | sleeps for N ms when `-w N` is present                       |
| `start`    | no-op, except `start sndrec32 ...` routes to audio playback  |
| `sndrec32` | plays the `.wav` path argument via miniaudio                 |
| `debug`    | no-op (only relevant to `build_reply_com.cmd`)               |

`c:\windows\media\NAME.wav` is rewritten to `media/NAME.wav` relative to
cwd so `play_sound.cmd`'s `if exist` check passes and the correct local
wav is handed to miniaudio.

## Memory model

- One bump arena per Script (`path`, statement text, label arrays) --
  `script_free` is a single `free`.
- One global scratch arena for per-statement transient strings, with
  mark/reset around each statement so nested `call`s nest naturally.
- Var structs and name strings live in a var arena; short values live in
  a 56-byte inline buffer per Var, and the heap buffer is reused across
  updates once grown.
- `pushd`/`popd` uses fixed inline storage.

A full test run does 4 mallocs / 2 frees total; a full game does ~17 / 15
(one malloc per sub-script loaded).

## Non-goals

Not a complete `cmd.exe` implementation. `setlocal ENABLEDELAYEDEXPANSION`
is a no-op because delayed expansion is always on; there's no pipe
support; `for /f` ignores `tokens=` / `usebackq` options; redirections
understand the common cases but nothing fancy.

The `.cmd` files are the authoritative game -- the emulator exists to
make them runnable without booting Windows.
