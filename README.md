# NickelHintFix

NickelHintFix is a [NickelHook](https://github.com/pgaskin/NickelHook) mod for Kobo eReaders that fixes a vertical **"wobble"** in the reader: with certain fonts, individual letters drift up or down by a pixel relative to the baseline, producing a visibly uneven line. The same fonts render cleanly on other devices.

It fixes the defect by loading affected glyphs **without hinting**, so Kobo's rasterizer (Monotype iType) draws the raw outline instead of grid-fitting it. iType stays the renderer for everything else, so this is a small, targeted change.

## The defect (what's actually happening)

Some fonts ship a **`gasp` table requesting grid-fitting at all sizes** but carry **no per-glyph hinting instructions** — the glyphs are unhinted outlines. A real-world example is **KF Adelph** (`unitsPerEm=1000`, a full `fpgm`/`prep`, `gasp = 0xFFFF→15`, and zero glyph instructions).

When such a font is used, **iType honors the `gasp` request and grid-fits the uninstructed outlines itself**, snapping each glyph's top and bottom to the pixel grid. That grid-fitting is **sub-pixel-position-sensitive**, so the *same letter* gets snapped to a different integer height depending on where it falls in a line. At a single font size, the letters `a`, `r`, `s`, `u` each render at both 26px and 27px, and `T` at both 39px and 40px — that inconsistency is the wobble, and it can be measured directly from the rasterized pixels.

**Stock and desktop FreeType ignore the `gasp` request** for uninstructed glyphs (no glyph instructions means nothing to execute), which is why the font looks fine on other devices. The grid-fitting happens in the iType driver at glyph-load, below the renderer, which is why swapping the renderer does not help but a load-time flag does.

## The fix

NickelHintFix hooks `FT_Load_Glyph` (in Kobo's `libkobo.so` platform plugin) and ORs in **`FT_LOAD_NO_HINTING`**, so iType emits the raw outline and the same letter renders at a consistent height. With the fix on, each affected letter renders at exactly one height and iType's grid-fit snapping is gone.

Native TrueType grid-fitting buys very little on Kobo's ~300 DPI panel, so applying this broadly is low-cost. An allow-list exempts any font you specifically want hinted.

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
| `nhf_fractional_cbox` | `1` | On every glyph load, copies the raw outline control box into the glyph's vertical metrics, in case iType snaps the metrics/placement. Mainly relevant for unhinted glyphs (whose outline is already fractional). |
| `nhf_hinting_allowlist` | *(empty)* | Comma-separated font families exempt from `nhf_no_hinting` (allowed to keep their own native hinting). Matched case-insensitively, e.g. `Georgia, Kobo Nickel`. |

Changes take effect on reboot.

## Uninstall

NickelClock-style: **delete the file** `KOBOeReader/.adds/nickelhintfix/uninstall` and reboot. NickelHook then removes the mod on the next boot. Deleting the whole `.adds/nickelhintfix/` directory works too.

The mod **never self-uninstalls on internal errors** — a safety trip only disables it for that boot and writes a `disabled-by-safety` marker.

## Safety

Two layers:

1. **NickelHook's startup failsafe** (`failsafe_delay=3`) renames the library out of the load path while Nickel starts; if Nickel crashes early, the library stays disabled for the next boot.
2. **NickelHintFix's own tripwire**: if a required FreeType API is missing, it stops changing FreeType for the boot, writes `disabled-by-safety`, and dumps a syslog snapshot — without uninstalling.
