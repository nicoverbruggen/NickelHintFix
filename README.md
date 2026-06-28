# NickelHintFix

NickelHintFix is a [NickelHook](https://github.com/pgaskin/NickelHook) mod for Kobo eReaders that fixes a vertical **"wobble"** in the reader: with certain fonts, individual letters drift up or down by a pixel relative to the baseline, producing a visibly uneven line. The same fonts render cleanly on other devices.

It fixes the defect by loading affected glyphs **without hinting**, so Kobo's rasterizer (Monotype iType) draws the raw outline instead of grid-fitting it. iType stays the renderer for everything else, so this is a small, targeted change.

## The defect (what's actually happening)

The wobble comes from how Kobo's rasterizer, Monotype **iType**, treats fonts whose glyphs carry **no per-glyph hinting instructions** — plain unhinted outlines. A real-world example is **KF Adelph** (`unitsPerEm=1000`, a full `fpgm`/`prep`/`cvt`, but zero glyph instructions).

iType is registered as a hinting-capable driver, so whenever a glyph is loaded **with hinting requested — which Nickel does by default** — iType grid-fits the outline. For a glyph that has no instructions to follow, iType falls back to its **own automatic grid-fitting**, snapping the glyph's top to a whole pixel row. That snap is **sub-pixel-position-sensitive**, so the *same letter* lands on a different integer height depending on where it falls in a line. At a single font size, the letters `a`, `r`, `s`, `u` each render at both 26px and 27px, and `T` at both 39px and 40px — that inconsistency is the wobble, and it can be measured directly from the rasterized pixels.

Two natural assumptions about the font turn out to be wrong — confirmed by disassembling Kobo's `libfreetype.so` (which wraps iType):

- The font's **`gasp` table is not the cause.** iType parses `gasp` into the face but never reads it while rendering, so editing `gasp` (e.g. clearing the grid-fit request) changes nothing.
- The font's **`fpgm`/`prep`/`cvt` programs are not the cause either.** iType's instruction interpreter is the only consumer of those tables, and it is invoked only for glyphs that *have* instructions. Unhinted glyphs bypass it entirely, so stripping those tables changes nothing.

The actual trigger is simply that **hinting is requested at glyph-load**, and the only switch the engine honors is the runtime load flag. This also explains why the same fonts look fine elsewhere: **desktop/stock FreeType** renders them with its auto-hinter (position-stable) or the stock TrueType hinter — not iType's auto-gridfit. The snapping happens inside the iType driver at glyph-load, below the renderer, which is why swapping the renderer doesn't help but a load-time flag does.

## The fix

NickelHintFix hooks `FT_Load_Glyph` (in Kobo's `libkobo.so` platform plugin) and ORs in **`FT_LOAD_NO_HINTING`** before the real call (`0x208` → `0x20a`). That sets iType's internal "skip grid-fit" flag, so it emits the raw scaled outline with no snapping. Every instance of a glyph then has identical geometry and the same letter renders at exactly one height — confirmed on-device, where each affected letter collapses from two heights to one.

Hinting buys very little on Kobo's ~300 DPI panel (KOReader ignores it at this resolution too), so applying this broadly is low-cost. An allow-list exempts any font you specifically want to keep natively hinted.

## Build

Install Podman and build with NickelTC:

```sh
./build.sh
```

It writes `KoboRoot.tgz` (the install package) and `src/libnickelhintfix.so`.

## Install

Copy `KoboRoot.tgz` to the Kobo's `.kobo` folder, eject the device, and reboot. After installation, files live under `KOBOeReader/.adds/nickelhintfix/`:

- `config` — your settings (created from `default` on first boot)
- `nickelhintfix.log` — the USB-visible log
- `uninstall` — delete this file (see Uninstall) to remove the mod

## Configuration

Settings live in `KOBOeReader/.adds/nickelhintfix/config`:

| Key | Default | Meaning |
|-----|---------|---------|
| `nhf_enabled` | `1` | Enable or disable NickelHintFix. `0` makes every hook pass through, so the device renders exactly as if the mod were not installed. |
| `nhf_no_hinting` | `1` | The fix. `1` loads glyphs with `FT_LOAD_NO_HINTING` (no iType grid-fitting, so heights are consistent). `0` is stock behaviour. |
| `nhf_hinting_allowlist` | *(empty)* | Comma-separated font families exempt from `nhf_no_hinting` (allowed to keep their own native hinting). Matched case-insensitively, e.g. `Georgia, Kobo Nickel`. |

Changes take effect on reboot.

## Uninstall

NickelClock-style: **delete the file** `KOBOeReader/.adds/nickelhintfix/uninstall` and reboot. NickelHook then removes the mod on the next boot. Deleting the whole `.adds/nickelhintfix/` directory works too.

The mod **never self-uninstalls on internal errors** — a safety trip only disables it for that boot and writes a `disabled-by-safety` marker.

## Safety

Two layers:

1. **NickelHook's startup failsafe** (`failsafe_delay=3`) renames the library out of the load path while Nickel starts; if Nickel crashes early, the library stays disabled for the next boot.
2. **NickelHintFix's own tripwire**: if a required FreeType API is missing, it stops changing FreeType for the boot, writes `disabled-by-safety`, and dumps a syslog snapshot — without uninstalling.
