#include "WheelPicture.h"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef WHEEL_PICTURE_TEST // (a test of the drawing alone, outside the game, includes this file with it defined)
#include <spdlog/spdlog.h>
#include <mods/VR.hpp>
#endif

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace WheelPicture
{
    namespace
    {
        constexpr float kPi           = 3.14159265f;
        constexpr float kPxPerM       = static_cast<float>(kPixels) / kSizeM;
        constexpr float kMid          = static_cast<float>(kPixels) * 0.5f;
        constexpr float kInnerPx      = kInnerM * kPxPerM;  // (about 125)
        constexpr float kOuterPx      = kOuterM * kPxPerM;  // (about 470)
        constexpr float kHoverOuterPx = 503.0f;             // a hovered slice drawn out to here (the hands' slice grows)
        constexpr float kSliceGapPx   = 1.6f;               // half the gap between two slices
        constexpr float kHalfSlice    = kPi / 12.0f;        // (15 degrees)
        constexpr float kTextRadius   = 318.0f;             // the middle of each slice's text
        constexpr float kSymbolRadius = 432.0f;             // each slice's small symbol, toward the rim
        constexpr int   kTextWidth    = 156;                // each slice's text box
        constexpr int   kMidTextWidth = 206;                // the middle circle's

        // Linear colours (the picture is stored as sRGB, premultiplied: the gun sight's way).
        struct Colour
        {
            float r, g, b;
        };
        constexpr Colour kDisc{ 0.008f, 0.010f, 0.016f };
        constexpr Colour kSlice{ 0.030f, 0.038f, 0.055f };
        constexpr Colour kSliceEmpty{ 0.018f, 0.020f, 0.026f };
        constexpr Colour kMidFill{ 0.014f, 0.018f, 0.026f };
        constexpr Colour kMidRing{ 0.20f, 0.22f, 0.26f };
        constexpr Colour kLeftFill{ 0.035f, 0.10f, 0.28f };
        constexpr Colour kLeftLine{ 0.25f, 0.58f, 1.00f };
        constexpr Colour kRightFill{ 0.28f, 0.10f, 0.025f };
        constexpr Colour kRightLine{ 1.00f, 0.48f, 0.12f };
        constexpr Colour kBothFill{ 0.20f, 0.08f, 0.24f };
        constexpr Colour kBothLine{ 0.95f, 0.85f, 1.00f };
        constexpr Colour kNoLeftLine{ 0.30f, 0.30f, 0.32f };
        constexpr Colour kText{ 0.90f, 0.90f, 0.90f };
        constexpr Colour kTextDim{ 0.48f, 0.50f, 0.54f };
        constexpr Colour kTextFaint{ 0.20f, 0.21f, 0.23f };
        constexpr Colour kMarkLeft{ 0.30f, 0.62f, 1.00f };
        constexpr Colour kMarkRight{ 1.00f, 0.52f, 0.16f };
        constexpr Colour kMarkUse{ 0.32f, 0.90f, 0.40f };
        constexpr Colour kMarkWorn{ 0.65f, 0.65f, 0.65f };
        constexpr Colour kNote{ 1.00f, 0.70f, 0.35f };

        // The picture being drawn: premultiplied linear colour and coverage, four floats a pixel.
        struct Canvas
        {
            std::vector<float> px{};

            void clear() { std::fill(px.begin(), px.end(), 0.0f); }

            // Over what is there: `a` is the colour's opacity times its coverage of the pixel.
            void put(int x, int y, const Colour& c, float a)
            {
                if (a <= 0.0f || x < 0 || y < 0 || x >= kPixels || y >= kPixels) {
                    return;
                }
                a              = std::min(a, 1.0f);
                float*      p  = &px[(static_cast<size_t>(y) * kPixels + static_cast<size_t>(x)) * 4];
                const float k  = 1.0f - a;
                p[0]           = c.r * a + p[0] * k;
                p[1]           = c.g * a + p[1] * k;
                p[2]           = c.b * a + p[2] * k;
                p[3]           = a + p[3] * k;
            }
        };

        float Cover(float signed_distance) { return std::clamp(signed_distance + 0.5f, 0.0f, 1.0f); }

        int Lo(float v) { return std::clamp(static_cast<int>(std::floor(v)), 0, kPixels - 1); }
        int Hi(float v) { return std::clamp(static_cast<int>(std::ceil(v)), 0, kPixels - 1); }

        void Disc(Canvas& cv, float cx, float cy, float radius, const Colour& c, float alpha)
        {
            for (int y = Lo(cy - radius - 1); y <= Hi(cy + radius + 1); ++y) {
                for (int x = Lo(cx - radius - 1); x <= Hi(cx + radius + 1); ++x) {
                    const float dx = static_cast<float>(x) + 0.5f - cx, dy = static_cast<float>(y) + 0.5f - cy;
                    cv.put(x, y, c, alpha * Cover(radius - std::sqrt(dx * dx + dy * dy)));
                }
            }
        }

        void Ring(Canvas& cv, float cx, float cy, float radius, float width, const Colour& c, float alpha)
        {
            const float outer = radius + width;
            for (int y = Lo(cy - outer - 1); y <= Hi(cy + outer + 1); ++y) {
                for (int x = Lo(cx - outer - 1); x <= Hi(cx + outer + 1); ++x) {
                    const float dx = static_cast<float>(x) + 0.5f - cx, dy = static_cast<float>(y) + 0.5f - cy;
                    const float d  = std::sqrt(dx * dx + dy * dy);
                    cv.put(x, y, c, alpha * Cover(width * 0.5f - std::abs(d - radius)));
                }
            }
        }

        // A capsule (a line with round ends), for the small symbols.
        void Capsule(Canvas& cv, float ax, float ay, float bx, float by, float radius, const Colour& c, float alpha)
        {
            const float ex = bx - ax, ey = by - ay;
            const float ll = std::max(ex * ex + ey * ey, 1e-6f);
            for (int y = Lo(std::min(ay, by) - radius - 1); y <= Hi(std::max(ay, by) + radius + 1); ++y) {
                for (int x = Lo(std::min(ax, bx) - radius - 1); x <= Hi(std::max(ax, bx) + radius + 1); ++x) {
                    const float px = static_cast<float>(x) + 0.5f - ax, py = static_cast<float>(y) + 0.5f - ay;
                    const float t  = std::clamp((px * ex + py * ey) / ll, 0.0f, 1.0f);
                    const float dx = px - ex * t, dy = py - ey * t;
                    cv.put(x, y, c, alpha * Cover(radius - std::sqrt(dx * dx + dy * dy)));
                }
            }
        }

        void Box(Canvas& cv, float x0, float y0, float x1, float y1, float round, const Colour& c, float alpha)
        {
            const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
            for (int y = Lo(y0 - 1); y <= Hi(y1 + 1); ++y) {
                for (int x = Lo(x0 - 1); x <= Hi(x1 + 1); ++x) {
                    const float qx = std::abs(static_cast<float>(x) + 0.5f - cx) - (hx - round);
                    const float qy = std::abs(static_cast<float>(y) + 0.5f - cy) - (hy - round);
                    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
                    const float d  = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - round;
                    cv.put(x, y, c, alpha * Cover(-d));
                }
            }
        }

        // One slice of the ring (centred at `angle`, counterclockwise as seen, between radii r0 and r1, `half` either side less the gap):
        // filled, and its edge `line` px wide in another colour when given.
        void Slice(Canvas& cv, float angle, float r0, float r1, float half, const Colour& fill, float fill_alpha, const Colour* line, float line_alpha,
            float line_width)
        {
            // (Its box: the arcs' ends and the points between.)
            float x0 = kMid, y0 = kMid, x1 = kMid, y1 = kMid;
            for (int i = 0; i <= 8; ++i) {
                const float a = angle - half + (2.0f * half) * static_cast<float>(i) / 8.0f;
                for (const float r : { r0, r1 }) {
                    const float x = kMid + std::cos(a) * r, y = kMid - std::sin(a) * r;
                    x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
                }
            }
            const float ca = std::cos(angle), sa = std::sin(angle);
            const float lo2 = (r0 - 1.0f) * (r0 - 1.0f), hi2 = (r1 + 1.0f) * (r1 + 1.0f);
            const float cone = std::cos(std::min(half + 0.06f, kPi * 0.45f)); // (pixels outside this cone skipped before the angle is worked out)
            for (int y = Lo(y0 - 2); y <= Hi(y1 + 2); ++y) {
                for (int x = Lo(x0 - 2); x <= Hi(x1 + 2); ++x) {
                    const float dx = static_cast<float>(x) + 0.5f - kMid, dy = kMid - (static_cast<float>(y) + 0.5f);
                    const float r2 = dx * dx + dy * dy;
                    if (r2 < lo2 || r2 > hi2) {
                        continue;
                    }
                    const float r     = std::sqrt(r2);
                    const float along = dx * ca + dy * sa;
                    if (along < cone * r) {
                        continue;
                    }
                    const float delta = std::atan2(dy * ca - dx * sa, along); // (the angle from the slice's middle)
                    // (Signed distance inside it, in pixels: to its arcs and to its two edges.)
                    const float s = std::min({ r - r0, r1 - r, (half - std::abs(delta)) * r - kSliceGapPx });
                    const float c = Cover(s);
                    if (c <= 0.0f) {
                        continue;
                    }
                    cv.put(x, y, fill, fill_alpha * c);
                    if (line != nullptr) {
                        cv.put(x, y, *line, line_alpha * c * Cover(line_width - s));
                    }
                }
            }
        }

        // The small symbol of a kind (drawn shapes: the game's icons are Flash pictures the mod cannot copy), `s` its half size.
        void Symbol(Canvas& cv, int kind, float sx, float sy, float s, const Colour& c, float alpha)
        {
            switch (kind) {
            case kMelee: {
                // A blade from the lower left to the upper right, its guard across a third of the way up.
                const float hx = sx - 0.9f * s, hy = sy + 0.9f * s, tx = sx + 0.9f * s, ty = sy - 0.9f * s;
                Capsule(cv, hx, hy, tx, ty, 0.13f * s, c, alpha);
                const float gx = hx + (tx - hx) * 0.3f, gy = hy + (ty - hy) * 0.3f, k = 0.42f * s * 0.7071f;
                Capsule(cv, gx - k, gy - k, gx + k, gy + k, 0.12f * s, c, alpha);
                break;
            }
            case kGun:
                Capsule(cv, sx - 0.9f * s, sy - 0.3f * s, sx + 0.9f * s, sy - 0.3f * s, 0.22f * s, c, alpha);
                Capsule(cv, sx - 0.45f * s, sy - 0.1f * s, sx - 0.75f * s, sy + 0.75f * s, 0.2f * s, c, alpha);
                break;
            case kThrown:
                Disc(cv, sx, sy + 0.15f * s, 0.6f * s, c, alpha);
                Capsule(cv, sx - 0.1f * s, sy - 0.6f * s, sx + 0.4f * s, sy - 0.8f * s, 0.12f * s, c, alpha);
                break;
            case kAid:
                Capsule(cv, sx - 0.75f * s, sy, sx + 0.75f * s, sy, 0.24f * s, c, alpha);
                Capsule(cv, sx, sy - 0.75f * s, sx, sy + 0.75f * s, 0.24f * s, c, alpha);
                break;
            case kApparel:
                Box(cv, sx - 0.6f * s, sy - 0.75f * s, sx + 0.6f * s, sy + 0.75f * s, 0.25f * s, c, alpha);
                break;
            case kPower:
                Ring(cv, sx, sy, 0.7f * s, 0.18f * s, c, alpha);
                Disc(cv, sx, sy, 0.25f * s, c, alpha);
                break;
            case kOther:
                Disc(cv, sx, sy, 0.35f * s, c, alpha);
                break;
            default:
                break;
            }
        }

        std::wstring Wide(const std::string& s)
        {
            if (s.empty()) {
                return {};
            }
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            if (n <= 0) {
                return std::wstring(s.begin(), s.end());
            }
            std::wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
            return w;
        }

        // Windows' own text drawing (GDI): white on black into a picture of its own, its brightness then the text's coverage, laid into the
        // canvas in the text's colour. Segoe UI, smoothed (not ClearType: the headset's pixels are not the screen's).
        enum FontKind : int
        {
            kFontName = 0,
            kFontDetail,
            kFontMark,
            kFontMid,
            kFontMidDetail,
            kFonts
        };
        struct Gdi
        {
            HDC       dc{ nullptr };
            HBITMAP   bitmap{ nullptr };
            HGDIOBJ   old{ nullptr };
            uint32_t* bits{ nullptr };
            int       width{ 1024 };
            int       height{ 400 };
            HFONT     fonts[kFonts]{};
            int       line[kFonts]{}; // (each font's line height)

            bool Make()
            {
                dc = CreateCompatibleDC(nullptr);
                if (dc == nullptr) {
                    return false;
                }
                BITMAPINFO info{};
                info.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth       = width;
                info.bmiHeader.biHeight      = -height; // (top to bottom)
                info.bmiHeader.biPlanes      = 1;
                info.bmiHeader.biBitCount    = 32;
                info.bmiHeader.biCompression = BI_RGB;
                void* memory                 = nullptr;
                bitmap                       = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &memory, nullptr, 0);
                if (bitmap == nullptr || memory == nullptr) {
                    return false;
                }
                bits = static_cast<uint32_t*>(memory);
                old  = SelectObject(dc, bitmap);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(255, 255, 255));
                SetTextAlign(dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
                constexpr int sizes[kFonts]   = { 25, 21, 19, 32, 22 };
                constexpr int weights[kFonts] = { FW_SEMIBOLD, FW_NORMAL, FW_BOLD, FW_BOLD, FW_NORMAL };
                for (int i = 0; i < kFonts; ++i) {
                    fonts[i] = CreateFontW(-sizes[i], 0, 0, 0, weights[i], FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                    if (fonts[i] == nullptr) {
                        return false;
                    }
                    SelectObject(dc, fonts[i]);
                    TEXTMETRICW tm{};
                    GetTextMetricsW(dc, &tm);
                    line[i] = std::clamp(static_cast<int>(tm.tmHeight), sizes[i], sizes[i] * 2);
                }
                return true;
            }

            int Width(FontKind font, const std::wstring& text)
            {
                SelectObject(dc, fonts[font]);
                SIZE size{};
                GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
                return size.cx;
            }

            // A line cut to fit `max_width` with "..." at its end.
            void Cut(FontKind font, std::wstring& text, int max_width)
            {
                while (!text.empty() && Width(font, text + L"\x2026") > max_width) {
                    text.pop_back();
                }
                while (!text.empty() && text.back() == L' ') {
                    text.pop_back();
                }
                text += L"\x2026";
            }

            // The text broken into lines no wider than `max_width` (at most `most`; the last one cut with "..." when more is left, and any
            // one word wider than the box cut too).
            std::vector<std::wstring> Wrap(FontKind font, const std::wstring& text, int max_width, int most)
            {
                std::vector<std::wstring> lines;
                std::vector<std::wstring> words;
                for (size_t i = 0; i < text.size();) {
                    const size_t j = text.find(L' ', i);
                    const size_t e = j == std::wstring::npos ? text.size() : j;
                    if (e > i) {
                        words.push_back(text.substr(i, e - i));
                    }
                    i = e + 1;
                }
                size_t w = 0;
                while (w < words.size() && static_cast<int>(lines.size()) < most) {
                    std::wstring text_line = words[w++];
                    while (w < words.size() && Width(font, text_line + L" " + words[w]) <= max_width) {
                        text_line += L" " + words[w++];
                    }
                    lines.push_back(std::move(text_line));
                }
                for (auto& l : lines) {
                    if (Width(font, l) > max_width) {
                        Cut(font, l, max_width);
                    }
                }
                if (!lines.empty() && w < words.size() && (lines.back().empty() || lines.back().back() != L'\x2026')) {
                    lines.back() += L" " + words[w];
                    Cut(font, lines.back(), max_width);
                }
                return lines;
            }

            // Lines centred on `cx`, from `top` down; returns the height drawn.
            int Draw(Canvas& cv, FontKind font, const std::vector<std::wstring>& lines, float cx, float top, const Colour& c, float alpha)
            {
                const int lh = line[font];
                if (lines.empty() || lh * static_cast<int>(lines.size()) > height) {
                    return 0;
                }
                SelectObject(dc, fonts[font]);
                int widths[8]{};
                int most = 0;
                for (size_t i = 0; i < lines.size() && i < 8; ++i) {
                    widths[i] = std::min(Width(font, lines[i]) + 4, width - 8);
                    most      = std::max(most, widths[i]);
                }
                for (int y = 0; y < lh * static_cast<int>(lines.size()); ++y) {
                    std::memset(bits + static_cast<size_t>(y) * width, 0, static_cast<size_t>(most + 8) * 4);
                }
                for (size_t i = 0; i < lines.size() && i < 8; ++i) {
                    TextOutW(dc, 2, static_cast<int>(i) * lh, lines[i].c_str(), static_cast<int>(lines[i].size()));
                }
                GdiFlush();
                for (size_t i = 0; i < lines.size() && i < 8; ++i) {
                    const int left = static_cast<int>(std::lround(cx - static_cast<float>(widths[i]) * 0.5f));
                    const int y0   = static_cast<int>(std::lround(top)) + static_cast<int>(i) * lh;
                    for (int y = 0; y < lh; ++y) {
                        const uint32_t* row = bits + static_cast<size_t>(static_cast<int>(i) * lh + y) * width;
                        for (int x = 0; x < widths[i] + 4 && x < width; ++x) {
                            const uint32_t p = row[x];
                            const int      v = std::max({ static_cast<int>(p & 0xFF), static_cast<int>((p >> 8) & 0xFF), static_cast<int>((p >> 16) & 0xFF) });
                            if (v != 0) {
                                cv.put(left + x, y0 + y, c, alpha * static_cast<float>(v) / 255.0f);
                            }
                        }
                    }
                }
                return lh * static_cast<int>(lines.size());
            }
        };

        // Each slice's middle for its text (picture pixels).
        void SlicePoint(int slice, float radius, float& x, float& y)
        {
            const float a = static_cast<float>(slice) * (kPi / 6.0f);
            x             = kMid + std::cos(a) * radius;
            y             = kMid - std::sin(a) * radius;
        }

        // The picture of `c` into `cv`.
        void Render(Canvas& cv, Gdi* gdi, const Content& c)
        {
            cv.clear();
            Disc(cv, kMid, kMid, kOuterPx + 10.0f, kDisc, 0.62f);
            // The slices (the hovered ones drawn last, over their neighbours' gaps).
            int order[12]{};
            int n = 0;
            for (int pass = 0; pass < 2; ++pass) {
                for (int k = 0; k < 12; ++k) {
                    const int  slot    = kSliceSlot[k];
                    const bool hovered = c.hover[0] == slot || c.hover[1] == slot;
                    if (hovered == (pass == 1)) {
                        order[n++] = k;
                    }
                }
            }
            for (int i = 0; i < 12; ++i) {
                const int   k     = order[i];
                const int   slot  = kSliceSlot[k];
                const Card& card  = c.cards[slot];
                const bool  left  = c.hover[0] == slot;
                const bool  right = c.hover[1] == slot;
                const float angle = static_cast<float>(k) * (kPi / 6.0f);
                const bool  empty = card.kind == kEmpty;
                if (!left && !right) {
                    Slice(cv, angle, kInnerPx + 6.0f, kOuterPx, kHalfSlice, empty ? kSliceEmpty : kSlice, empty ? 0.45f : 0.72f, nullptr, 0.0f, 0.0f);
                    continue;
                }
                // (The left hand on an empty slot: its own colour left out, a grey edge. fix174: anything else is the left hand's to choose:
                // a melee weapon into the left hand, the rest used the game's own way.)
                const bool    left_can = left && !empty;
                const Colour* line     = left && right ? &kBothLine : right ? &kRightLine : left_can ? &kLeftLine : &kNoLeftLine;
                const Colour& fill     = left && right ? kBothFill : right ? kRightFill : left_can ? kLeftFill : kSlice;
                Slice(cv, angle, kInnerPx + 2.0f, kHoverOuterPx, kHalfSlice + 0.012f, fill, empty ? 0.6f : 0.9f, line, 1.0f, 3.5f);
            }
            // The middle circle (picks nothing).
            Disc(cv, kMid, kMid, kInnerPx, kMidFill, 0.88f);
            Ring(cv, kMid, kMid, kInnerPx - 1.5f, 2.0f, kMidRing, 0.7f);

            // Each slice's symbol and text.
            for (int k = 0; k < 12; ++k) {
                const int   slot  = kSliceSlot[k];
                const Card& card  = c.cards[slot];
                const bool  hover = c.hover[0] == slot || c.hover[1] == slot;
                float       tx = 0.0f, ty = 0.0f, sx = 0.0f, sy = 0.0f;
                SlicePoint(k, kTextRadius, tx, ty);
                SlicePoint(k, kSymbolRadius + (hover ? 18.0f : 0.0f), sx, sy);
                if (card.kind == kEmpty) {
                    if (gdi != nullptr) {
                        gdi->Draw(cv, kFontDetail, { c.reading ? L"\x2026" : L"empty" }, tx, ty - 12.0f, kTextFaint, 1.0f);
                    }
                    continue;
                }
                const float dim = card.none ? 0.55f : 1.0f;
                Symbol(cv, card.kind, sx, sy, 15.0f, kTextDim, 0.9f * dim);
                if (gdi == nullptr) {
                    continue;
                }
                const auto name   = gdi->Wrap(kFontName, Wide(card.name), kTextWidth, 2);
                const auto detail = card.detail.empty() ? std::vector<std::wstring>{} : gdi->Wrap(kFontDetail, Wide(card.detail), kTextWidth, 1);
                // The marks: what each hand holds, the power in use, apparel worn.
                std::vector<std::pair<std::wstring, Colour>> marks;
                if (card.left) {
                    marks.emplace_back(L"L", kMarkLeft);
                }
                if (card.right) {
                    marks.emplace_back(L"R", kMarkRight);
                }
                if (card.in_use) {
                    marks.emplace_back(L"in use", kMarkUse);
                }
                if (card.worn) {
                    marks.emplace_back(L"worn", kMarkWorn);
                }
                const int name_h   = gdi->line[kFontName] * static_cast<int>(name.size());
                const int detail_h = gdi->line[kFontDetail] * static_cast<int>(detail.size());
                const int marks_h  = marks.empty() ? 0 : gdi->line[kFontMark];
                float     top      = ty - static_cast<float>(name_h + detail_h + marks_h) * 0.5f;
                top += static_cast<float>(gdi->Draw(cv, kFontName, name, tx, top, kText, (hover ? 1.0f : 0.92f) * dim));
                top += static_cast<float>(gdi->Draw(cv, kFontDetail, detail, tx, top, kTextDim, dim));
                if (!marks.empty()) {
                    int total = 0;
                    for (const auto& [text, colour] : marks) {
                        total += gdi->Width(kFontMark, text) + 10;
                    }
                    float x = tx - static_cast<float>(total - 10) * 0.5f;
                    for (const auto& [text, colour] : marks) {
                        const int w = gdi->Width(kFontMark, text);
                        gdi->Draw(cv, kFontMark, { text }, x + static_cast<float>(w) * 0.5f, top, colour, 1.0f);
                        x += static_cast<float>(w + 10);
                    }
                }
            }

            // The middle: the slice a hand came to last, large; else what the wheel is.
            if (gdi == nullptr) {
                return;
            }
            std::vector<std::wstring> big, lower;
            Colour                    lower_colour = kTextDim;
            if (c.reading) {
                lower = { L"reading your", L"favorites\x2026" };
            } else if (c.focus >= 0 && c.focus < 12 && c.cards[c.focus].kind != kEmpty) {
                const Card& card = c.cards[c.focus];
                big              = gdi->Wrap(kFontMid, Wide(card.name), kMidTextWidth, 3);
                // (fix174: no "left hand: melee weapons only" note any more: the left hand's choice of anything else is used as the game uses it.)
                if (!card.detail.empty()) {
                    lower = gdi->Wrap(kFontMidDetail, Wide(card.detail), kMidTextWidth, 1);
                    if (!card.more.empty()) {
                        const auto more = gdi->Wrap(kFontMidDetail, Wide(card.more), kMidTextWidth, 2);
                        lower.insert(lower.end(), more.begin(), more.end());
                    }
                }
            } else if (c.focus >= 0 && c.focus < 12) {
                lower = { L"empty" };
            } else {
                lower = { L"Favorites", L"let go to choose" };
            }
            const int big_h   = gdi->line[kFontMid] * static_cast<int>(big.size());
            const int lower_h = gdi->line[kFontMidDetail] * static_cast<int>(lower.size());
            float     top     = kMid - static_cast<float>(big_h + lower_h) * 0.5f;
            top += static_cast<float>(gdi->Draw(cv, kFontMid, big, kMid, top, kText, 1.0f));
            gdi->Draw(cv, kFontMidDetail, lower, kMid, top, lower_colour, 1.0f);
        }

        // Linear light to sRGB bytes (a table: 4096 steps).
        const std::array<uint8_t, 4096>& SrgbTable()
        {
            static const auto table = [] {
                std::array<uint8_t, 4096> t{};
                for (int i = 0; i < 4096; ++i) {
                    const float c = static_cast<float>(i) / 4095.0f;
                    const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
                    t[i]          = static_cast<uint8_t>(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
                }
                return t;
            }();
            return table;
        }

        void Encode(const Canvas& cv, std::vector<uint32_t>& out)
        {
            const auto& t = SrgbTable();
            out.resize(static_cast<size_t>(kPixels) * kPixels);
            const auto at = [](float v) { return static_cast<size_t>(std::clamp(v, 0.0f, 1.0f) * 4095.0f + 0.5f); };
            for (size_t i = 0; i < out.size(); ++i) {
                const float* p = &cv.px[i * 4];
                const auto   a = static_cast<uint32_t>(std::lround(std::clamp(p[3], 0.0f, 1.0f) * 255.0f));
                out[i]         = t[at(p[0])] | (static_cast<uint32_t>(t[at(p[1])]) << 8) | (static_cast<uint32_t>(t[at(p[2])]) << 16) | (a << 24);
            }
        }

        // A picture saved for the log (PNG, through Windows' own image coding, WIC), laid over an even grey so it can be looked at as the
        // headset shows it over the world. The drawing thread's own (never the game's): S_OK when saved.
        HRESULT SavePng(const std::wstring& path, const std::vector<uint32_t>& pixels)
        {
            if (pixels.size() != static_cast<size_t>(kPixels) * kPixels) {
                return E_INVALIDARG;
            }
            const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            std::vector<uint32_t> bgra(pixels.size());
            for (size_t i = 0; i < pixels.size(); ++i) {
                // (Premultiplied: shown over a grey of 64 as colour + grey x (1 - alpha), each channel.)
                const uint32_t p = pixels[i];
                const uint32_t a = p >> 24;
                const auto     over = [&](uint32_t c) { return std::min<uint32_t>(255, c + 64 * (255 - a) / 255); };
                bgra[i]             = over((p >> 16) & 0xFF) | (over((p >> 8) & 0xFF) << 8) | (over(p & 0xFF) << 16) | 0xFF000000u;
            }
            IWICImagingFactory*    factory = nullptr;
            IWICStream*            stream  = nullptr;
            IWICBitmapEncoder*     encoder = nullptr;
            IWICBitmapFrameEncode* frame   = nullptr;
            HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            if (SUCCEEDED(hr)) {
                hr = factory->CreateStream(&stream);
            }
            if (SUCCEEDED(hr)) {
                hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
            }
            if (SUCCEEDED(hr)) {
                hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
            }
            if (SUCCEEDED(hr)) {
                hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
            }
            if (SUCCEEDED(hr)) {
                hr = encoder->CreateNewFrame(&frame, nullptr);
            }
            if (SUCCEEDED(hr)) {
                hr = frame->Initialize(nullptr);
            }
            if (SUCCEEDED(hr)) {
                hr = frame->SetSize(kPixels, kPixels);
            }
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
            if (SUCCEEDED(hr)) {
                hr = frame->SetPixelFormat(&format);
            }
            if (SUCCEEDED(hr) && format != GUID_WICPixelFormat32bppBGRA) {
                hr = E_FAIL;
            }
            if (SUCCEEDED(hr)) {
                hr = frame->WritePixels(kPixels, kPixels * 4, static_cast<UINT>(bgra.size() * 4), reinterpret_cast<BYTE*>(bgra.data()));
            }
            if (SUCCEEDED(hr)) {
                hr = frame->Commit();
            }
            if (SUCCEEDED(hr)) {
                hr = encoder->Commit();
            }
            for (IUnknown* u : { static_cast<IUnknown*>(frame), static_cast<IUnknown*>(encoder), static_cast<IUnknown*>(stream), static_cast<IUnknown*>(factory) }) {
                if (u != nullptr) {
                    u->Release();
                }
            }
            if (SUCCEEDED(init)) {
                CoUninitialize();
            }
            return hr;
        }

#ifndef WHEEL_PICTURE_TEST
        std::mutex              g_mtx;
        std::condition_variable g_cv;
        Content                 g_want{};
        uint64_t                g_asked{ 0 };
        bool                    g_started{ false };
        std::atomic<uint64_t>   g_drawn{ 0 };
        std::atomic<int>        g_last_us{ 0 };
        std::atomic<uint64_t>   g_serial{ 0 };

        void Loop()
        {
            // (Below the game's own threads: a picture a moment later rather than a game thread kept waiting.)
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            Gdi        gdi;
            const bool gdi_ok = gdi.Make();
            if (!gdi_ok) {
                spdlog::warn("[Wheel] (fix171) Windows' text drawing could not be set up: the wheel shows its slices and symbols without names");
            }
            Canvas cv;
            cv.px.resize(static_cast<size_t>(kPixels) * kPixels * 4);
            uint64_t done = 0;
            int      logs = 0;
            bool     saved_open = false, saved_hover = false; // (the session's first pictures saved for the log)
            for (;;) {
                Content  c{};
                uint64_t asked = 0;
                {
                    std::unique_lock lock{ g_mtx };
                    g_cv.wait(lock, [&] { return g_asked != done; });
                    c     = g_want;
                    asked = g_asked;
                }
                done = asked;
                try {
                    const auto t0 = std::chrono::steady_clock::now();
                    Render(cv, gdi_ok ? &gdi : nullptr, c);
                    auto picture    = std::make_shared<runtimes::OpenXR::WheelPicture>();
                    picture->width  = kPixels;
                    picture->height = kPixels;
                    picture->open   = c.open;
                    picture->serial = g_serial.fetch_add(1) + 1;
                    Encode(cv, picture->pixels);
                    const int us = static_cast<int>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
                    g_last_us.store(us);
                    g_drawn.fetch_add(1);
                    if (logs < 8) {
                        ++logs;
                        spdlog::info("[Wheel] (fix171) its picture drawn in {:.1f} ms ({}x{}, opening {})", us / 1000.0, kPixels, kPixels, c.open);
                    }
                    // (For the log, once a session each: its first picture with the favorites read, and its first with a hand on a slice.)
                    const bool hovered = c.hover[0] >= 0 || c.hover[1] >= 0;
                    if (!c.reading && ((!saved_open && !hovered) || (!saved_hover && hovered))) {
                        (hovered ? saved_hover : saved_open) = true;
                        const auto path = Framework::get_persistent_dir(hovered ? "wheel_picture_hover.png" : "wheel_picture.png");
                        const auto hr   = SavePng(path.wstring(), picture->pixels);
                        spdlog::info("[Wheel] (fix171) its picture saved to {} (hr {:x})", path.string(), static_cast<uint32_t>(hr));
                    }
                    if (auto xr = VR::get()->m_openxr; xr != nullptr) { // (a copy: it may be made anew on another thread)
                        xr->set_wheel_picture(std::move(picture));
                    }
                } catch (const std::exception& e) {
                    if (logs < 8) {
                        ++logs;
                        spdlog::error("[Wheel] (fix171) its picture could not be drawn: {}", e.what());
                    }
                }
            }
        }
#endif
    }

#ifndef WHEEL_PICTURE_TEST
    void Draw(const Content& content)
    {
        std::scoped_lock _{ g_mtx };
        if (!g_started) {
            g_started = true;
            std::thread(Loop).detach();
        }
        g_want = content;
        ++g_asked;
        g_cv.notify_one();
    }

    std::string Status() { return std::format("{} pictures drawn, the last in {:.1f} ms", g_drawn.load(), g_last_us.load() / 1000.0); }
#endif
}
