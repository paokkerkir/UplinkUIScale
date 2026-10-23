// UplinkUIScale - resolution independent UI scaling for Uplink.
//
// Works with any Windows build of Uplink (original, GOG, Steam) because it only
// touches the game's import table entries for SDL 1.2 and OpenGL 1.1 by name;
// no hardcoded addresses.
//
// How it works:
//   * The game is told (via SDL_ListModes) that the only available video mode is
//     a "logical" resolution, e.g. 1365x768 on a 2560x1440 display. The game
//     lays its UI out for that size, so everything is as big as at 1024x768.
//   * The real window is created at the output resolution and the GL viewport
//     covers it, so all vector drawing (quads, lines, textures) is scaled.
//   * Everything OpenGL does in window pixels is fixed up: glScissor,
//     glLineWidth, glLineStipple, glReadPixels, glBitmap (text glyphs) and
//     glDrawPixels (images). Mouse coordinates are mapped back to logical.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_BGR_EXT
#define GL_BGR_EXT 0x80E0
#define GL_BGRA_EXT 0x80E1
#endif

// ---------------------------------------------------------------------------
// version.dll proxy

static HMODULE g_realVersion;
static FARPROC g_verProcs[17];
static const char *kVerNames[17] = {
    "GetFileVersionInfoA",     "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
    "GetFileVersionInfoExW",   "GetFileVersionInfoSizeA",    "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",  "GetFileVersionInfoW",
    "VerFindFileA",            "VerFindFileW",               "VerInstallFileA",
    "VerInstallFileW",         "VerLanguageNameA",           "VerLanguageNameW",
    "VerQueryValueA",          "VerQueryValueW"};

#define VER_PROXY(i, name) \
    extern "C" __declspec(naked) void Proxy_##name() { __asm { jmp dword ptr [g_verProcs + 4 * i] } }

VER_PROXY(0, GetFileVersionInfoA)
VER_PROXY(1, GetFileVersionInfoByHandle)
VER_PROXY(2, GetFileVersionInfoExA)
VER_PROXY(3, GetFileVersionInfoExW)
VER_PROXY(4, GetFileVersionInfoSizeA)
VER_PROXY(5, GetFileVersionInfoSizeExA)
VER_PROXY(6, GetFileVersionInfoSizeExW)
VER_PROXY(7, GetFileVersionInfoSizeW)
VER_PROXY(8, GetFileVersionInfoW)
VER_PROXY(9, VerFindFileA)
VER_PROXY(10, VerFindFileW)
VER_PROXY(11, VerInstallFileA)
VER_PROXY(12, VerInstallFileW)
VER_PROXY(13, VerLanguageNameA)
VER_PROXY(14, VerLanguageNameW)
VER_PROXY(15, VerQueryValueA)
VER_PROXY(16, VerQueryValueW)

static void LoadRealVersionDll()
{
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (n == 0 || n + 13 >= MAX_PATH) return;
    strcat(path, "\\version.dll");
    g_realVersion = LoadLibraryA(path);
    if (!g_realVersion) return;
    for (int i = 0; i < 17; ++i) g_verProcs[i] = GetProcAddress(g_realVersion, kVerNames[i]);
}

// ---------------------------------------------------------------------------
// Config / log

enum Filter { FILTER_AUTO, FILTER_SMOOTH, FILTER_SHARP, FILTER_LINEAR };
enum Mode { MODE_AUTO, MODE_FULLSCREEN, MODE_WINDOWED, MODE_BORDERLESS };

static struct Config {
    bool enabled = true;
    bool log = false;
    int width = 0, height = 0;
    int mode = MODE_AUTO;
    bool dpiAware = true;
    float scale = 0.0f;
    float multiplier = 1.0f;
    bool integerScale = false;
    int baseWidth = 1024, baseHeight = 768;
    int textFilter = FILTER_AUTO;
    float textBoldness = 0.05f;
    int imageFilter = FILTER_AUTO;
    float lineScale = 1.0f;
    bool cursor = true;
    float cursorSize = 1.0f;
} cfg;

static char g_dir[MAX_PATH];
static FILE *g_log;

static void Log(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static std::string IniStr(const char *ini, const char *sec, const char *key, const char *def)
{
    char buf[256];
    GetPrivateProfileStringA(sec, key, def, buf, sizeof(buf), ini);
    // Strip trailing comments and whitespace
    char *c = strpbrk(buf, ";#");
    if (c) *c = 0;
    size_t len = strlen(buf);
    while (len && (buf[len - 1] == ' ' || buf[len - 1] == '\t')) buf[--len] = 0;
    return buf;
}

static int IniInt(const char *ini, const char *sec, const char *key, int def)
{
    std::string s = IniStr(ini, sec, key, "");
    return s.empty() ? def : atoi(s.c_str());
}

static float IniFloat(const char *ini, const char *sec, const char *key, float def)
{
    std::string s = IniStr(ini, sec, key, "");
    return s.empty() ? def : (float)atof(s.c_str());
}

static int ParseFilter(const std::string &s)
{
    if (!_stricmp(s.c_str(), "smooth")) return FILTER_SMOOTH;
    if (!_stricmp(s.c_str(), "sharp")) return FILTER_SHARP;
    if (!_stricmp(s.c_str(), "linear")) return FILTER_LINEAR;
    return FILTER_AUTO;
}

static void LoadConfig()
{
    char ini[MAX_PATH];
    _snprintf(ini, MAX_PATH, "%s\\UplinkUIScale.ini", g_dir);
    ini[MAX_PATH - 1] = 0;

    cfg.enabled = IniInt(ini, "General", "Enabled", 1) != 0;
    cfg.log = IniInt(ini, "General", "Log", 0) != 0;

    cfg.width = IniInt(ini, "Display", "Width", 0);
    cfg.height = IniInt(ini, "Display", "Height", 0);
    std::string m = IniStr(ini, "Display", "Mode", "auto");
    cfg.mode = !_stricmp(m.c_str(), "fullscreen") ? MODE_FULLSCREEN
             : !_stricmp(m.c_str(), "windowed")   ? MODE_WINDOWED
             : !_stricmp(m.c_str(), "borderless") ? MODE_BORDERLESS
                                                  : MODE_AUTO;
    cfg.dpiAware = IniInt(ini, "Display", "DPIAware", 1) != 0;

    cfg.scale = IniFloat(ini, "Scale", "Scale", 0.0f);
    cfg.multiplier = IniFloat(ini, "Scale", "Multiplier", 1.0f);
    cfg.integerScale = IniInt(ini, "Scale", "IntegerScale", 0) != 0;
    cfg.baseWidth = IniInt(ini, "Scale", "BaseWidth", 1024);
    cfg.baseHeight = IniInt(ini, "Scale", "BaseHeight", 768);

    cfg.textFilter = ParseFilter(IniStr(ini, "Rendering", "TextFilter", "auto"));
    cfg.textBoldness = IniFloat(ini, "Rendering", "TextBoldness", 0.05f);
    cfg.imageFilter = ParseFilter(IniStr(ini, "Rendering", "ImageFilter", "auto"));
    cfg.lineScale = IniFloat(ini, "Rendering", "LineScale", 1.0f);

    cfg.cursor = IniInt(ini, "Cursor", "ScaleCursor", 1) != 0;
    cfg.cursorSize = IniFloat(ini, "Cursor", "CursorSize", 1.0f);

    if (cfg.multiplier <= 0.0f) cfg.multiplier = 1.0f;
    if (cfg.baseWidth < 320) cfg.baseWidth = 1024;
    if (cfg.baseHeight < 240) cfg.baseHeight = 768;
    if (cfg.textBoldness < 0.0f) cfg.textBoldness = 0.0f;
    if (cfg.textBoldness > 0.4f) cfg.textBoldness = 0.4f;
    if (cfg.lineScale <= 0.0f) cfg.lineScale = 1.0f;
    if (cfg.cursorSize <= 0.0f) cfg.cursorSize = 1.0f;
}

// ---------------------------------------------------------------------------
// SDL 1.2 types (only the parts we touch)

#define SDL_OPENGL 0x00000002u
#define SDL_RESIZABLE 0x00000010u
#define SDL_NOFRAME 0x00000020u
#define SDL_FULLSCREEN 0x80000000u
#define SDL_MOUSEMOTION 4
#define SDL_MOUSEBUTTONDOWN 5
#define SDL_MOUSEBUTTONUP 6
#define SDL_APPMOUSEFOCUS 0x01

struct SDL_Rect { int16_t x, y; uint16_t w, h; };
struct SDL_MouseMotionEvent { uint8_t type, which, state; uint16_t x, y; int16_t xrel, yrel; };
struct SDL_MouseButtonEvent { uint8_t type, which, button, state; uint16_t x, y; };

typedef void *(__cdecl *PFN_SDL_SetVideoMode)(int, int, int, uint32_t);
typedef int(__cdecl *PFN_SDL_VideoModeOK)(int, int, int, uint32_t);
typedef SDL_Rect **(__cdecl *PFN_SDL_ListModes)(void *, uint32_t);
typedef int(__cdecl *PFN_SDL_PollEvent)(void *);
typedef uint8_t(__cdecl *PFN_SDL_GetMouseState)(int *, int *);
typedef void(__cdecl *PFN_SDL_GL_SwapBuffers)(void);
typedef int(__cdecl *PFN_SDL_ShowCursor)(int);
typedef uint8_t(__cdecl *PFN_SDL_GetAppState)(void);

static PFN_SDL_SetVideoMode o_SDL_SetVideoMode;
static PFN_SDL_VideoModeOK o_SDL_VideoModeOK;
static PFN_SDL_ListModes o_SDL_ListModes;
static PFN_SDL_PollEvent o_SDL_PollEvent;
static PFN_SDL_GetMouseState o_SDL_GetMouseState;
static PFN_SDL_GL_SwapBuffers o_SDL_GL_SwapBuffers;
static PFN_SDL_ShowCursor p_SDL_ShowCursor;
static PFN_SDL_GetAppState p_SDL_GetAppState;

typedef void(APIENTRY *PFN_glBitmap)(GLsizei, GLsizei, GLfloat, GLfloat, GLfloat, GLfloat, const GLubyte *);
typedef void(APIENTRY *PFN_glDrawPixels)(GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void(APIENTRY *PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
typedef void(APIENTRY *PFN_glScissor)(GLint, GLint, GLsizei, GLsizei);
typedef void(APIENTRY *PFN_glLineWidth)(GLfloat);
typedef void(APIENTRY *PFN_glLineStipple)(GLint, GLushort);

static PFN_glBitmap o_glBitmap;
static PFN_glDrawPixels o_glDrawPixels;
static PFN_glReadPixels o_glReadPixels;
static PFN_glScissor o_glScissor;
static PFN_glLineWidth o_glLineWidth;
static PFN_glLineStipple o_glLineStipple;

// ---------------------------------------------------------------------------
// Layout state

static struct State {
    bool layoutReady = false;
    int refW = 0, refH = 0;   // output size the logical mode was derived from
    int logW = 0, logH = 0;   // resolution the game sees
    double baseScale = 1.0;

    bool active = false;      // GL/mouse remapping in effect
    int gameW = 0, gameH = 0; // resolution the game actually requested
    int outW = 0, outH = 0;   // real window client size
    int vx = 0, vy = 0, vw = 0, vh = 0; // GL viewport, bottom-up
    float sx = 1.0f, sy = 1.0f;
    int textFilter = FILTER_SMOOTH;
    int imageFilter = FILTER_LINEAR;

    int mouseX = -1, mouseY = -1; // last real mouse position (top-down)
} S;

static void PrimaryWorkArea(RECT *rc)
{
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, rc, 0))
        SetRect(rc, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
}

static void EnsureLayout()
{
    if (S.layoutReady) return;
    S.layoutReady = true;

    S.refW = cfg.width > 0 ? cfg.width : GetSystemMetrics(SM_CXSCREEN);
    S.refH = cfg.height > 0 ? cfg.height : GetSystemMetrics(SM_CYSCREEN);
    if (S.refW < 640 || S.refH < 480) { S.refW = 1024; S.refH = 768; }

    double s;
    if (cfg.scale > 0.0f) {
        s = cfg.scale;
    } else {
        s = (S.refW / (double)cfg.baseWidth < S.refH / (double)cfg.baseHeight)
                ? S.refW / (double)cfg.baseWidth
                : S.refH / (double)cfg.baseHeight;
        s *= cfg.multiplier;
        if (s < 1.0) s = 1.0;
    }
    if (cfg.integerScale) s = s < 1.0 ? 1.0 : floor(s + 1e-6);
    // Never lay the UI out smaller than 800x600
    double maxS = S.refW / 800.0 < S.refH / 600.0 ? S.refW / 800.0 : S.refH / 600.0;
    if (s > maxS) s = cfg.integerScale ? floor(maxS) : maxS;
    if (s < 0.5) s = 0.5;
    S.baseScale = s;

    S.logW = (int)floor(S.refW / s + 1e-4);
    S.logH = (int)floor(S.refH / s + 1e-4);
    Log("Layout: reference %dx%d, scale %.4f -> logical %dx%d", S.refW, S.refH, s, S.logW, S.logH);
}

static bool IsIntegral(float v) { return fabsf(v - floorf(v + 0.5f)) < 0.01f; }

static void SetupViewport(int gw, int gh, int ow, int oh)
{
    double s = ow / (double)gw < oh / (double)gh ? ow / (double)gw : oh / (double)gh;
    if (cfg.integerScale && s >= 1.0) s = floor(s + 1e-6);
    int vw = (int)floor(gw * s + 0.5), vh = (int)floor(gh * s + 0.5);
    if (!cfg.integerScale) {
        // Close enough: fill the window (sub-pixel non-uniformity is invisible)
        if (ow - vw < s) vw = ow;
        if (oh - vh < s) vh = oh;
    }
    if (vw > ow) vw = ow;
    if (vh > oh) vh = oh;

    S.gameW = gw; S.gameH = gh;
    S.outW = ow; S.outH = oh;
    S.vw = vw; S.vh = vh;
    S.vx = (ow - vw) / 2;
    S.vy = (oh - vh) / 2;
    S.sx = vw / (float)gw;
    S.sy = vh / (float)gh;

    bool integral = IsIntegral(S.sx) && IsIntegral(S.sy);
    S.textFilter = cfg.textFilter == FILTER_AUTO ? (integral ? FILTER_SHARP : FILTER_SMOOTH) : cfg.textFilter;
    S.imageFilter = cfg.imageFilter == FILTER_AUTO ? (integral ? FILTER_SHARP : FILTER_LINEAR) : cfg.imageFilter;
    if (S.imageFilter == FILTER_SMOOTH) S.imageFilter = FILTER_LINEAR;

    Log("Viewport: game %dx%d, window %dx%d, viewport %d,%d %dx%d, scale %.4f x %.4f, text filter %d, image filter %d",
        gw, gh, ow, oh, S.vx, S.vy, vw, vh, S.sx, S.sy, S.textFilter, S.imageFilter);
}

static inline float AvgScale() { return (S.sx + S.sy) * 0.5f; }
static inline bool Scaling() { return S.active && (S.sx != 1.0f || S.sy != 1.0f || S.vx || S.vy); }

// ---------------------------------------------------------------------------
// GL helpers

struct SavedMatrices {
    GLint mode;
    GLfloat proj[16], mv[16];
};

// Switch to window coordinates (origin bottom-left of the given viewport rect).
// Matrices are saved by value: the projection stack can be as shallow as 2.
static void BeginWindowSpace(SavedMatrices &m, float x, float y, float w, float h)
{
    glGetIntegerv(GL_MATRIX_MODE, &m.mode);
    glGetFloatv(GL_PROJECTION_MATRIX, m.proj);
    glGetFloatv(GL_MODELVIEW_MATRIX, m.mv);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(x, x + w, y, y + h, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void EndWindowSpace(const SavedMatrices &m)
{
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(m.proj);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(m.mv);
    glMatrixMode(m.mode);
}

static void DrawTexturedQuad(float x0, float y0, float x1, float y1)
{
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(x0, y0);
    glTexCoord2f(1, 0); glVertex2f(x1, y0);
    glTexCoord2f(1, 1); glVertex2f(x1, y1);
    glTexCoord2f(0, 1); glVertex2f(x0, y1);
    glEnd();
}

static void SetTexParams(bool nearest)
{
    GLint f = nearest ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// Upload with tightly packed client memory, leaving the game's unpack state alone
static void UploadPacked(GLint internalFormat, int w, int h, GLenum format, const void *data)
{
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
    glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, w, h, 0, format, GL_UNSIGNED_BYTE, data);
    glPopClientAttrib();
}

// ---------------------------------------------------------------------------
// Upscaling of 1-bit masks (glyphs, cursor)
//
// "smooth" reconstructs the mask bilinearly on a supersampled grid and turns
// the 0.5 iso-contour into an anti-aliased edge one output pixel wide. Straight
// edges stay where they were, staircases become clean diagonals.

static float MaskAt(const uint8_t *m, int w, int h, int x, int y)
{
    return (x < 0 || y < 0 || x >= w || y >= h) ? 0.0f : (float)m[y * w + x];
}

static float MaskBilinear(const uint8_t *m, int w, int h, float u, float v)
{
    u -= 0.5f;
    v -= 0.5f;
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    float fx = u - x0, fy = v - y0;
    float a = MaskAt(m, w, h, x0, y0), b = MaskAt(m, w, h, x0 + 1, y0);
    float c = MaskAt(m, w, h, x0, y0 + 1), d = MaskAt(m, w, h, x0 + 1, y0 + 1);
    return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}

struct ScaledMask {
    int tw, th;   // texture size
    int pad;      // padding around the source, in source pixels
    std::vector<uint8_t> alpha;
};

// mode: FILTER_SHARP / FILTER_LINEAR (texel per source pixel) or FILTER_SMOOTH
static void BuildScaledMask(const uint8_t *m, int w, int h, int mode, float scale, float threshold,
                            ScaledMask &out)
{
    out.pad = 1;
    if (mode != FILTER_SMOOTH) {
        out.tw = w + 2;
        out.th = h + 2;
        out.alpha.assign(out.tw * out.th, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                out.alpha[(y + 1) * out.tw + x + 1] = m[y * w + x] ? 255 : 0;
        return;
    }
    int n = (int)ceilf(scale * 2.0f);
    if (n < 2) n = 2;
    if (n > 16) n = 16;
    out.tw = (w + 2) * n;
    out.th = (h + 2) * n;
    out.alpha.resize(out.tw * out.th);
    float ramp = scale > 1.0f ? scale : 1.0f; // contour gradient ~1 per source px
    for (int ty = 0; ty < out.th; ++ty) {
        float v = (ty + 0.5f) / n - 1.0f;
        for (int tx = 0; tx < out.tw; ++tx) {
            float u = (tx + 0.5f) / n - 1.0f;
            float a = (MaskBilinear(m, w, h, u, v) - threshold) * ramp + 0.5f;
            a = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
            out.alpha[ty * out.tw + tx] = (uint8_t)(a * 255.0f + 0.5f);
        }
    }
}

// ---------------------------------------------------------------------------
// Text: glBitmap glyphs become cached, scaled alpha textures

struct GlyphTex { GLuint tex; int tw, th; int pad; };
static std::unordered_map<std::string, GlyphTex> g_glyphs;
static GLuint g_imageTex;
static GLuint g_cursorTex;
static int g_cursorTexW, g_cursorTexH, g_cursorTexPad;

static void ResetGLCaches()
{
    // Called on a fresh GL context: the old names died with the old context
    g_glyphs.clear();
    g_imageTex = 0;
    g_cursorTex = 0;
}

static const GlyphTex *GetGlyph(GLsizei w, GLsizei h, const GLubyte *bits)
{
    GLint lsb, rowLen, skipRows, skipPix, align;
    glGetIntegerv(GL_UNPACK_LSB_FIRST, &lsb);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLen);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPix);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    if (align < 1) align = 1;

    int rowPixels = rowLen > 0 ? rowLen : w;
    int stride = ((rowPixels + 7) / 8 + align - 1) / align * align;

    std::string key;
    key.resize(8 + (size_t)w * h);
    memcpy(&key[0], &w, 4);
    memcpy(&key[4], &h, 4);
    uint8_t *mask = (uint8_t *)&key[8];
    for (int y = 0; y < h; ++y) {
        const GLubyte *row = bits + (size_t)(y + skipRows) * stride;
        for (int x = 0; x < w; ++x) {
            int bx = x + skipPix;
            int bit = lsb ? (bx & 7) : 7 - (bx & 7);
            mask[y * w + x] = (row[bx >> 3] >> bit) & 1;
        }
    }

    auto it = g_glyphs.find(key);
    if (it != g_glyphs.end()) return &it->second;

    ScaledMask sm;
    BuildScaledMask(mask, w, h, S.textFilter, AvgScale(), 0.5f - cfg.textBoldness, sm);

    GlyphTex g;
    g.tw = sm.tw;
    g.th = sm.th;
    g.pad = sm.pad;
    GLint prevTex;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGenTextures(1, &g.tex);
    glBindTexture(GL_TEXTURE_2D, g.tex);
    SetTexParams(S.textFilter == FILTER_SHARP);
    UploadPacked(GL_ALPHA8, sm.tw, sm.th, GL_ALPHA, sm.alpha.data());
    glBindTexture(GL_TEXTURE_2D, prevTex);

    return &g_glyphs.emplace(std::move(key), g).first->second;
}

static void APIENTRY h_glBitmap(GLsizei w, GLsizei h, GLfloat xorig, GLfloat yorig, GLfloat xmove,
                                GLfloat ymove, const GLubyte *bits)
{
    if (!Scaling()) {
        o_glBitmap(w, h, xorig, yorig, xmove, ymove, bits);
        return;
    }

    if (bits && w > 0 && h > 0) {
        GLboolean valid = GL_FALSE;
        glGetBooleanv(GL_CURRENT_RASTER_POSITION_VALID, &valid);
        if (!valid) return; // glBitmap is a no-op (no move either)

        GLfloat rp[4], color[4];
        glGetFloatv(GL_CURRENT_RASTER_POSITION, rp);
        glGetFloatv(GL_CURRENT_RASTER_COLOR, color);

        const GlyphTex *g = GetGlyph(w, h, bits);

        // Same pixel placement as unscaled glBitmap, in logical pixels
        float lx = floorf((rp[0] - S.vx) / S.sx - xorig + 1e-3f);
        float ly = floorf((rp[1] - S.vy) / S.sy - yorig + 1e-3f);
        float x0 = (lx - g->pad) * S.sx, y0 = (ly - g->pad) * S.sy;
        float x1 = (lx + w + g->pad) * S.sx, y1 = (ly + h + g->pad) * S.sy;
        if (S.textFilter == FILTER_SHARP) {
            x0 = floorf(x0 + 0.5f); y0 = floorf(y0 + 0.5f);
            x1 = floorf(x1 + 0.5f); y1 = floorf(y1 + 0.5f);
        }

        glPushAttrib(GL_ENABLE_BIT | GL_TEXTURE_BIT | GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT);
        SavedMatrices m;
        BeginWindowSpace(m, 0, 0, (float)S.vw, (float)S.vh);
        glDisable(GL_LIGHTING);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g->tex);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4fv(color);
        DrawTexturedQuad(x0, y0, x1, y1);
        EndWindowSpace(m);
        glPopAttrib(); // restores raster position too
    }

    // Advance the raster position by the scaled amount
    o_glBitmap(0, 0, 0, 0, xmove * S.sx, ymove * S.sy, NULL);
}

// ---------------------------------------------------------------------------
// Images: glDrawPixels becomes a scaled textured quad

static void APIENTRY h_glDrawPixels(GLsizei w, GLsizei h, GLenum format, GLenum type, const void *pixels)
{
    if (!Scaling() || w <= 0 || h <= 0) {
        o_glDrawPixels(w, h, format, type, pixels);
        return;
    }

    bool texturable = type == GL_UNSIGNED_BYTE &&
                      (format == GL_RGBA || format == GL_RGB || format == GL_BGRA_EXT ||
                       format == GL_BGR_EXT || format == GL_LUMINANCE || format == GL_LUMINANCE_ALPHA ||
                       format == GL_ALPHA);
    if (!texturable) {
        glPixelZoom(S.sx, S.sy);
        o_glDrawPixels(w, h, format, type, pixels);
        glPixelZoom(1.0f, 1.0f);
        return;
    }

    GLboolean valid = GL_FALSE;
    glGetBooleanv(GL_CURRENT_RASTER_POSITION_VALID, &valid);
    if (!valid) return;
    GLfloat rp[4];
    glGetFloatv(GL_CURRENT_RASTER_POSITION, rp);

    // glDrawPixels covers the pixel centres inside [rp, rp+size): round in logical px
    float lx = floorf((rp[0] - S.vx) / S.sx + 0.5f);
    float ly = floorf((rp[1] - S.vy) / S.sy + 0.5f);
    float x0 = lx * S.sx, y0 = ly * S.sy;
    float x1 = (lx + w) * S.sx, y1 = (ly + h) * S.sy;
    bool sharp = S.imageFilter == FILTER_SHARP;
    if (sharp) {
        x0 = floorf(x0 + 0.5f); y0 = floorf(y0 + 0.5f);
        x1 = floorf(x1 + 0.5f); y1 = floorf(y1 + 0.5f);
    }

    glPushAttrib(GL_ENABLE_BIT | GL_TEXTURE_BIT | GL_CURRENT_BIT);
    if (!g_imageTex) glGenTextures(1, &g_imageTex);
    glBindTexture(GL_TEXTURE_2D, g_imageTex);
    SetTexParams(sharp);
    GLint internal = (format == GL_RGB || format == GL_BGR_EXT || format == GL_LUMINANCE) ? GL_RGB
                   : format == GL_ALPHA ? GL_ALPHA : GL_RGBA;
    // Same unpack state as glDrawPixels would have used
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, pixels);

    SavedMatrices m;
    BeginWindowSpace(m, 0, 0, (float)S.vw, (float)S.vh);
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1, 1, 1, 1);
    DrawTexturedQuad(x0, y0, x1, y1);
    EndWindowSpace(m);
    glPopAttrib();
}

// ---------------------------------------------------------------------------
// Framebuffer reads: logical rect -> real rect, then point-sample back down

static int BytesPerPixel(GLenum format)
{
    switch (format) {
    case GL_RGBA: case GL_BGRA_EXT: return 4;
    case GL_RGB: case GL_BGR_EXT: return 3;
    case GL_LUMINANCE_ALPHA: return 2;
    case GL_LUMINANCE: case GL_ALPHA: case GL_RED: case GL_GREEN: case GL_BLUE: return 1;
    default: return 0;
    }
}

static void APIENTRY h_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *data)
{
    int bpp = type == GL_UNSIGNED_BYTE ? BytesPerPixel(format) : 0;
    // Full-window reads (screenshots use the real surface size) pass through
    bool fullWindow = x == 0 && y == 0 && w == S.outW && h == S.outH;
    if (!Scaling() || fullWindow || bpp == 0 || w <= 0 || h <= 0) {
        o_glReadPixels(x, y, w, h, format, type, data);
        return;
    }

    int rx0 = S.vx + (int)floorf(x * S.sx + 0.5f), ry0 = S.vy + (int)floorf(y * S.sy + 0.5f);
    int rx1 = S.vx + (int)floorf((x + w) * S.sx + 0.5f), ry1 = S.vy + (int)floorf((y + h) * S.sy + 0.5f);
    int rw = rx1 - rx0 > 1 ? rx1 - rx0 : 1, rh = ry1 - ry0 > 1 ? ry1 - ry0 : 1;

    std::vector<uint8_t> tmp((size_t)rw * rh * bpp);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    o_glReadPixels(rx0, ry0, rw, rh, format, type, tmp.data());
    glPopClientAttrib();

    GLint align, rowLen, skipRows, skipPix;
    glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLen);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPix);
    if (align < 1) align = 1;
    int rowPixels = rowLen > 0 ? rowLen : w;
    int stride = (rowPixels * bpp + align - 1) / align * align;

    uint8_t *dst = (uint8_t *)data;
    for (int j = 0; j < h; ++j) {
        int sj = (int)((j + 0.5f) * rh / h);
        if (sj >= rh) sj = rh - 1;
        uint8_t *drow = dst + (size_t)(j + skipRows) * stride + (size_t)skipPix * bpp;
        for (int i = 0; i < w; ++i) {
            int si = (int)((i + 0.5f) * rw / w);
            if (si >= rw) si = rw - 1;
            memcpy(drow + (size_t)i * bpp, &tmp[((size_t)sj * rw + si) * bpp], bpp);
        }
    }
}

// ---------------------------------------------------------------------------
// Pixel-sized state

static void APIENTRY h_glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (!Scaling()) {
        o_glScissor(x, y, w, h);
        return;
    }
    int x0 = S.vx + (int)floorf(x * S.sx + 0.5f), y0 = S.vy + (int)floorf(y * S.sy + 0.5f);
    int x1 = S.vx + (int)floorf((x + w) * S.sx + 0.5f), y1 = S.vy + (int)floorf((y + h) * S.sy + 0.5f);
    o_glScissor(x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
}

static void APIENTRY h_glLineWidth(GLfloat width)
{
    o_glLineWidth(Scaling() ? width * AvgScale() * cfg.lineScale : width);
}

static void APIENTRY h_glLineStipple(GLint factor, GLushort pattern)
{
    if (Scaling()) {
        factor = (GLint)floorf(factor * AvgScale() + 0.5f);
        if (factor < 1) factor = 1;
        if (factor > 256) factor = 256;
    }
    o_glLineStipple(factor, pattern);
}

// ---------------------------------------------------------------------------
// Software cursor (the SDL arrow would stay tiny)

static const char *kArrow[] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.........X ",
    "X......XXXXX",
    "X...X..X    ",
    "X..XX..X    ",
    "X.X  X..X   ",
    "XX   X..X   ",
    "X     X..X  ",
    "      X..X  ",
    "       XX   ",
};
static const int kArrowW = 12, kArrowH = 19;

static void BuildCursorTexture(float scale)
{
    // Masks are stored bottom-up to match GL texture orientation
    uint8_t all[kArrowW * kArrowH], fill[kArrowW * kArrowH];
    for (int y = 0; y < kArrowH; ++y)
        for (int x = 0; x < kArrowW; ++x) {
            char c = kArrow[kArrowH - 1 - y][x];
            all[y * kArrowW + x] = c != ' ';
            fill[y * kArrowW + x] = c == '.';
        }
    bool sharp = IsIntegral(scale);
    int mode = sharp ? FILTER_SHARP : FILTER_SMOOTH;
    ScaledMask ma, mf;
    BuildScaledMask(all, kArrowW, kArrowH, mode, scale, 0.5f, ma);
    BuildScaledMask(fill, kArrowW, kArrowH, mode, scale, 0.5f, mf);

    std::vector<uint8_t> rgba((size_t)ma.tw * ma.th * 4);
    for (size_t i = 0; i < (size_t)ma.tw * ma.th; ++i) {
        uint8_t a = ma.alpha[i], f = mf.alpha[i];
        uint8_t c = a ? (uint8_t)(f * 255 / a > 255 ? 255 : f * 255 / a) : 0;
        rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = c;
        rgba[i * 4 + 3] = a;
    }

    glGenTextures(1, &g_cursorTex);
    glBindTexture(GL_TEXTURE_2D, g_cursorTex);
    SetTexParams(sharp);
    UploadPacked(GL_RGBA, ma.tw, ma.th, GL_RGBA, rgba.data());
    g_cursorTexW = ma.tw;
    g_cursorTexH = ma.th;
    g_cursorTexPad = ma.pad;
}

static void DrawCursor()
{
    if (S.mouseX < 0 || S.mouseY < 0) return;
    if (p_SDL_GetAppState && !(p_SDL_GetAppState() & SDL_APPMOUSEFOCUS)) return;

    float scale = AvgScale() * cfg.cursorSize;
    if (cfg.integerScale) scale = floorf(scale + 0.5f);
    if (scale < 1.0f) scale = 1.0f;

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    if (!g_cursorTex) BuildCursorTexture(scale);
    glViewport(0, 0, S.outW, S.outH);
    SavedMatrices m;
    BeginWindowSpace(m, 0, 0, (float)S.outW, (float)S.outH);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_cursorTex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    float hx = (float)S.mouseX, hy = (float)(S.outH - S.mouseY); // hotspot = arrow tip
    float x0 = hx - g_cursorTexPad * scale;
    float y1 = hy + g_cursorTexPad * scale;
    float x1 = x0 + (kArrowW + 2 * g_cursorTexPad) * scale;
    float y0 = y1 - (kArrowH + 2 * g_cursorTexPad) * scale;
    DrawTexturedQuad(floorf(x0), floorf(y0), floorf(x1), floorf(y1));
    EndWindowSpace(m);
    glPopAttrib();
}

static void __cdecl h_SDL_GL_SwapBuffers(void)
{
    if (S.active && cfg.cursor) DrawCursor();
    o_SDL_GL_SwapBuffers();
}

// ---------------------------------------------------------------------------
// Mouse

static void ToLogical(int rx, int ry, int *lx, int *ly)
{
    int top = S.outH - S.vy - S.vh;
    int x = (int)floorf((rx - S.vx) / S.sx);
    int y = (int)floorf((ry - top) / S.sy);
    *lx = x < 0 ? 0 : x >= S.gameW ? S.gameW - 1 : x;
    *ly = y < 0 ? 0 : y >= S.gameH ? S.gameH - 1 : y;
}

static int __cdecl h_SDL_PollEvent(void *ev)
{
    int r = o_SDL_PollEvent(ev);
    if (r && ev && S.active) {
        uint8_t type = *(uint8_t *)ev;
        int lx, ly;
        if (type == SDL_MOUSEMOTION) {
            SDL_MouseMotionEvent *e = (SDL_MouseMotionEvent *)ev;
            S.mouseX = e->x;
            S.mouseY = e->y;
            ToLogical(e->x, e->y, &lx, &ly);
            e->x = (uint16_t)lx;
            e->y = (uint16_t)ly;
            e->xrel = (int16_t)floorf(e->xrel / S.sx + 0.5f);
            e->yrel = (int16_t)floorf(e->yrel / S.sy + 0.5f);
        } else if (type == SDL_MOUSEBUTTONDOWN || type == SDL_MOUSEBUTTONUP) {
            SDL_MouseButtonEvent *e = (SDL_MouseButtonEvent *)ev;
            S.mouseX = e->x;
            S.mouseY = e->y;
            ToLogical(e->x, e->y, &lx, &ly);
            e->x = (uint16_t)lx;
            e->y = (uint16_t)ly;
        }
    }
    return r;
}

static uint8_t __cdecl h_SDL_GetMouseState(int *x, int *y)
{
    int rx = 0, ry = 0;
    uint8_t buttons = o_SDL_GetMouseState(&rx, &ry);
    if (S.active) {
        S.mouseX = rx;
        S.mouseY = ry;
        ToLogical(rx, ry, &rx, &ry);
    }
    if (x) *x = rx;
    if (y) *y = ry;
    return buttons;
}

// ---------------------------------------------------------------------------
// Video mode

static SDL_Rect g_mode;
static SDL_Rect *g_modeList[2];

static SDL_Rect **__cdecl h_SDL_ListModes(void *format, uint32_t flags)
{
    EnsureLayout();
    g_mode.x = g_mode.y = 0;
    g_mode.w = (uint16_t)S.logW;
    g_mode.h = (uint16_t)S.logH;
    g_modeList[0] = &g_mode;
    g_modeList[1] = NULL;
    return g_modeList;
}

static int __cdecl h_SDL_VideoModeOK(int w, int h, int bpp, uint32_t flags)
{
    EnsureLayout();
    if (flags & SDL_OPENGL) return o_SDL_VideoModeOK(S.refW, S.refH, bpp, flags);
    return o_SDL_VideoModeOK(w, h, bpp, flags);
}

static void WindowedSize(int gw, int gh, int *ow, int *oh)
{
    if (cfg.width > 0 && cfg.height > 0) {
        *ow = cfg.width;
        *oh = cfg.height;
        return;
    }
    RECT wa, frame = {0, 0, 0, 0};
    PrimaryWorkArea(&wa);
    AdjustWindowRect(&frame, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    int aw = (wa.right - wa.left) - (frame.right - frame.left);
    int ah = (wa.bottom - wa.top) - (frame.bottom - frame.top);
    // Largest window with the game's aspect ratio that fits the work area
    double k = aw / (double)gw < ah / (double)gh ? aw / (double)gw : ah / (double)gh;
    if (cfg.integerScale) k = k < 1.0 ? 1.0 : floor(k + 1e-6);
    *ow = (int)floor(gw * k + 0.5);
    *oh = (int)floor(gh * k + 0.5);
}

static void PlaceWindow(bool borderless)
{
    HWND hwnd = WindowFromDC(wglGetCurrentDC());
    if (!hwnd) return;
    if (borderless) {
        SetWindowPos(hwnd, HWND_TOP, 0, 0, S.outW, S.outH, SWP_NOSIZE | SWP_SHOWWINDOW);
        return;
    }
    RECT wa, rc;
    PrimaryWorkArea(&wa);
    GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = wa.left + ((wa.right - wa.left) - w) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - h) / 2;
    SetWindowPos(hwnd, NULL, x < wa.left ? wa.left : x, y < wa.top ? wa.top : y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);
}

static void *__cdecl h_SDL_SetVideoMode(int w, int h, int bpp, uint32_t flags)
{
    if (!(flags & SDL_OPENGL) || w <= 0 || h <= 0) {
        S.active = false;
        return o_SDL_SetVideoMode(w, h, bpp, flags);
    }
    EnsureLayout();

    int mode = cfg.mode;
    if (mode == MODE_AUTO) mode = (flags & SDL_FULLSCREEN) ? MODE_FULLSCREEN : MODE_WINDOWED;

    uint32_t f = flags & ~(SDL_FULLSCREEN | SDL_NOFRAME | SDL_RESIZABLE);
    int ow, oh;
    if (mode == MODE_FULLSCREEN) {
        ow = S.refW;
        oh = S.refH;
        f |= SDL_FULLSCREEN;
    } else if (mode == MODE_BORDERLESS) {
        ow = GetSystemMetrics(SM_CXSCREEN);
        oh = GetSystemMetrics(SM_CYSCREEN);
        f |= SDL_NOFRAME;
    } else {
        WindowedSize(w, h, &ow, &oh);
    }

    Log("SDL_SetVideoMode: game %dx%d flags 0x%08X -> %dx%d flags 0x%08X", w, h, flags, ow, oh, f);
    S.active = false;
    void *surface = o_SDL_SetVideoMode(ow, oh, bpp, f);
    if (!surface) {
        Log("  failed, falling back to the game's own request");
        return o_SDL_SetVideoMode(w, h, bpp, flags);
    }

    ResetGLCaches();
    SetupViewport(w, h, ow, oh);
    S.active = true;

    glViewport(S.vx, S.vy, S.vw, S.vh);
    o_glLineWidth(AvgScale() * cfg.lineScale);
    glPointSize(AvgScale() > 1.0f ? AvgScale() : 1.0f);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    if (mode != MODE_FULLSCREEN) PlaceWindow(mode == MODE_BORDERLESS);
    if (cfg.cursor && p_SDL_ShowCursor) p_SDL_ShowCursor(0);
    return surface;
}

// ---------------------------------------------------------------------------
// Import table patching

struct HookDef {
    const char *dll;
    const char *name;
    void *hook;
    void **orig;
};

static HookDef g_hooks[] = {
    {"sdl.dll", "SDL_SetVideoMode", (void *)h_SDL_SetVideoMode, (void **)&o_SDL_SetVideoMode},
    {"sdl.dll", "SDL_VideoModeOK", (void *)h_SDL_VideoModeOK, (void **)&o_SDL_VideoModeOK},
    {"sdl.dll", "SDL_ListModes", (void *)h_SDL_ListModes, (void **)&o_SDL_ListModes},
    {"sdl.dll", "SDL_PollEvent", (void *)h_SDL_PollEvent, (void **)&o_SDL_PollEvent},
    {"sdl.dll", "SDL_GetMouseState", (void *)h_SDL_GetMouseState, (void **)&o_SDL_GetMouseState},
    {"sdl.dll", "SDL_GL_SwapBuffers", (void *)h_SDL_GL_SwapBuffers, (void **)&o_SDL_GL_SwapBuffers},
    {"opengl32.dll", "glBitmap", (void *)h_glBitmap, (void **)&o_glBitmap},
    {"opengl32.dll", "glDrawPixels", (void *)h_glDrawPixels, (void **)&o_glDrawPixels},
    {"opengl32.dll", "glReadPixels", (void *)h_glReadPixels, (void **)&o_glReadPixels},
    {"opengl32.dll", "glScissor", (void *)h_glScissor, (void **)&o_glScissor},
    {"opengl32.dll", "glLineWidth", (void *)h_glLineWidth, (void **)&o_glLineWidth},
    {"opengl32.dll", "glLineStipple", (void *)h_glLineStipple, (void **)&o_glLineStipple},
};

static void WriteIAT(void **slot, void *value)
{
    DWORD old;
    if (VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
        *slot = value;
        VirtualProtect(slot, sizeof(void *), old, &old);
    }
}

static int PatchImports(HMODULE module)
{
    uint8_t *base = (uint8_t *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;

    int patched = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; ++d) {
        const char *dllName = (const char *)(base + d->Name);
        bool any = false;
        for (HookDef &h : g_hooks)
            if (!_stricmp(dllName, h.dll)) any = true;
        if (!any) continue;

        HMODULE dll = GetModuleHandleA(dllName);
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        IMAGE_THUNK_DATA *names = d->OriginalFirstThunk ? (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk) : NULL;

        for (int i = 0; iat[i].u1.Function; ++i) {
            void **slot = (void **)&iat[i].u1.Function;
            for (HookDef &h : g_hooks) {
                if (*h.orig || _stricmp(dllName, h.dll)) continue;
                bool match = false;
                if (names && !IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) {
                    IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names[i].u1.AddressOfData);
                    match = !strcmp((const char *)ibn->Name, h.name);
                } else if (dll) {
                    match = *slot == (void *)GetProcAddress(dll, h.name);
                }
                if (!match) continue;
                *h.orig = *slot;
                WriteIAT(slot, h.hook);
                ++patched;
                Log("Hooked %s!%s", dllName, h.name);
            }
        }
    }
    return patched;
}

static bool g_installed;

static void Install()
{
    if (g_installed) return;
    g_installed = true;

    HMODULE exe = GetModuleHandleA(NULL);
    int n = PatchImports(exe);

    // Anything the game doesn't import still needs a working pointer
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    HMODULE sdl = GetModuleHandleA("SDL.dll");
    if (!o_glBitmap) o_glBitmap = glBitmap;
    if (!o_glDrawPixels) o_glDrawPixels = glDrawPixels;
    if (!o_glReadPixels) o_glReadPixels = glReadPixels;
    if (!o_glScissor) o_glScissor = glScissor;
    if (!o_glLineWidth) o_glLineWidth = glLineWidth;
    if (!o_glLineStipple) o_glLineStipple = glLineStipple;
    if (sdl) {
        p_SDL_ShowCursor = (PFN_SDL_ShowCursor)GetProcAddress(sdl, "SDL_ShowCursor");
        p_SDL_GetAppState = (PFN_SDL_GetAppState)GetProcAddress(sdl, "SDL_GetAppState");
    }

    // Remapping only ever switches on inside the SDL_SetVideoMode hook, so a
    // build without these imports simply runs unscaled.
    bool sdlOk = o_SDL_SetVideoMode && o_SDL_ListModes && o_SDL_PollEvent;
    Log("Installed %d hooks (opengl32 %s, SDL %s)%s", n, gl ? "loaded" : "missing", sdl ? "loaded" : "missing",
        sdlOk ? "" : " - WARNING: core SDL imports not found, scaling disabled");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);

    GetModuleFileNameA(inst, g_dir, MAX_PATH);
    char *slash = strrchr(g_dir, '\\');
    if (slash) *slash = 0;

    LoadRealVersionDll();
    LoadConfig();
    if (cfg.log) {
        char path[MAX_PATH];
        _snprintf(path, MAX_PATH, "%s\\UplinkUIScale.log", g_dir);
        path[MAX_PATH - 1] = 0;
        g_log = fopen(path, "w");
        Log("UplinkUIScale 1.0");
    }
    if (!cfg.enabled) {
        Log("Disabled in ini");
        return TRUE;
    }
    if (cfg.dpiAware) SetProcessDPIAware();
    Install();
    return TRUE;
}
