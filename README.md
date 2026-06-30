# NickelHintFix

NickelHintFix is a [NickelHook](https://github.com/pgaskin/NickelHook) mod for Kobo eReaders that fixes two font-rendering defects in the reader's old (4.x) WebKit/iType stack:

1. **Glyph "wobble"** — with certain fonts, individual letters drift up or down by a pixel relative to the baseline, producing a visibly uneven line. The same fonts render cleanly on other devices.
2. **Broken vertical (tategaki) CJK text** when "better typography" is enabled — Japanese/Chinese vertical books render with the long-vowel mark `ー`, brackets `「」`, and punctuation `、。` horizontal or mislaid.

The wobble is fixed by loading non-allowlisted glyphs **without hinting**, so Kobo's rasterizer (Monotype iType) draws the raw outline instead of grid-fitting it. The vertical-text breakage is fixed by withholding `optimizeLegibility` from vertical books so they stay on WebKit's vertical-capable rendering path. Both are small, targeted, per-condition changes; iType stays the renderer.

> [!NOTE]
> The binary observations in this README were made to understand and fix a font-rendering bug in the Kobo software 4.x font stack. They are included to document why NickelHintFix changes a FreeType glyph-load flag, and to keep the fix limited to that compatibility issue. This kind of investigation is only practical because Kobo maintains an open device ecosystem and allows SSH access to their devices, which I appreciate and strongly encourage as a developer. This project does not include or redistribute Kobo firmware, Kobo binaries, or Monotype iType code.

## The defect (what's actually happening)

The wobble comes from how Kobo's rasterizer, Monotype **iType**, treats fonts whose glyphs carry **no per-glyph hinting instructions** — plain unhinted outlines.

iType is registered as a hinting-capable driver, so whenever a glyph is loaded **with hinting requested — which Nickel does by default** — iType grid-fits the outline. For a glyph that has no instructions to follow, iType falls back to its **own automatic grid-fitting**, snapping the glyph's top to a whole pixel row. That snap is **sub-pixel-position-sensitive**, so the *same letter* lands on a different integer height depending on where it falls in a line. At a single font size, the letters `a`, `r`, `s`, `u` each render at both 26px and 27px, and `T` at both 39px and 40px — that inconsistency is the wobble, and it can be measured directly from the rasterized pixels.

Two natural assumptions about the font turn out to be wrong. The bug-fix analysis below checked Kobo's `libfreetype.so` (which wraps iType) to understand why the same font renders differently on the device:

- The font's **`gasp` table is not the cause.** In the inspected binaries, iType parses `gasp` into the face, but the glyph-rendering path does not appear to read it, so editing `gasp` (e.g. clearing the grid-fit request) changes nothing.
- The font's **`fpgm`/`prep`/`cvt` programs are not the cause either.** iType's instruction interpreter is the only consumer of those tables, and it is invoked only for glyphs that *have* instructions. Uninstructed glyphs bypass it entirely, so stripping those tables from the original uninstructed font changes nothing.

The actual trigger is simply that **hinting is requested at glyph-load** for an uninstructed glyph. For an unmodified font, the reliable engine-level fix is the runtime load flag. The same raw-outline result can also be achieved at the font level by adding no-op per-glyph instructions, which routes iType away from its bad uninstructed-glyph auto-gridfit path without changing the outlines. This also explains why the same fonts look fine elsewhere: **desktop/stock FreeType** renders them with its auto-hinter (position-stable) or the stock TrueType hinter — not iType's auto-gridfit. The snapping happens inside the iType driver at glyph-load, below the renderer, which is why swapping the renderer doesn't help but a load-time flag does.

## Binary validation

This compatibility analysis was done to understand and fix the font-rendering bug, and to keep the mod limited to the smallest useful runtime change. The observations below were checked against the Kobo binaries from a stock `KoboRoot` image:

- `usr/lib/libfreetype.so.6.6.2`
- `usr/local/Kobo/platforms/libkobo.so`

The glyph-load path looks like this:

```text
Nickel reader / WebKit layout
  -> Qt font engine: QFontEngineFT::loadGlyph (libkobo.so)
     -> QFontEngineFT::loadFlags
        base flags 0x200 + glyph flag 0x8 = 0x208
     -> NickelHintFix FT_Load_Glyph hook
        -> disabled or allow-listed:
           real FT_Load_Glyph(face, glyph, 0x208)
        -> nhf_no_hinting=1:
           real FT_Load_Glyph(face, glyph, 0x20a)
           adds FT_LOAD_NO_HINTING (0x2)
        -> Kobo libfreetype: FT_Load_Glyph @ 0x6f260
           tests 0x8002 load-flag mask
        -> iType driver load_glyph
           itype_driver_class + 0x48 -> target 0x7567d
           -> no per-glyph instructions + hinting requested:
              iType uninstructed auto-gridfit
              position-sensitive snap -> wobble
           -> per-glyph instructions + hinting requested:
              idrv_expand_stik
              iType bytecode interpreter
           -> hinting disabled:
              raw scaled outline
              no grid-fit snap -> stable geometry

gasp table:
  loaded and cached, but not read by this glyph-rendering path
```

The FreeType binary is not stock upstream FreeType alone. It contains Monotype iType symbols and strings including `itype_driver_class`, `itype_renderer_class`, `itype_raster`, `itype drv`, `itype renderer`, and `idrv_expand_stik`. The iType driver class is present at `.data.rel.ro:0xd0e9c`; its module flags are `0x401`, matching `FT_MODULE_FONT_DRIVER | DRIVER_HAS_HINTER`, and its glyph-load function pointer resolves to the iType glyph loader at `0x7567d`.

The main FreeType entry point, `FT_Load_Glyph` at `0x6f260`, makes the load-flag decision before handing control to the font driver. At `0x6f2fc` it builds the mask `0x8002`, which is `FT_LOAD_NO_AUTOHINT | FT_LOAD_NO_HINTING`, and at `0x6f302` it branches away from the auto-hinter when either bit is set. For iType-driven fonts, the auto-hinter path is not selected; the call continues through the driver's `load_glyph` slot, with the caller's load flags passed through to iType.

Inside the iType glyph loader, the important branch is the per-glyph instruction-count check:

```text
0x757a0  ldrsh.w r3, [glyph, #0x28]  ; per-glyph instruction count
0x757a4  cmp     r3, #0
0x757a6  ble     0x7585e              ; zero instructions: skip interpreter path
0x757a8  ldr     r3, [r6, #0x18]
0x757aa  ldr     r3, [r3, #0x18]
0x757ac  lsls    r2, r3, #0x1e       ; test scaler flags
0x757ae  bpl     0x7585e              ; hinting disabled: skip interpreter path
0x757c6  blx     idrv_expand_stik     ; instructed + hinted glyphs reach interpreter
```

That is the core mechanism observed in the inspected binaries: uninstructed glyphs do not enter the bytecode interpreter, while instructed glyphs can. Adding a no-op per-glyph instruction therefore changes which iType path the glyph takes without changing the outline. Adding `FT_LOAD_NO_HINTING` changes the scaler flags instead, making iType skip grid-fitting and emit the raw scaled outline.

The `gasp` table was also checked. The table tag `0x67617370` is recognized by the sfnt table-loading code, and the loader at `0x947b8` caches `gasp` fields on the face. `FT_Get_Gasp` at `0x7187c` reads those cached fields when explicitly asked. But the glyph-load and rendering path does not call `FT_Get_Gasp`, and no rendering-path read of the cached `gasp` fields was found in this analysis. This matches the practical result: editing `gasp` does not change the wobble.

`libkobo.so` is where Nickel's Qt font engine calls into this FreeType build. It imports `FT_Load_Glyph`, and the `QFontEngineFT::loadGlyph` path calls it through the PLT. `QFontEngineFT` initializes its base load flags to `0x200`, then the glyph path ORs in `0x8` before the normal call, producing the observed `0x208` load flags. NickelHintFix adds `FT_LOAD_NO_HINTING` (`0x2`) to that value, so the affected call becomes `0x20a`.

Static binary inspection supports the control flow, flags, and table access described above. The exact pixel-height examples are runtime evidence from rasterized output rather than facts the binary alone can prove.

## The fix

NickelHintFix hooks `FT_Load_Glyph` (in Kobo's `libkobo.so` platform plugin) and ORs in **`FT_LOAD_NO_HINTING`** before the real call (`0x208` -> `0x20a`). That sets iType's internal "skip grid-fit" flag, so it emits the raw scaled outline with no snapping. Every instance of a glyph then has identical geometry and the same letter renders at exactly one height — observed on-device, where each affected letter collapses from two heights to one.

Hinting buys very little on Kobo's ~300 DPI panel (KOReader ignores it at this resolution too), so applying this broadly is low-cost. An allow-list exempts any font you specifically want to keep natively hinted.

## The vertical-text fix

Kobo's "better typography" toggle sets `webkitTextRendering=optimizeLegibility` in `eReader.conf`, which Nickel injects as `text-rendering: optimizeLegibility` into every book's stylesheet. In the device's old Qt 5.2 WebKit that forces the **complex text path** (`QTextLayout`-based), which has **no vertical-writing-mode support** — so vertical (tategaki) books lose per-glyph vertical orientation: the long-vowel mark `ー`, brackets `「」`, and ideographic punctuation `、。` come out in their horizontal forms. WebKit's **simple path** renders vertical text correctly; the only reason the book lands on the broken path is the injected `optimizeLegibility`.

Rather than fight that injected value, NickelHintFix overrides it on the **live page**: when a view's writing mode is applied (`CustomWebView::setWritingDirection`) and it's vertical, it pushes a tiny **user stylesheet** — `*{text-rendering:auto !important}` — onto that page's `QWebSettings`. A *user-origin* `!important` rule outranks the author's `optimizeLegibility`, so WebKit re-cascades and re-renders the page on the simple path immediately. For non-vertical views the stylesheet is cleared, so horizontal books keep "better typography" untouched.

This indirection is deliberate, and was found by on-device tracing (see the kobo-font-investigation reports): the obvious seams don't work. `KepubBookReader::pageStyleCss`/`setWritingDirection` are *virtual* methods — NickelHook can only patch PLT (`JUMP_SLOT`) entries, not vtable slots — so they can't be hooked. And `ReadingSettings::getWebkitTextRendering` (the value source) is read *before* the book's vertical writing-mode is ever parsed, and never re-read, so rewriting its return is futile. Overriding the already-rendered page once vertical *is* known is the reliable path. The vertical enum values are still derived at runtime from Nickel's own `writingDirectionFromString` (no hardcoded magic numbers).

> [!NOTE]
> This keys on the book's **writing mode, not its language** — so it applies to *any* vertical book (Japanese, Traditional Chinese, etc.), and leaves *all* horizontal books alone (they don't hit this bug, in any language). That's the correct discriminator: the defect is specific to vertical writing mode, not to a particular `lang`.

Every Nickel symbol it hooks or resolves is marked **optional**: if a firmware update renames any of them, the vertical fix simply goes inert and the hinting fix above is unaffected.

## Build

Install Podman and build with NickelTC:

```sh
git submodule update --init
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
| `nhf_vertfix` | `1` | The vertical-text fix. `1` pushes `text-rendering:auto` onto vertical (tategaki) pages so they render correctly under "better typography". `0` leaves Nickel's behaviour unchanged. |

Changes take effect on reboot.

## Compatibility

Both fixes patch the **Nickel/WebKit/iType software stack, not the panel hardware**, so this is **not specific to any one Kobo** — it should work across the device line (B&W or colour) on a compatible firmware. The real variable is the firmware **version**, not the model.

It binds to **exact symbols** — there are no hardcoded offsets or magic numbers. The vertical writing-mode enum values are derived at runtime from Nickel's own `writingDirectionFromString`, and the page's `QWebSettings` is reached by *calling* `CustomWebView::settings()` rather than assuming a struct layout. So a firmware that merely shuffles internals still works.

A firmware update **could** break a hook only by renaming/refactoring the symbols it binds (the C++ method names, the `KepubBookReader`/`CustomWebView`/`ReadingSettings` classes, or — very unlikely — the frozen Qt 5.2 `QWebSettings` API). That's uncommon for routine releases, and the same maintenance reality every Kobo mod lives with (NickelMenu, NickelClock, …). When it does happen, it **fails safe**:

- Every vertical-fix symbol is `optional` — if one goes missing, the vertical fix logs it and goes **inert**; the hinting fix and the device keep working.
- The startup log makes diagnosis a glance: `startup: vertical-fix syms: cwvSetDir=… cwvSettings=… setUserCss=… kepubCtor=… wdFromString=…`. Any `(nil)` is the symbol that moved.
- The `FT_Load_Glyph` hinting hook is the one hard dependency, but it's a plain, long-stable FreeType C symbol; if it ever failed, the NickelHook failsafe (see Safety) disables the whole mod for the next boot rather than risking the device.

## Uninstall

NickelClock-style: **delete the file** `KOBOeReader/.adds/nickelhintfix/uninstall` and reboot. NickelHook then removes the mod on the next boot. Deleting the whole `.adds/nickelhintfix/` directory works too.

The mod **never self-uninstalls on internal errors** — a safety trip disables it immediately, writes a `disabled-by-safety` marker, and keeps it inert on later boots until that marker is removed.

## Safety

Two layers:

1. **NickelHook's startup failsafe** (`failsafe_delay=3`) renames the library out of the load path while Nickel starts; if Nickel crashes early, the library stays disabled for the next boot.
2. **NickelHintFix's own tripwire**: if a required FreeType API is missing, it stops changing FreeType, writes `disabled-by-safety`, and dumps a syslog snapshot — without uninstalling.
