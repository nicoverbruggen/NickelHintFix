# NickelTypeFix

A [NickelHook](https://github.com/pgaskin/NickelHook) mod for Kobo eReaders that fixes several
**text-rendering defects** in the reader's old Qt 5.2 / QtWebKit / Monotype iType stack. 

Each fix is independent and fail-safe. Individual fixes engage only if they can safely be applied, or otherwise don't apply. You can disable individual fixes via a configuration file in `.adds/nickel-type-fix`.

> [!IMPORTANT]
> This mod works on Kobo software version **4.x only** (inert on 5.x). See [Compatibility](#compatibility) for more information about compatibility.

## What it fixes

1. **Glyph "wobble"**: letters that drift a pixel up/down, giving an uneven line, on fonts with no hinting instructions. 
→ loads unhinted fonts, so iType stops grid-fitting inconsistently.
2. **Vertical (tategaki) CJK text** rendering sideways/misplaced under `optimizeLegibility`. 
→ keeps vertical books on WebKit's correct rendering path.
3. **Justified kepubs breaking at sentence boundaries** (uneven gaps) under `optimizeLegibility`. the main justification fix. 
→ corrects Qt's justifier so the boundary space gets its share.
4. **Justification skewing around punctuation** (em/en dashes, ellipses, curly quotes). 
→ secondary justification fix.

## Why was this made?

**This fixes what is usually broken when you enable `optimizeLegibility`, which is justification and vertical CJK text.**

The point is to keep `optimizeLegibility` (which gets you ligatures, better text rendering, and optionally hyphenation) without any bugs. The cause of the bugs and the mechanism for each fix is [documented here](ABOUT.md).

## Prerequisite: enable `optimizeLegibility`

The first fix (glyph wobble) is the standout, and has been my personal pet peeve with Kobo's renderer. This fix is independent of everything below, needs no configuration, and is arguably the biggest single improvement the mod makes. It just works for every font. Yay!

The other fixes, by contrast, only do anything when Kobo's WebKit **`optimizeLegibility`** text-rendering path is turned on. It's off by default and is a manual opt-in in the Kobo config file (**not** a UI setting). Edit `KOBOeReader/.kobo/Kobo/Kobo eReader.conf` and add:

    [Reading]
    webkitTextRendering=optimizeLegibility

Then reboot. With this off, the vertical and justification fixes will correctly log that they engaged, but you won't see a difference because the broken render path is never taken. You now get the following with `optimizeLegibility` set:

- Working GPOS functionality w/ fonts (improved tracking and kerning)
- Hyphenation and ligatures (advanced font features)
- Working justification (*fixed with this mod)
- Working vertical text rendering (*fixed with this mod)

## Screenshots

These are actual page captures from the author's own **Kobo Clara BW** before and after installing the mod.

The middle **diff** overlays the two: **red** is ink the fix removed (its old position), **green** is ink the fix added (its new position), white is unchanged. This way, the effect is obvious even where it's subtle on the page.

### 1. Glyph outline rendering fix ("wobble" fix)

Letters can drift exactly one pixel off the baseline; the diff lights up nearly every glyph the unhinting re-rasterizes:

| original | diff | fixed |
|---|---|---|
| ![wobble original](docs/screenshots/wobble.png) | ![wobble diff](docs/highlight/wobble-diff.png) | ![wobble fixed](docs/screenshots/wobble-free.png) |

### 2. Vertical text orientation fix

CJK punctuation (`、` `。`) and small kana float centered in the
cell instead of tucking to the top-right where vertical Japanese needs them:

| original | diff | fixed |
|---|---|---|
| ![vertical original](docs/screenshots/cjk-broken.png) | ![vertical diff](docs/highlight/cjk-diff.png) | ![vertical fixed](docs/screenshots/cjk-correct.png) |

### 3. Justification fix

Most noticeable: a starved gap at the sentence boundary (`justification   maths.`) with the rest of the line over-stretched, vs. even word spacing:

| original | diff | fixed |
|---|---|---|
| ![justify original](docs/screenshots/justification-broken.png) | ![justify diff](docs/highlight/justify-diff.png) | ![justify fixed](docs/screenshots/justification-correct.png) |

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

Requires Kobo firmware **4.21+ (the 4.x series, which uses Qt 5.2 / QtWebKit / iType)**. 

**It does not work on 5.x (Qt6 / Chromium — no iType, no QtWebKit, and NickelHook doesn't load there).**

The mod is not tied to any one model. The two in-memory patches related to justification anchor to position-independent instruction patterns, verified byte-identical across the 4.38 and 4.45 firmware branches (Sage, Elipsa, Libra 2, Clara 2E … and Clara BW/Colour, Libra Colour).

## Safety

There are two independent layers of protection, so a failure at worst sits a single fix out — and at absolute worst cannot brick the device.

**Whole-mod boot failsafe.** Before any hook or in-memory patch is applied, NickelHook renames the plugin to `libnickeltypefix.so.failsafe`, and only renames it back three seconds *after* Nickel has started successfully. If applying the hooks or the justification patches ever crashes or hangs Nickel during boot, that rename-back never runs: on the next boot the plugin is no longer at its load path, the mod stays disengaged, and the boot loop is broken automatically. No user action is needed to recover.

**Per-fix graceful degradation.** Each fix engages only if it can be applied safely, and a failure in one never affects the others:

- Hooked and looked-up symbols are optional — if a symbol isn't present on a given firmware, that fix simply sits out instead of aborting the mod.
- Where a justification fix can't locate its instruction pattern, or the bytes at a target site aren't what's expected, the fix logs and is skipped. When it does apply, all of its edits are located and verified up front and are written both-or-nothing (a mid-write failure rolls the already-patched sites back).
- The hinting fix carries a persistent `disabled-by-safety` marker: if `FT_Load_Glyph` is ever unexpectedly unavailable at runtime, it records the marker and passes glyphs through untouched on this and every later boot, leaving the vertical and justification fixes running.

## Build

Install Podman/Docker and build with NickelTC:

```sh
git submodule update --init
./build.sh          # → KoboRoot.tgz + src/libnickeltypefix.so
```

## Install

Copy `KoboRoot.tgz` to the Kobo's `.kobo` folder, eject, and reboot. 

On boot the mod also removes the older standalone mods it supersedes (NickelHintFix, NickelJustifyFix) so they don't co-load.

## Uninstall

Delete `KOBOeReader/.adds/nickel-type-fix/uninstall` and reboot — NickelHook removes the mod on the next boot. The in-memory patches revert automatically (nothing was written to disk).

## Development

This repository was created with the assistance of large language models (specifically, Opus 4.8 and GPT 5.5). All of it was carefully reviewed by the author, and tested on the author's actual Kobo devices before release.
