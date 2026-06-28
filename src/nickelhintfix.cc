#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <NickelHook.h>

#include "config.h"
#include "util.h"

// Minimal FreeType type shims — only the fields we touch. The struct layouts
// match the device's FreeType so we can read face->family_name / face->glyph and
// the glyph outline/metrics.
typedef int FT_Error;
typedef signed int FT_Int;
typedef signed int FT_Int32;
typedef unsigned int FT_UInt;
typedef unsigned short FT_UShort;
typedef short FT_Short;
typedef long FT_Long;
typedef long FT_Pos;
typedef signed long FT_Fixed;
typedef void *FT_Library;

typedef struct FT_Vector_ {
    FT_Pos x;
    FT_Pos y;
} FT_Vector;

typedef struct FT_BBox_ {
    FT_Pos xMin;
    FT_Pos yMin;
    FT_Pos xMax;
    FT_Pos yMax;
} FT_BBox;

typedef struct FT_Bitmap_ {
    unsigned int rows;
    unsigned int width;
    int pitch;
    unsigned char *buffer;
    unsigned short num_grays;
    unsigned char pixel_mode;
    unsigned char palette_mode;
    void *palette;
} FT_Bitmap;

typedef struct FT_Outline_ {
    short n_contours;
    short n_points;
    FT_Vector *points;
    char *tags;
    short *contours;
    int flags;
} FT_Outline;

typedef struct FT_Generic_ {
    void *data;
    void *finalizer;
} FT_Generic;

typedef struct FT_Glyph_Metrics_ {
    FT_Pos width;
    FT_Pos height;
    FT_Pos horiBearingX;
    FT_Pos horiBearingY;
    FT_Pos horiAdvance;
    FT_Pos vertBearingX;
    FT_Pos vertBearingY;
    FT_Pos vertAdvance;
} FT_Glyph_Metrics;

typedef struct FT_GlyphSlotRec_ *FT_GlyphSlot;
typedef struct FT_FaceRec_ *FT_Face;

typedef struct FT_GlyphSlotRec_ {
    FT_Library library;
    FT_Face face;
    FT_GlyphSlot next;
    FT_UInt glyph_index;
    FT_Generic generic;
    FT_Glyph_Metrics metrics;
    FT_Fixed linearHoriAdvance;
    FT_Fixed linearVertAdvance;
    FT_Vector advance;
    int format;
    FT_Bitmap bitmap;
    FT_Int bitmap_left;
    FT_Int bitmap_top;
    FT_Outline outline;
} FT_GlyphSlotRec;

typedef struct FT_FaceRec_ {
    FT_Long num_faces;
    FT_Long face_index;
    FT_Long face_flags;
    FT_Long style_flags;
    FT_Long num_glyphs;
    char *family_name;
    char *style_name;
    FT_Int num_fixed_sizes;
    void *available_sizes;
    FT_Int num_charmaps;
    void *charmaps;
    FT_Generic generic;
    FT_BBox bbox;
    FT_UShort units_per_EM;
    FT_Short ascender;
    FT_Short descender;
    FT_Short height;
    FT_Short max_advance_width;
    FT_Short max_advance_height;
    FT_Short underline_position;
    FT_Short underline_thickness;
    FT_GlyphSlot glyph;
} FT_FaceRec;

static const int NHF_FT_GLYPH_FORMAT_OUTLINE = 0x6f75746c; // 'outl'
static const FT_Int32 NHF_FT_LOAD_NO_HINTING = 0x2;

static const char *const NHF_LIBKOBO = "/usr/local/Kobo/platforms/libkobo.so";

static FT_Error (*real_FT_Load_Glyph)(FT_Face, FT_UInt, FT_Int32);
static void (*p_FT_Outline_Get_CBox)(const FT_Outline*, FT_BBox*);

static bool nhf_safety_disabled = false;
static bool nhf_safety_log_dumped = false;

// --- Preferences ---------------------------------------------------------------

static bool nhf_enabled() {
    return nhf_global_config_bool("nhf_enabled", true);
}

static bool nhf_no_hinting() {
    return nhf_global_config_bool("nhf_no_hinting", true);
}

static bool nhf_fractional_cbox() {
    return nhf_global_config_bool("nhf_fractional_cbox", true);
}

// Comma-separated font families allowed to keep their own native hinting (i.e.
// exempt from nhf_no_hinting). Matched case-insensitively against the FT face.
static bool nhf_font_hinting_allowed(const char *family) {
    if (!family || !*family)
        return false;
    const char *list = nhf_global_config_get("nhf_hinting_allowlist");
    if (!list || !*list)
        return false;

    size_t flen = strlen(family);
    const char *p = list;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t')
            p++;
        const char *start = p;
        while (*p && *p != ',')
            p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        if ((size_t)(end - start) == flen && !strncasecmp(family, start, flen))
            return true;
    }
    return false;
}

// --- Safety --------------------------------------------------------------------

static bool nhf_path_exists(const char *path) {
    return !access(path, F_OK);
}

static void nhf_write_marker(const char *path, const char *message) {
    mkdir(NHF_CONFIG_DIR, 0755);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    if (message)
        fprintf(f, "%s\n", message);
    fclose(f);
}

// Disable for this boot only — never self-uninstalls. The disabled-by-safety
// marker keeps the mod inert on later boots until the user deletes it.
static void nhf_disable_for_safety(const char *reason) {
    if (nhf_safety_disabled)
        return;
    nhf_safety_disabled = true;
    NHF_LOG("SAFETY: disabling NickelHintFix for this boot: %s", reason ? reason : "unknown reason");
    nhf_write_marker(NHF_CONFIG_DIR "/disabled-by-safety", reason);
    if (!nhf_safety_log_dumped) {
        nhf_safety_log_dumped = true;
        nh_dump_log();
    }
}

static bool nhf_safety_marker_present() {
    static int present = -1;
    if (present == -1)
        present = nhf_path_exists(NHF_CONFIG_DIR "/disabled-by-safety") ? 1 : 0;
    return present == 1;
}

// --- Init / uninstall ----------------------------------------------------------

static int nhf_init() {
    nhf_global_config_get("");
    const char *allowlist = nhf_global_config_get("nhf_hinting_allowlist");
    NHF_LOG("startup: nhf_enabled=%d nhf_no_hinting=%d nhf_fractional_cbox=%d nhf_hinting_allowlist='%s'",
        nhf_enabled() ? 1 : 0,
        nhf_no_hinting() ? 1 : 0,
        nhf_fractional_cbox() ? 1 : 0,
        allowlist ? allowlist : "");
    if (nhf_safety_marker_present())
        NHF_LOG("startup: disabled-by-safety marker present; passing through");
    return 0;
}

static bool nhf_delete_file_if_exists(const char *path) {
    if (access(path, F_OK) && errno == ENOENT)
        return true;
    return nh_delete_file(path);
}

static bool nhf_delete_dir_if_exists(const char *path) {
    if (access(path, F_OK) && errno == ENOENT)
        return true;
    return nh_delete_dir(path);
}

static bool nhf_uninstall() {
    bool ok = true;
    NHF_LOG("uninstall: removing NickelHintFix files");
    ok = nhf_delete_file_if_exists(NHF_CONFIG_DIR "/doc") && ok;
    ok = nhf_delete_file_if_exists(NHF_CONFIG_DIR "/default") && ok;
    ok = nhf_delete_file_if_exists(NHF_CONFIG_DIR "/config") && ok;
    ok = nhf_delete_file_if_exists(NHF_CONFIG_DIR "/nickelhintfix.log") && ok;
    ok = nhf_delete_file_if_exists(NHF_CONFIG_DIR "/disabled-by-safety") && ok;
    ok = nhf_delete_file_if_exists(NHF_CONFIG_DIR "/uninstall") && ok;
    ok = nhf_delete_dir_if_exists(NHF_CONFIG_DIR) && ok;
    return ok;
}

static struct nh_info NickelHintFixInfo = {
    .name = "NickelHintFix",
    .desc = "Fix the iType grid-fit wobble by loading uninstructed glyphs unhinted.",
    // NickelClock-style: deleting the shipped 'uninstall' file (the uninstall_xflag)
    // triggers removal on the next boot. The uninstall_flag is a separate force path
    // that is normally absent.
    .uninstall_flag = NHF_CONFIG_DIR "/uninstall-now",
    .uninstall_xflag = NHF_CONFIG_DIR "/uninstall",
    .failsafe_delay = 3,
};

static struct nh_hook NickelHintFixHooks[] = {
    {
        .sym = "FT_Load_Glyph",
        .sym_new = "_nhf_FT_Load_Glyph",
        .lib = NHF_LIBKOBO,
        .out = nh_symoutptr(real_FT_Load_Glyph),
        .desc = "load glyphs unhinted and/or apply a fractional control box",
    },
    {0},
};

static struct nh_dlsym NickelHintFixSymbols[] = {
    {
        .name = "FT_Outline_Get_CBox",
        .out = nh_symoutptr(p_FT_Outline_Get_CBox),
        .desc = "outline control box for nhf_fractional_cbox",
        .optional = true,
    },
    {0},
};

NickelHook(
    .init = &nhf_init,
    .info = &NickelHintFixInfo,
    .hook = NickelHintFixHooks,
    .dlsym = NickelHintFixSymbols,
    .uninstall = &nhf_uninstall,
)

extern "C" __attribute__((visibility("default"))) FT_Error _nhf_FT_Load_Glyph(FT_Face face, FT_UInt glyph_index, FT_Int32 load_flags) {
    if (!real_FT_Load_Glyph) {
        nhf_disable_for_safety("original FT_Load_Glyph pointer was NULL");
        return 1;
    }
    if (nhf_safety_disabled || nhf_safety_marker_present())
        return real_FT_Load_Glyph(face, glyph_index, load_flags);

    const char *family = (face && face->family_name) ? face->family_name : "";

    // Load uninstructed glyphs without hinting so iType draws the raw
    // outline instead of grid-fitting it. Skipped for allow-listed families.
    FT_Int32 effective_flags = load_flags;
    if (nhf_enabled() && nhf_no_hinting() && !nhf_font_hinting_allowed(family))
        effective_flags |= NHF_FT_LOAD_NO_HINTING;

    FT_Error err = real_FT_Load_Glyph(face, glyph_index, effective_flags);

    // Fractional control box: copy the raw outline cbox back into the glyph's
    // vertical metrics, in case iType snaps the metrics/placement. Applies to all
    // glyph loads (it mainly matters for unhinted glyphs, whose outline is
    // already fractional).
    if (nhf_enabled() && nhf_fractional_cbox() && !err && face && face->glyph &&
        face->glyph->format == NHF_FT_GLYPH_FORMAT_OUTLINE && p_FT_Outline_Get_CBox) {
        FT_BBox cb = {0, 0, 0, 0};
        p_FT_Outline_Get_CBox(&face->glyph->outline, &cb);
        face->glyph->metrics.horiBearingY = cb.yMax;
        face->glyph->metrics.height = cb.yMax - cb.yMin;
    }

    return err;
}
