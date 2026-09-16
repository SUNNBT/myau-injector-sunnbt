// Myau Injector -- new injection UI, by sunnbt
//
// A fresh, self-contained Win32 + GDI+ window on top of the existing loader
// core (target detection / DLL injection / payload download). Functional flow
// is unchanged: find the running Minecraft java process, push the native
// payload into it, stream progress and log lines back to the UI.

#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <objidl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "download.h"
#include "inject.h"
#include "payload_url.h"
#include "resource.h"

namespace {

constexpr UINT WM_LOG_LINE    = WM_APP + 1;
constexpr UINT WM_INJECT_DONE = WM_APP + 2;
constexpr UINT WM_PROGRESS    = WM_APP + 3;
constexpr UINT_PTR TIMER_TARGET = 2;
constexpr UINT TARGET_POLL_MS   = 1000;

constexpr int WINDOW_WIDTH  = 760;
constexpr int WINDOW_HEIGHT = 500;
constexpr int HEADER_HEIGHT = 72;
constexpr int CLOSE_WIDTH    = 46;
constexpr int CLOSE_HEIGHT   = 36;
constexpr int PAD = 24;
constexpr int LEFT_X   = PAD;
constexpr int LEFT_W   = 300;
constexpr int RIGHT_X  = LEFT_X + LEFT_W + PAD;
constexpr int RIGHT_W  = WINDOW_WIDTH - RIGHT_X - PAD;
constexpr int CARD_TOP = HEADER_HEIGHT + 16;
constexpr int CARD_BOTTOM = WINDOW_HEIGHT - 56;
constexpr int STATUS_TOP   = CARD_TOP + 18;
constexpr int BUTTON_TOP   = CARD_BOTTOM - 64;
constexpr int BUTTON_H     = 50;
constexpr int PROGRESS_TOP = BUTTON_TOP + BUTTON_H + 14;
constexpr int PROGRESS_H   = 4;
constexpr int FOOTER_TOP   = WINDOW_HEIGHT - 34;
constexpr int CONSOLE_STRIP = 30;
constexpr int CONSOLE_PAD   = 14;
constexpr int CONSOLE_LINE  = 17;
constexpr size_t CONSOLE_MAX_LINES = 400;

constexpr int SIZE_TITLE    = 24;
constexpr int SIZE_SUB      = 12;
constexpr int SIZE_STATUS   = 34;
constexpr int SIZE_BODY     = 14;
constexpr int SIZE_BUTTON   = 17;
constexpr int SIZE_SMALL    = 12;
constexpr int SIZE_CONSOLE  = 13;

const COLORREF COLOUR_BG       = RGB(14, 14, 19);
const COLORREF COLOUR_CARD     = RGB(22, 22, 30);
const COLORREF COLOUR_CONSOLE  = RGB(9, 9, 13);
const COLORREF COLOUR_BORDER   = RGB(38, 38, 50);
const COLORREF COLOUR_TEXT     = RGB(238, 240, 248);
const COLORREF COLOUR_BODY     = RGB(178, 182, 198);
const COLORREF COLOUR_MUTED    = RGB(132, 136, 154);
const COLORREF COLOUR_DIM      = RGB(96, 99, 116);
const COLORREF COLOUR_ACCENT       = RGB(124, 92, 255);
const COLORREF COLOUR_ACCENT_HOT   = RGB(146, 118, 255);
const COLORREF COLOUR_ACCENT_DOWN  = RGB(100, 72, 214);
const COLORREF COLOUR_ACCENT_OFF   = RGB(40, 34, 66);
const COLORREF COLOUR_ACCENT_INK_OFF = RGB(120, 114, 158);
const COLORREF COLOUR_GOOD     = RGB(80, 200, 120);
const COLORREF COLOUR_BAD      = RGB(226, 86, 92);
const COLORREF COLOUR_WHITE    = RGB(255, 255, 255);

Gdiplus::Color gp(COLORREF c) {
    return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
}

enum class Stage { IDLE, WORKING, DONE_OK, DONE_FAILED };
enum class Ink  { NORMAL, GOOD, BAD, DEBUG };
struct Line { std::wstring text; Ink ink = Ink::NORMAL; };

HWND g_window = nullptr;
Gdiplus::Bitmap *g_logo = nullptr;
Gdiplus::Rect g_logoContent;
ULONG_PTR g_gdiplusToken = 0;
HDC g_memory = nullptr;
HBITMAP g_memoryBitmap = nullptr;
HGDIOBJ g_memoryOld = nullptr;
HBRUSH g_bgBrush = nullptr;
HANDLE g_fontResource = nullptr;
std::wstring g_family = L"Segoe UI";
HFONT g_fontTitle = nullptr, g_fontSub = nullptr, g_fontStatus = nullptr;
HFONT g_fontBody = nullptr, g_fontButton = nullptr, g_fontSmall = nullptr, g_fontConsole = nullptr;
Stage g_stage = Stage::IDLE;
bool g_buttonEnabled = false, g_armed = false, g_buttonHot = false, g_buttonDown = false, g_closeHot = false;
double g_progress = -1.0;
DWORD g_targetPid = 0;
std::wstring g_targetLauncher;
std::vector<Line> g_console;

void enterStage(Stage stage);
DWORD WINAPI injectThread(LPVOID);

std::wstring formatStr(const wchar_t *pattern, ...) {
    wchar_t buffer[512];
    va_list args; va_start(args, pattern);
    _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, pattern, args);
    va_end(args);
    return buffer;
}
struct Payload { const void *bytes = nullptr; DWORD size = 0; };
Payload payload(int id) {
    Payload out;
    HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!found) return out;
    HGLOBAL loaded = LoadResource(nullptr, found);
    if (!loaded) return out;
    out.bytes = LockResource(loaded);
    out.size  = SizeofResource(nullptr, found);
    return out;
}
std::wstring sidecarOr(const wchar_t *fileName, const wchar_t *fallback) {
    wchar_t exePath[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        std::wstring path(exePath, length);
        size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            path = path.substr(0, slash + 1) + fileName;
            HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                char raw[8192]; DWORD read = 0;
                BOOL ok = ReadFile(h, raw, sizeof(raw)-1, &read, nullptr);
                CloseHandle(h);
                if (ok && read > 0) {
                    raw[read] = 0;
                    int wide = MultiByteToWideChar(CP_UTF8, 0, raw, (int)read, nullptr, 0);
                    std::wstring t(wide, 0);
                    MultiByteToWideChar(CP_UTF8, 0, raw, (int)read, &t[0], wide);
                    size_t a = t.find_first_not_of(L" \t\r\n"), b = t.find_last_not_of(L" \t\r\n");
                    if (a != std::wstring::npos) return t.substr(a, b-a+1);
                }
            }
        }
    }
    return fallback;
}
std::wstring payloadUrl() { return sidecarOr(L"myau_url.txt", MYAU_PAYLOAD_URL); }
std::wstring cacheDllPath() {
    wchar_t t[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, t)) return L"";
    return std::wstring(t) + L"myau_native_cache.bin";
}
std::wstring cacheMetaPath() {
    std::wstring d = cacheDllPath();
    return d.empty() ? L"" : d + L".meta";
}
bool readFileBytes(const std::wstring &p, std::vector<BYTE> &out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr);
    CloseHandle(h);
    if (!ok || read != out.size()) { out.clear(); return false; }
    return true;
}
bool writeFileBytes(const std::wstring &p, const std::vector<BYTE> &d) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, d.data(), (DWORD)d.size(), &written, nullptr);
    CloseHandle(h);
    return ok && written == d.size();
}
bool readCacheMeta(std::wstring &etag, std::wstring &lm) {
    std::wstring p = cacheMetaPath();
    if (p.empty()) return false;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char raw[1024] = {0}; DWORD read = 0;
    BOOL ok = ReadFile(h, raw, sizeof(raw)-1, &read, nullptr);
    CloseHandle(h);
    if (!ok || read == 0) return false;
    raw[read] = 0;
    int wide = MultiByteToWideChar(CP_UTF8, 0, raw, (int)read, nullptr, 0);
    std::wstring t(wide, 0);
    MultiByteToWideChar(CP_UTF8, 0, raw, (int)read, &t[0], wide);
    size_t sp = t.find(L'\n');
    if (sp == std::wstring::npos) return false;
    etag = t.substr(0, sp);
    lm = t.substr(sp+1);
    auto trim = [](std::wstring &s) { size_t b = s.find_last_not_of(L" \t\r\n"); s = b==std::wstring::npos?L"":s.substr(0,b+1); };
    trim(etag); trim(lm);
    return true;
}
void writeCacheMeta(const std::wstring &etag, const std::wstring &lm) {
    std::wstring p = cacheMetaPath();
    if (p.empty()) return;
    std::wstring t = etag + L"\n" + lm;
    int n = WideCharToMultiByte(CP_UTF8, 0, t.c_str(), (int)t.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, t.c_str(), (int)t.size(), &u[0], n, nullptr, nullptr);
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, u.data(), (DWORD)u.size(), &w, nullptr);
    CloseHandle(h);
}

void loadFont() {
    Payload ttf = payload(IDR_FONT);
    if (!ttf.bytes || ttf.size == 0) return;
    DWORD installed = 0;
    g_fontResource = AddFontMemResourceEx(const_cast<void*>(ttf.bytes), ttf.size, nullptr, &installed);
    if (!g_fontResource || installed == 0) return;
    Gdiplus::PrivateFontCollection col;
    if (col.AddMemoryFont(ttf.bytes, (INT)ttf.size) != Gdiplus::Ok) return;
    INT n = col.GetFamilyCount();
    if (n <= 0) return;
    std::unique_ptr<Gdiplus::FontFamily[]> fam(new Gdiplus::FontFamily[n]);
    INT got = 0;
    if (col.GetFamilies(n, fam.get(), &got) != Gdiplus::Ok || got <= 0) return;
    wchar_t name[LF_FACESIZE] = {0};
    if (fam[0].GetFamilyName(name) == Gdiplus::Ok) g_family = name;
}
HFONT createFont(const wchar_t *f, int px) {
    return CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH|FF_DONTCARE, f);
}
void createFonts() {
    g_fontTitle  = createFont(g_family.c_str(), SIZE_TITLE);
    g_fontSub    = createFont(g_family.c_str(), SIZE_SUB);
    g_fontStatus = createFont(g_family.c_str(), SIZE_STATUS);
    g_fontBody   = createFont(g_family.c_str(), SIZE_BODY);
    g_fontButton = createFont(g_family.c_str(), SIZE_BUTTON);
    g_fontSmall  = createFont(g_family.c_str(), SIZE_SMALL);
    g_fontConsole= createFont(L"Consolas", SIZE_CONSOLE);
}
void destroyFonts() {
    for (HFONT *f : {&g_fontTitle,&g_fontSub,&g_fontStatus,&g_fontBody,&g_fontButton,&g_fontSmall,&g_fontConsole})
        if (*f) { DeleteObject(*f); *f = nullptr; }
}
SIZE measureText(HDC dc, HFONT f, const wchar_t *t) {
    HGDIOBJ p = SelectObject(dc, f);
    SIZE s = {0,0};
    GetTextExtentPoint32W(dc, t, (int)wcslen(t), &s);
    SelectObject(dc, p);
    return s;
}
void drawText(HDC dc, HFONT f, COLORREF c, int x, int y, const wchar_t *t) {
    HGDIOBJ p = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, c);
    TextOutW(dc, x, y, t, (int)wcslen(t));
    SelectObject(dc, p);
}
void drawTextIn(HDC dc, HFONT f, COLORREF c, RECT b, const wchar_t *t) {
    HGDIOBJ p = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, c);
    DrawTextW(dc, t, -1, &b, DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX|DT_END_ELLIPSIS);
    SelectObject(dc, p);
}
void fillRect(HDC dc, RECT b, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &b, br);
    DeleteObject(br);
}

Gdiplus::Bitmap *loadLogo() {
    Payload png = payload(IDR_LOGO);
    if (!png.bytes || png.size == 0) return nullptr;
    HGLOBAL buf = GlobalAlloc(GMEM_MOVEABLE, png.size);
    if (!buf) return nullptr;
    void *t = GlobalLock(buf);
    memcpy(t, png.bytes, png.size);
    GlobalUnlock(buf);
    IStream *st = nullptr;
    if (CreateStreamOnHGlobal(buf, TRUE, &st) != S_OK) { GlobalFree(buf); return nullptr; }
    Gdiplus::Bitmap *img = Gdiplus::Bitmap::FromStream(st);
    st->Release();
    if (img && img->GetLastStatus() != Gdiplus::Ok) { delete img; return nullptr; }
    return img;
}
Gdiplus::Rect trimToContent(Gdiplus::Bitmap *img) {
    Gdiplus::Rect whole(0,0,(INT)img->GetWidth(),(INT)img->GetHeight());
    Gdiplus::BitmapData d;
    if (img->LockBits(&whole, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &d) != Gdiplus::Ok) return whole;
    int l=whole.Width, t=whole.Height, r=-1, b=-1;
    for (int y=0;y<whole.Height;y++) {
        const BYTE *row = (const BYTE*)d.Scan0 + (INT_PTR)y*d.Stride;
        for (int x=0;x<whole.Width;x++) {
            if (row[x*4+3] <= 16) continue;
            if (x<l) l=x; if (x>r) r=x; if (y<t) t=y; if (y>b) b=y;
        }
    }
    img->UnlockBits(&d);
    if (r<l || b<t) return whole;
    return Gdiplus::Rect(l,t,r-l+1,b-t+1);
}
void addRoundedRect(Gdiplus::GraphicsPath &p, float x, float y, float w, float h, float r) {
    if (r*2 > h) r = h/2;
    if (r*2 > w) r = w/2;
    if (r <= 0) { p.AddRectangle(Gdiplus::RectF(x,y,w,h)); return; }
    float d = r*2;
    p.AddArc(x,y,d,d,180,90);
    p.AddArc(x+w-d,y,d,d,270,90);
    p.AddArc(x+w-d,y+h-d,d,d,0,90);
    p.AddArc(x,y+h-d,d,d,90,90);
    p.CloseFigure();
}

RECT closeRect()  { return {WINDOW_WIDTH-CLOSE_WIDTH, 0, WINDOW_WIDTH, CLOSE_HEIGHT}; }
RECT leftCardRect() { return {LEFT_X, CARD_TOP, LEFT_X+LEFT_W, CARD_BOTTOM}; }
RECT consoleRect()  { return {RIGHT_X, CARD_TOP, RIGHT_X+RIGHT_W, CARD_BOTTOM}; }
RECT buttonRect()   { return {LEFT_X, BUTTON_TOP, LEFT_X+LEFT_W, BUTTON_TOP+BUTTON_H}; }
RECT progressRect() { return {LEFT_X, PROGRESS_TOP, LEFT_X+LEFT_W, PROGRESS_TOP+PROGRESS_H}; }

const wchar_t *buttonLabel() {
    switch (g_stage) {
        case Stage::WORKING: return L"Injecting...";
        case Stage::DONE_OK: return L"Injected";
        default: return g_armed ? L"Waiting for game" : L"Inject";
    }
}

void paintShapes(HDC dc) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    RECT card = leftCardRect();
    { Gdiplus::GraphicsPath p;
      addRoundedRect(p, card.left+0.5f, card.top+0.5f, card.right-card.left-1.0f, card.bottom-card.top-1.0f, 12.0f);
      Gdiplus::SolidBrush back(gp(COLOUR_CARD)); g.FillPath(&back, &p);
      Gdiplus::Pen border(gp(COLOUR_BORDER), 1.0f); g.DrawPath(&border, &p);
    }
    RECT con = consoleRect();
    { Gdiplus::GraphicsPath p;
      addRoundedRect(p, con.left+0.5f, con.top+0.5f, con.right-con.left-1.0f, con.bottom-con.top-1.0f, 12.0f);
      Gdiplus::SolidBrush back(gp(COLOUR_CONSOLE)); g.FillPath(&back, &p);
      Gdiplus::Pen border(gp(COLOUR_BORDER), 1.0f); g.DrawPath(&border, &p);
      Gdiplus::SolidBrush strip(gp(COLOUR_BORDER));
      g.FillRectangle(&strip, con.left+1.0f, con.top+CONSOLE_STRIP, con.right-con.left-2.0f, 1.0f);
      Gdiplus::SolidBrush dot(gp(COLOUR_ACCENT));
      for (int i=0;i<3;i++) g.FillEllipse(&dot, con.left+16+i*14, con.top+CONSOLE_STRIP/2-4, 8.0f, 8.0f);
    }
    RECT close = closeRect();
    if (g_closeHot) {
        Gdiplus::GraphicsPath p;
        addRoundedRect(p, close.left, close.top, close.right-close.left, close.bottom-close.top, 8.0f);
        Gdiplus::SolidBrush hot(gp(COLOUR_BAD)); g.FillPath(&hot, &p);
    }
    { Gdiplus::Pen cross(gp(g_closeHot?COLOUR_WHITE:COLOUR_MUTED), 1.4f);
      float cx=(close.left+close.right)/2.0f, cy=(close.top+close.bottom)/2.0f, arm=5.0f;
      g.DrawLine(&cross, cx-arm, cy-arm, cx+arm, cy+arm);
      g.DrawLine(&cross, cx+arm, cy-arm, cx-arm, cy+arm);
    }
    RECT btn = buttonRect();
    { Gdiplus::GraphicsPath face;
      addRoundedRect(face, btn.left, btn.top, btn.right-btn.left, btn.bottom-btn.top, 11.0f);
      COLORREF fc = !g_buttonEnabled ? COLOUR_ACCENT_OFF
                  : (g_buttonDown ? COLOUR_ACCENT_DOWN : (g_buttonHot ? COLOUR_ACCENT_HOT : COLOUR_ACCENT));
      Gdiplus::SolidBrush ink(gp(fc)); g.FillPath(&ink, &face);
    }
    if (g_progress >= 0.0) {
      RECT bar = progressRect();
      float x=bar.left, y=bar.top, w=bar.right-bar.left, h=bar.bottom-bar.top;
      Gdiplus::GraphicsPath track;
      addRoundedRect(track, x, y, w, h, h/2);
      Gdiplus::SolidBrush ti(gp(COLOUR_ACCENT_OFF)); g.FillPath(&ti, &track);
      double fr = g_progress>1?1:g_progress;
      if (fr > 0) {
        float filled = (float)(w*fr); if (filled<h) filled=h;
        Gdiplus::GraphicsPath done;
        addRoundedRect(done, x, y, filled, h, h/2);
        Gdiplus::SolidBrush di(gp(g_stage==Stage::DONE_FAILED?COLOUR_BAD:COLOUR_ACCENT_HOT));
        g.FillPath(&di, &done);
      }
    }
    if (g_logo && g_logoContent.Width > 0) {
      float lh = 34.0f;
      float lw = (float)(g_logoContent.Width * (lh / g_logoContent.Height));
      g.DrawImage(g_logo, Gdiplus::RectF(LEFT_X, (HEADER_HEIGHT-lh)/2.0f, lw, lh),
                  g_logoContent.X, g_logoContent.Y, g_logoContent.Width, g_logoContent.Height, Gdiplus::UnitPixel);
    }
}

void paintText(HDC dc) {
    int titleLeft = LEFT_X;
    if (g_logo && g_logoContent.Width > 0) {
        float lh=34.0f;
        float lw=(float)(g_logoContent.Width*(lh/g_logoContent.Height));
        titleLeft = (int)(LEFT_X + lw + 14.0f + 0.5f);
    }
    drawText(dc, g_fontTitle, COLOUR_TEXT, titleLeft, 18, L"Myau Injector");
    drawText(dc, g_fontSub, COLOUR_ACCENT, titleLeft, 46, L"by sunnbt");

    drawText(dc, g_fontSmall, COLOUR_DIM, LEFT_X+20, STATUS_TOP, L"GAME STATUS");
    std::wstring big; COLORREF bc;
    if (g_stage==Stage::WORKING) { big=L"Injecting..."; bc=COLOUR_ACCENT_HOT; }
    else if (g_stage==Stage::DONE_OK) { big=L"Injected"; bc=COLOUR_GOOD; }
    else if (g_targetPid) { big=formatStr(L"Ready  ·  %s", g_targetLauncher.c_str()); bc=COLOUR_GOOD; }
    else { big=L"Waiting"; bc=COLOUR_MUTED; }
    RECT bb = {LEFT_X+18, STATUS_TOP+22, LEFT_X+LEFT_W-18, STATUS_TOP+22+44};
    drawTextIn(dc, g_fontStatus, bc, bb, big.c_str());

    std::wstring sub;
    if (g_stage==Stage::DONE_OK) sub=L"Client loaded into the game.";
    else if (g_targetPid) sub=formatStr(L"PID %lu  ·  press Inject", g_targetPid);
    else if (g_armed) sub=L"Watching for Minecraft...";
    else sub=L"Start Minecraft 1.8.9, then Inject.";
    RECT sb = {LEFT_X+20, STATUS_TOP+70, LEFT_X+LEFT_W-20, STATUS_TOP+70+20};
    drawTextIn(dc, g_fontBody, COLOUR_BODY, sb, sub.c_str());

    { RECT btn=buttonRect();
      SIZE lab=measureText(dc, g_fontButton, buttonLabel());
      int lx=btn.left+(btn.right-btn.left-lab.cx)/2;
      int ly=btn.top+(btn.bottom-btn.top-lab.cy)/2;
      drawText(dc, g_fontButton, g_buttonEnabled?COLOUR_WHITE:COLOUR_ACCENT_INK_OFF, lx, ly, buttonLabel());
    }
    RECT con = consoleRect();
    drawText(dc, g_fontSmall, COLOUR_MUTED, con.left+70, con.top+CONSOLE_STRIP/2-SIZE_SMALL/2-1, L"log");
    int textTop = con.top+CONSOLE_STRIP+CONSOLE_PAD;
    int textL = con.left+CONSOLE_PAD+2;
    int textR = con.right-CONSOLE_PAD-2;
    int visible = (con.bottom-CONSOLE_PAD-textTop)/CONSOLE_LINE;
    if (visible>0 && !g_console.empty()) {
      size_t first = g_console.size() > (size_t)visible ? g_console.size()-visible : 0;
      int ly = textTop;
      for (size_t i=first;i<g_console.size();i++) {
        const Line &ln = g_console[i];
        COLORREF c = ln.ink==Ink::GOOD?COLOUR_GOOD : ln.ink==Ink::BAD?COLOUR_BAD
                   : ln.ink==Ink::DEBUG?COLOUR_FAINT : COLOUR_MUTED;
        RECT box = {textL, ly, textR, ly+CONSOLE_LINE};
        drawTextIn(dc, g_fontConsole, c, box, (L"> "+ln.text).c_str());
        ly += CONSOLE_LINE;
      }
    }
    drawText(dc, g_fontSmall, COLOUR_DIM, LEFT_X, FOOTER_TOP,
             L"Myau Injector  ·  by sunnbt  ·  for Minecraft 1.8.9");
}

Ink inkFor(const std::wstring &t) {
    static const wchar_t *bad[] = {L"error",L"cannot",L"failed",L"expired",L"did not",
                                   L"stopped",L"not a Windows",L"empty file",L"32-bit",L"None of these"};
    for (auto *b : bad) if (t.find(b)!=std::wstring::npos) return Ink::BAD;
    if (t.find(L"loaded")!=std::wstring::npos) return Ink::GOOD;
    return Ink::NORMAL;
}
void addLine(const std::wstring &t, bool replaceLast, bool isDebug) {
    Ink ink = inkFor(t);
    if (isDebug && ink!=Ink::BAD) ink = Ink::DEBUG;
    if (replaceLast && !g_console.empty()) g_console.back() = Line{t, ink};
    else {
        g_console.push_back(Line{t, ink});
        if (g_console.size() > CONSOLE_MAX_LINES)
            g_console.erase(g_console.begin(), g_console.begin()+(g_console.size()-CONSOLE_MAX_LINES));
    }
    if (g_window) InvalidateRect(g_window, &consoleRect(), FALSE);
}
void postLine(const std::wstring &l) { PostMessageW(g_window, WM_LOG_LINE, 0, (LPARAM) new std::wstring(l)); }
void postSameLine(const std::wstring &l) { PostMessageW(g_window, WM_LOG_LINE, 1, (LPARAM) new std::wstring(l)); }
void postDebug(const std::wstring &) {}
void postProgress(double f) {
    if (f<0) f=0; if (f>1) f=1;
    PostMessageW(g_window, WM_PROGRESS, (WPARAM)(f*1000+0.5), 0);
}
std::vector<BYTE> embeddedDll() {
    HRSRC h = FindResourceA(nullptr, MAKEINTRESOURCEA(IDR_EMBEDDED_DLL), (LPCSTR)RT_RCDATA);
    if (!h) return {};
    HGLOBAL m = LoadResource(nullptr, h);
    if (!m) return {};
    DWORD s = SizeofResource(nullptr, h);
    const BYTE *d = (const BYTE*)LockResource(m);
    if (!d || s<2) return {};
    return std::vector<BYTE>(d, d+s);
}
void refreshTarget() {
    if (g_stage==Stage::WORKING || g_stage==Stage::DONE_OK) return;
    DWORD pid=0; std::wstring launcher;
    if (!findTarget(pid, launcher)) {
        if (g_targetPid) {
            g_targetPid=0; g_targetLauncher.clear();
            addLine(g_armed?L"the game is gone -- still waiting":L"the game is gone -- waiting for another", false, false);
            InvalidateRect(g_window, &leftCardRect(), FALSE);
        }
        return;
    }
    if (pid==g_targetPid && launcher==g_targetLauncher) return;
    g_targetPid=pid; g_targetLauncher=launcher;
    addLine(formatStr(L"found %s -- pid %lu", launcher.c_str(), pid), false, false);
    InvalidateRect(g_window, &leftCardRect(), FALSE);
    if (g_armed) {
        g_armed=false;
        addLine(L"game detected -- injecting", false, false);
        enterStage(Stage::WORKING);
        CloseHandle(CreateThread(nullptr, 0, injectThread, nullptr, 0, nullptr));
    }
}
DWORD WINAPI injectThread(LPVOID) {
    std::wstring url = payloadUrl();
    std::vector<BYTE> lib;
    std::wstring err;
    postProgress(0); postDebug(L"target ready");

    std::vector<BYTE> builtin = embeddedDll();
    if (!builtin.empty()) {
        if (builtin[0]!='M' || builtin[1]!='Z') {
            postLine(L"the built-in payload is not a Windows library -- rebuild the injector");
            PostMessageW(g_window, WM_INJECT_DONE, FALSE, 0); return 0;
        }
        postLine(L"using the built-in payload");
        postProgress(0.6);
        bool ok = runInjection(builtin.data(), builtin.size(), nullptr, 0, g_targetPid, postLine, postDebug);
        postProgress(ok?1.0:0.6);
        PostMessageW(g_window, WM_INJECT_DONE, ok?TRUE:FALSE, 0);
        return 0;
    }

    std::wstring cachePath = cacheDllPath();
    std::vector<BYTE> cached;
    bool haveCache = !cachePath.empty() && readFileBytes(cachePath, cached);
    std::wstring etag, lm;
    if (haveCache) readCacheMeta(etag, lm);

    postLine(L"connecting...");
    int lastPct=-1; bool newVer=false;
    FetchStatus st = downloadWithCache(url, lib, etag, lm,
        [&](unsigned long long d, unsigned long long t) {
            if (!t) return;
            int pct = (int)(d*100/t);
            if (pct==lastPct) return;
            bool first = lastPct<0;
            lastPct=pct;
            postProgress(0.6*d/(double)t);
            std::wstring l = formatStr(L"downloading  %3d%%", pct);
            if (first) postLine(l); else postSameLine(l);
        },
        [&]() { newVer=true; postLine(L"New version detected, downloading..."); },
        err);
    if (st==FetchStatus::Downloaded) {
        if (!cachePath.empty()) { writeFileBytes(cachePath, lib); writeCacheMeta(etag, lm); }
    } else if (st==FetchStatus::NotModified) {
        if (haveCache) { lib=cached; postLine(L"cached copy is up to date"); }
        else { err=L"server says not modified but no cached copy exists"; st=FetchStatus::Failed; }
    }
    if (st==FetchStatus::Failed) {
        if (haveCache) { postLine(L"download failed -- using cached copy"); lib=cached; }
        else { postLine(err); PostMessageW(g_window, WM_INJECT_DONE, FALSE, 0); return 0; }
    }
    if (lib.size()<2 || lib[0]!='M' || lib[1]!='Z') {
        postLine(L"what came back is not a Windows library -- check the download address");
        PostMessageW(g_window, WM_INJECT_DONE, FALSE, 0); return 0;
    }
    postProgress(0.6);
    postLine(formatStr(newVer?L"updated -- got %.1f MB":L"got %.1f MB", lib.size()/1048576.0));
    bool ok = runInjection(lib.data(), lib.size(), nullptr, 0, g_targetPid, postLine, postDebug);
    postProgress(ok?1.0:0.6);
    PostMessageW(g_window, WM_INJECT_DONE, ok?TRUE:FALSE, 0);
    return 0;
}
void enterStage(Stage s) {
    g_stage = s;
    switch (s) {
        case Stage::WORKING: g_buttonEnabled=false; g_progress=0; break;
        case Stage::DONE_OK: g_buttonEnabled=false; g_progress=1; break;
        case Stage::DONE_FAILED: g_armed=false; g_buttonEnabled=true; g_targetPid=0; refreshTarget(); break;
        default: g_progress=-1; g_armed=false; g_buttonEnabled=true; g_targetPid=0; refreshTarget(); break;
    }
    InvalidateRect(g_window, nullptr, FALSE);
}
void ensureBuffer(HDC ref) {
    if (g_memory) return;
    g_memory = CreateCompatibleDC(ref);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = WINDOW_WIDTH;
    info.bmiHeader.biHeight = -WINDOW_HEIGHT;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits=nullptr;
    g_memoryBitmap = CreateDIBSection(ref, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    g_memoryOld = SelectObject(g_memory, g_memoryBitmap);
}

LRESULT CALLBACK windowProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CREATE:
            g_console.push_back(Line{L"myau injector ready -- by sunnbt", Ink::NORMAL});
            g_console.push_back(Line{L"start Minecraft 1.8.9, then press Inject", Ink::NORMAL});
            g_buttonEnabled = true;
            SetTimer(w, TIMER_TARGET, TARGET_POLL_MS, nullptr);
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(w, &ps);
            ensureBuffer(dc);
            RECT all = {0,0,WINDOW_WIDTH,WINDOW_HEIGHT};
            FillRect(g_memory, &all, g_bgBrush);
            paintShapes(g_memory); paintText(g_memory);
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top,
                   ps.rcPaint.right-ps.rcPaint.left, ps.rcPaint.bottom-ps.rcPaint.top,
                   g_memory, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            EndPaint(w, &ps);
            return 0;
        }
        case WM_NCHITTEST: {
            POINT c = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(w, &c);
            if (PtInRect(&closeRect(), c)) return HTCLIENT;
            return c.y < HEADER_HEIGHT ? HTCAPTION : HTCLIENT;
        }
        case WM_TIMER: if (wp==TIMER_TARGET) refreshTarget(); return 0;
        case WM_MOUSEMOVE: {
            POINT c = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            bool bh = g_buttonEnabled && PtInRect(&buttonRect(), c);
            bool ch = PtInRect(&closeRect(), c) != FALSE;
            if (bh!=g_buttonHot) { g_buttonHot=bh; InvalidateRect(w, &buttonRect(), FALSE); }
            if (ch!=g_closeHot) { g_closeHot=ch; InvalidateRect(w, &closeRect(), FALSE); }
            TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, w, 0};
            TrackMouseEvent(&t);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_buttonHot||g_buttonDown) { g_buttonHot=g_buttonDown=false; InvalidateRect(w,&buttonRect(),FALSE); }
            if (g_closeHot) { g_closeHot=false; InvalidateRect(w,&closeRect(),FALSE); }
            return 0;
        case WM_LBUTTONDOWN: {
            POINT c = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (PtInRect(&closeRect(), c)) { PostMessageW(w, WM_CLOSE, 0, 0); return 0; }
            if (g_buttonEnabled && PtInRect(&buttonRect(), c)) {
                g_buttonDown=true; SetCapture(w);
                InvalidateRect(w, &buttonRect(), FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!g_buttonDown) return 0;
            g_buttonDown=false; ReleaseCapture();
            POINT c = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            InvalidateRect(w, &buttonRect(), FALSE);
            if (g_buttonEnabled && PtInRect(&buttonRect(), c) && g_stage!=Stage::WORKING) {
                if (g_armed) { g_armed=false; addLine(L"stopped waiting", false, false); }
                else if (g_targetPid) {
                    enterStage(Stage::WORKING);
                    CloseHandle(CreateThread(nullptr, 0, injectThread, nullptr, 0, nullptr));
                } else {
                    g_armed=true;
                    addLine(L"waiting for the game to start...", false, false);
                }
            }
            return 0;
        }
        case WM_LOG_LINE: {
            std::wstring *l = (std::wstring*)lp;
            addLine(*l, (wp&1)!=0, false);
            delete l;
            return 0;
        }
        case WM_PROGRESS:
            g_progress = (double)wp/1000.0;
            InvalidateRect(g_window, &progressRect(), FALSE);
            return 0;
        case WM_INJECT_DONE:
            enterStage(wp ? Stage::DONE_OK : Stage::DONE_FAILED);
            return 0;
        case WM_DESTROY:
            g_window = nullptr;
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(w, m, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    Gdiplus::GdiplusStartupInput su;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &su, nullptr);
    loadFont(); createFonts();
    g_logo = loadLogo();
    if (g_logo) g_logoContent = trimToContent(g_logo);
    g_bgBrush = CreateSolidBrush(COLOUR_BG);
    HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));

    WNDCLASSEXW cls = {sizeof(cls)};
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cls.hbrBackground = nullptr;
    cls.lpszClassName = L"MyauLoader";
    cls.hIcon = icon; cls.hIconSm = icon;
    RegisterClassExW(&cls);

    RECT want = {0,0,WINDOW_WIDTH,WINDOW_HEIGHT};
    AdjustWindowRect(&want, WS_POPUP, FALSE);
    g_window = CreateWindowExW(WS_EX_APPWINDOW, cls.lpszClassName, L"Myau Injector", WS_POPUP,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               want.right-want.left, want.bottom-want.top,
                               nullptr, nullptr, instance, nullptr);
    if (!g_window) return 1;
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    SetWindowPos(g_window, nullptr,
                 work.left+(work.right-work.left-WINDOW_WIDTH)/2,
                 work.top+(work.bottom-work.top-WINDOW_HEIGHT)/2,
                 WINDOW_WIDTH, WINDOW_HEIGHT, SWP_NOZORDER);
    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(g_window, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(g_window, 19, &dark, sizeof(dark));
    DWORD corner = 2;
    DwmSetWindowAttribute(g_window, 33, &corner, sizeof(corner));
    ShowWindow(g_window, show);
    UpdateWindow(g_window);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_memory) {
        SelectObject(g_memory, g_memoryOld);
        DeleteObject(g_memoryBitmap);
        DeleteDC(g_memory);
    }
    destroyFonts();
    if (g_fontResource) RemoveFontMemResourceEx(g_fontResource);
    DeleteObject(g_bgBrush);
    delete g_logo;
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    return 0;
}
