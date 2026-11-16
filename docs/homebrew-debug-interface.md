# Talking to the emulator from homebrew

A Loopy program can tell an automated run what it is doing, and even steer the
emulated mouse, without any code that behaves differently on a real console.
This page lists the channels and the one emulator-specific protocol,
**autodrive**.

## Channels that work everywhere

- **Serial port 0.** `printf` over SCI0 is the usual debug console. A script
  can wait for text with `wait-until serial 0 "LEVEL 2"`. The bytes also
  appear in a `--verbose` log as `[Serial] port0 tx XX`. On a console nothing
  needs to be listening.
- **RAM.** Keep a state variable (level, room, a "done" flag) at a fixed
  address and let the script test it with `wait-until mem.b ADDR == VALUE` or
  `assert`. This is the cleanest channel: it costs nothing on hardware and
  needs no register.
- **A write to a spare register.** `wait-until write ADDR VALUE` catches a
  store as it happens. Choose a register your program does not use for
  display: the scroll registers of a background layer that is switched off
  are safe. Do **not** use the bitmap colour latch (`BM_COL_LATCH`,
  0x0C059050) as a scratch register: on a real console bit 8 of it turns on
  the colour buffer, so trace values corrupt the picture there.

## Autodrive: steering the mouse

Retail mouse games can be driven with `mouse move` and `mouse click` in a
script. A homebrew game can go further and steer the mouse to where it wants
the cursor, which makes mouse-driven test routes independent of the game's
pointer speed and acceleration.

Turn it on with `--autodrive` (`input.autodrive=true`); it also plugs in the
mouse at power-on. The cartridge publishes its cursor and its target in four
background-layer registers. **The program must not display BG0 or BG1 while
it uses autodrive**, because these registers then hold the protocol instead
of scroll and palette values. Until the magic value appears the emulator does
nothing, so the same ROM runs normally on a console or without
`--autodrive`.

| Register | Address | Name | Contents |
| --- | --- | --- | --- |
| BG0 scroll X | 0x0C05A002 | AP_CTL | `0x0A00` magic in bits 11-8; bit 0: target valid; bit 1: hold the left button; bit 2: hold the right button |
| BG0 scroll Y | 0x0C05A004 | AP_DBG | free for the cartridge (12 bits); logged at debug level whenever it changes |
| BG0 palette select | 0x0C05A00A | AP_CUR | where the cursor is now: `(y << 8) \| x` in game pixels |
| BG1 palette select | 0x0C05A00C | AP_TGT | where it should go: `(y << 8) \| x` |

Once a frame, while AP_CTL carries the magic, the emulator:

1. holds the left and right buttons exactly as bits 1 and 2 ask. Raise a bit,
   wait until your own input code has seen the press, drop the bit: one click
   per request, however long either side takes;
2. if bit 0 is set and AP_CUR has changed since it last moved the mouse,
   moves the mouse by `2 × (target − cursor)` counts, Y inverted, which takes
   a game that moves its cursor one pixel per two counts straight to the
   target. Publish AP_CUR again after your game has applied the motion and
   clamped the cursor; the next move corrects any error. If AP_CUR stays the
   same for 8 frames, the move is repeated, at most 4 times in a row.

A script can override the target with `mouse to X Y` while the cartridge
publishes its cursor; the cartridge's own AP_TGT counts again once the cursor
arrives.

A minimal cartridge side, called once per game frame after the cursor update:

```c
#define AP_CTL (*(volatile uint16_t *)0x0C05A002)
#define AP_DBG (*(volatile uint16_t *)0x0C05A004)
#define AP_CUR (*(volatile uint16_t *)0x0C05A00A)
#define AP_TGT (*(volatile uint16_t *)0x0C05A00C)

void apdrive_publish(int cur_x, int cur_y, int tgt_x, int tgt_y, int want_left, int step)
{
    AP_CUR = (uint16_t)((cur_y << 8) | cur_x);
    AP_TGT = (uint16_t)((tgt_y << 8) | tgt_x);
    AP_DBG = (uint16_t)(step & 0xFFF);
    AP_CTL = 0x0A00 | 0x001 | (want_left ? 0x002 : 0);
}
```

## Finding performance problems

- `--timing.model=console` charges every instruction what a console was
  measured to charge. The stock model only charges instruction fetches, so
  code that does a lot of data access, DMA or VDP work looks much faster in
  the emulator than it really is.
- `--profile FILE --profile.from=A --profile.to=B` writes the cycles spent at
  every program address between frames A and B. Match addresses to your
  source with your linker map or `addr2line`.
- `--strict` ends the run with exit code 2 the first time the program touches
  hardware the emulator does not implement, instead of carrying on with
  values that may not match a console.
