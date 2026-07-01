# NickelTypeFix

A [NickelHook](https://github.com/pgaskin/NickelHook) mod for Kobo eReaders that fixes several
**text-rendering defects** in the reader's old Qt 5.2 / QtWebKit / Monotype iType stack. Each fix
is independent and fail-safe — it engages only where its seam exists and sits out safely
otherwise. Nothing is written to any device library on disk; a boot without the mod is stock.

> Firmware **4.x only** (inert on 5.x). See [Compatibility](#compatibility).

## What it fixes

1. **Glyph "wobble"** — letters that drift a pixel up/down, giving an uneven line, on fonts with
   no hinting instructions. → loads them unhinted so iType stops grid-fitting inconsistently.
2. **Vertical (tategaki) CJK text** rendering sideways/misplaced under `optimizeLegibility`. →
   keeps vertical books on WebKit's correct rendering path.
3. **Justified kepubs breaking at sentence boundaries** (uneven gaps) under `optimizeLegibility` —
   the main justification fix. → corrects Qt's justifier so the boundary space gets its share.
4. **Justification skewing around punctuation** (em/en dashes, ellipses, curly quotes). →
   secondary justify fix.

Ligatures/kerning stay on throughout — the point is to keep `optimizeLegibility` *and* have text
render correctly. Cause + mechanism for each fix is in **[docs/how-it-works.md](docs/how-it-works.md)**.

## Prerequisite: enable optimizeLegibility

Fix 1 (glyph wobble) is the standout — it's independent of everything below, needs no
configuration, and is arguably the biggest single improvement the mod makes. It just works.

Fixes 2–4, by contrast, only do anything when Kobo's WebKit **`optimizeLegibility`** text-rendering
path is turned on — that's the path they correct. It's off by default and is a manual opt-in in the
Kobo config file (**not** a UI setting). Edit `KOBOeReader/.kobo/Kobo/Kobo eReader.conf` and add:

    [Reading]
    webkitTextRendering=optimizeLegibility

Then reboot. With this off, the vertical and justification fixes will correctly log that they
engaged, but you won't see a difference because the broken render path is never taken — while Fix 1
keeps improving every book regardless.

## Screenshots

<!-- TODO: drop before/after images into docs/img/ and they'll render here -->
| | before | after |
|---|---|---|
| Justified kepub (Fix 3) | ![justify before](docs/img/justify-before.png) | ![justify after](docs/img/justify-after.png) |
| Glyph wobble (Fix 1) | ![wobble before](docs/img/wobble-before.png) | ![wobble after](docs/img/wobble-after.png) |
| Vertical text (Fix 2) | ![vertical before](docs/img/vertical-before.png) | ![vertical after](docs/img/vertical-after.png) |

## Configuration

Settings live in `KOBOeReader/.adds/nickel-type-fix/config` (auto-created with these defaults on
first boot — there's no shipped template file). Changes take effect on reboot.

| Key | Default | Meaning |
|-----|---------|---------|
| `ntf_enabled` | `1` | Master switch. `0` = behaves as if not installed. |
| `ntf_no_hinting` | `1` | Fix 1 (wobble): load glyphs unhinted. `0` = stock. |
| `ntf_hinting_allowlist` | *(empty)* | Comma-separated font families to keep natively hinted, e.g. `Georgia, Kobo Nickel`. |
| `ntf_vertfix` | `1` | Fix 2 (vertical text). |
| `ntf_justify_kospan` | `1` | Fix 3 (koboSpan-boundary justification — the main one). |
| `ntf_justify_punct` | `1` | Fix 4 (punctuation justification). |
| `ntf_log` | `1` | Verbose per-fix logging to `nickel-type-fix.log`. `0` to quieten. |

Each fix logs whether it engaged, so one boot tells the whole story.

## Compatibility

Requires Kobo firmware **4.21+ (the 4.x series: Qt 5.2 / QtWebKit / iType)**. It does **not** work
on 5.x (Qt6 / Chromium — no iType, no QtWebKit, and NickelHook doesn't load there).

It's **not** tied to any one model — the two in-memory patches (Fixes 3–4) anchor to
position-independent instruction patterns, verified byte-identical across the 4.38 and 4.45
firmware branches (Sage, Elipsa, Libra 2, Clara 2E … and Clara BW/Colour, Libra Colour). Where a
pattern or symbol isn't found on a given firmware, that fix logs and sits out — the mod fails
safe, never breaks the device.

## Build

Install Podman/Docker and build with NickelTC:

```sh
git submodule update --init
./build.sh          # → KoboRoot.tgz + src/libnickeltypefix.so
```

## Install

Copy `KoboRoot.tgz` to the Kobo's `.kobo` folder, eject, and reboot. On boot the mod also removes
the older standalone mods it supersedes (NickelHintFix, NickelJustifyFix) so they don't co-load.

## Uninstall

Delete `KOBOeReader/.adds/nickel-type-fix/uninstall` and reboot — NickelHook removes the mod on the
next boot. The in-memory patches revert automatically (nothing was written to disk).
