/*
 * QuizFabric client for the Waveshare ESP32-S3-Touch-LCD-7B.
 *
 * UI styled after the 3DS client's bottom screen: pink/purple animated
 * gradient, glass panels, coloured answer tiles (same palette as the 3DS).
 * The 3DS top-screen content (question + image) is fitted into the upper
 * part of this single screen.
 *
 * GIFs / animated question images are NOT loaded (too slow on the ESP32).
 */
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "esp32s3/rom/cache.h"
#include "rgb_lcd_port.h"
#include "i2c.h"
#include "io_extension.h"
#include "gt911.h"
#include "font8x8_basic.h"

// ------------------------------------------------------------------ config
const char *WIFI_SSID = "VM6842809";
const char *WIFI_PASS = "h2hFnrycpprn";

String SERVER_HOST = "play.lucasgeitgey.com";
int SERVER_PORT = 80;

// ------------------------------------------------------------------ display
static const int FB_W = EXAMPLE_LCD_H_RES;   // 1024
static const int FB_H = EXAMPLE_LCD_V_RES;   // 600
uint16_t *fb = nullptr;

#define RGB(r, g, b) ((uint16_t)((((r) * 31 + 127) / 255) << 11) | \
                       ((((g) * 63 + 127) / 255) << 5) | \
                       (((b) * 31 + 127) / 255))

// 3DS palette
const uint16_t C_INK     = RGB(46, 16, 70);
const uint16_t C_GLASS   = RGB(141, 105, 183);   // white 18% over purple
const uint16_t C_GLASS_HI= RGB(167, 135, 205);   // white 36% over purple
const uint16_t C_GOLD    = RGB(255, 208, 92);
const uint16_t C_GOOD    = RGB(58, 214, 160);
const uint16_t C_BAD     = RGB(255, 92, 116);
const uint16_t C_TEXT    = RGB(255, 255, 255);
const uint16_t C_DIM     = RGB(235, 235, 245);
const uint16_t C_FAINT   = RGB(200, 185, 215);

uint16_t ansColor(int i) {
    switch (i & 7) {
        case 0: return RGB(255, 79, 129);
        case 1: return RGB(124, 77, 255);
        case 2: return RGB(255, 159, 69);
        case 3: return RGB(56, 224, 200);
        case 4: return RGB(255, 110, 199);
        case 5: return RGB(104, 132, 255);
        case 6: return RGB(196, 116, 255);
        default: return RGB(90, 214, 140);
    }
}

// ------------------------------------------------------------------ drawing
void fillRect(int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_W) w = FB_W - x;
    if (y + h > FB_H) h = FB_H - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) fb[yy * FB_W + xx] = c;
}

uint16_t lerpColor(uint16_t a, uint16_t b, float t) {
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + (br - ar) * t;
    int g = ag + (bg - ag) * t;
    int bl = ab + (bb - ab) * t;
    return (r << 11) | (g << 5) | bl;
}

// Animated pink/purple vertical gradient, like the 3DS background.
void drawBackground(void) {
    float s = sinf(millis() * 0.00035f) * 0.5f + 0.5f;
    uint16_t topA = RGB(255, 138, 205), botA = RGB(96, 42, 176);
    uint16_t topB = RGB(255, 106, 173), botB = RGB(70, 30, 150);
    uint16_t top = lerpColor(topA, topB, s);
    uint16_t bot = lerpColor(botA, botB, s);
    for (int y = 0; y < FB_H; y++) {
        uint16_t c = lerpColor(top, bot, (float)y / FB_H);
        for (int x = 0; x < FB_W; x++) fb[y * FB_W + x] = c;
    }
}

// The same gradient, but only inside one rectangle. Used for partial
// redraws (the animation drifts so slowly the seam is invisible).
void drawBackgroundRect(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_W) w = FB_W - x;
    if (y + h > FB_H) h = FB_H - y;
    if (w <= 0 || h <= 0) return;
    float s = sinf(millis() * 0.00035f) * 0.5f + 0.5f;
    uint16_t topA = RGB(255, 138, 205), botA = RGB(96, 42, 176);
    uint16_t topB = RGB(255, 106, 173), botB = RGB(70, 30, 150);
    uint16_t top = lerpColor(topA, topB, s);
    uint16_t bot = lerpColor(botA, botB, s);
    for (int yy = y; yy < y + h; yy++) {
        uint16_t c = lerpColor(top, bot, (float)yy / FB_H);
        for (int xx = x; xx < x + w; xx++) fb[yy * FB_W + xx] = c;
    }
}

void drawRect(int x, int y, int w, int h, uint16_t c) {
    fillRect(x, y, w, 1, c);
    fillRect(x, y + h - 1, w, 1, c);
    fillRect(x, y, 1, h, c);
    fillRect(x + w - 1, y, 1, h, c);
}

// 3DS-style glass panel: fill + slightly brighter top edge.
void glassPanel(int x, int y, int w, int h, bool hi) {
    fillRect(x, y, w, h, hi ? C_GLASS_HI : C_GLASS);
    fillRect(x, y, w, 2, C_GLASS_HI);
}

// Answer tile: colour + darker bottom edge (3DS look).
void tile(int x, int y, int w, int h, uint16_t c) {
    fillRect(x, y, w, h, c);
    fillRect(x, y + h - 6, w, 6, lerpColor(c, C_INK, 0.35f));
}

void drawChar(int x, int y, char c, uint16_t color, int scale) {
    if (c < 32 || c > 127) c = ' ';
    const char *glyph = font8x8_basic[(int)c];
    for (int gy = 0; gy < 8; gy++) {
        for (int gx = 0; gx < 8; gx++) {
            if (glyph[gy] & (1 << gx)) {
                int px = x + gx * scale;
                int py = y + gy * scale;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++) {
                        if (px + sx >= 0 && px + sx < FB_W && py + sy >= 0 && py + sy < FB_H)
                            fb[(py + sy) * FB_W + (px + sx)] = color;
                    }
            }
        }
    }
}

void drawString(int x, int y, const char *s, uint16_t color, int scale) {
    int cx = x;
    while (*s) {
        if (cx + 8 * scale > FB_W) break;
        drawChar(cx, y, *s, color, scale);
        cx += 8 * scale;
        s++;
    }
}

int textW(const char *s, int scale) { return strlen(s) * 8 * scale; }

void drawCentered(int y, const char *s, uint16_t color, int scale) {
    drawString((FB_W - textW(s, scale)) / 2, y, s, color, scale);
}

// Shadowed centered text like the 3DS.
void drawCenteredShadow(int y, const char *s, uint16_t color, int scale) {
    drawCentered(y + scale, s, C_INK, scale);
    drawCentered(y, s, color, scale);
}

// Centres text around a specific x (for labels inside buttons/keys).
void drawCenteredAt(int cx, int y, const char *s, uint16_t color, int scale) {
    drawString(cx - textW(s, scale) / 2, y, s, color, scale);
}

// Wraps text into lines (simple word wrap). Returns line count.
int wrapText(const char *s, int maxw, int scale, String *lines, int maxLines) {
    int n = 0;
    String cur = "";
    String word = "";
    for (const char *p = s; ; p++) {
        if (*p && *p != ' ' && *p != '\n') { word += *p; continue; }
        if (cur.length() && textW((cur + " " + word).c_str(), scale) > maxw) {
            if (n < maxLines) lines[n++] = cur;
            cur = word;
        } else {
            cur = cur.length() ? cur + " " + word : word;
        }
        word = "";
        if (*p == '\n') {
            if (n < maxLines) lines[n++] = cur;
            cur = "";
        }
        if (!*p) break;
    }
    if (cur.length() && n < maxLines) lines[n++] = cur;
    return n;
}

// Single-buffered rendering: fb IS the panel's framebuffer (the RGB
// controller scans it continuously), so drawing is immediately visible.
// blit() just flushes the CPU cache so the LCD DMA sees the writes.
void blit(void) {
    Cache_WriteBack_Addr((uint32_t)fb, FB_W * FB_H * 2);
}

// Flush only one rectangle to the LCD DMA instead of the whole framebuffer.
// Cache lines are 64 bytes: align the x down and the width up so we cover
// the rect (overlapping flushes are harmless).
void blitRect(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_W) w = FB_W - x;
    if (y + h > FB_H) h = FB_H - y;
    if (w <= 0 || h <= 0) return;
    int ax = x & ~0x3F;
    int aw = w + (x - ax);
    for (int yy = y; yy < y + h; yy++)
        Cache_WriteBack_Addr((uint32_t)(fb + (uint32_t)yy * FB_W + ax), aw * 2);
}

// Back button (top-left, 3DS-style tile).
bool backHit(int x, int y, int w, int h) {
    return x >= 12 && x < 12 + w && y >= 12 && y < 12 + h;
}
void drawBack(void) {
    tile(12, 12, 120, 44, C_INK);
    drawCenteredAt(12 + 60, 24, "Back", C_TEXT, 2);
}

// ------------------------------------------------------------------ touch
touch_gt911_point_t tp;
bool tpActive = false;
int tpX = 0, tpY = 0;
unsigned long tpDownMs = 0;
bool tapConsumed = false;

void touch_step(void) {
    tp = touch_gt911_read_point(1);
    bool active = tp.cnt > 0;
    if (active && !tpActive) {
        tpActive = true;
        tpDownMs = millis();
        tapConsumed = false;
    }
    if (active) { tpX = tp.x[0]; tpY = tp.y[0]; }
    if (!active && tpActive) tpActive = false;
}

bool tapIn(int x, int y, int w, int h) {
    if (tapConsumed) return false;
    if (!tpActive && tpX >= x && tpX < x + w && tpY >= y && tpY < y + h) {
        unsigned long held = millis() - tpDownMs;
        tapConsumed = true;
        return held < 800;
    }
    return false;
}

void touch_wait_release(void) { while (touch_gt911_read_point(1).cnt > 0) delay(5); }

// ------------------------------------------------------------------ .565 decode
static const uint8_t PERM[64] = {
    0, 1, 8, 9, 16, 17, 10, 2, 3, 11, 18, 19, 24, 25, 20, 12,
    4, 5, 14, 13, 22, 23, 28, 21, 6, 7, 15, 26, 27, 30, 31, 29,
    32, 33, 40, 41, 48, 49, 42, 34, 35, 43, 50, 51, 56, 57, 52, 44,
    36, 37, 46, 45, 54, 55, 60, 53, 38, 39, 47, 58, 59, 62, 63, 61
};
static int pow2up(int v) { int p = 8; while (p < v) p <<= 1; return p; }

bool decode565(const uint8_t *data, size_t len, uint16_t **spr, int *w, int *h) {
    if (len < 6) return false;
    int iw = data[0] | (data[1] << 8);
    int ih = data[2] | (data[3] << 8);
    if (iw < 1 || ih < 1 || iw > 1024 || ih > 1024) return false;
    int pw = pow2up(iw), ph = pow2up(ih);
    size_t fbSize = (size_t)pw * ph * 4;
    if (len == 4 + fbSize) {
    } else if (len >= 6) {
        int n = data[4];
        if (n >= 2 && len == 6 + (size_t)n * fbSize) return false;
        return false;
    } else return false;
    uint16_t *img = (uint16_t *)heap_caps_malloc((size_t)iw * ih * 2, MALLOC_CAP_SPIRAM);
    if (!img) return false;
    for (int by = 0; by < (ph >> 3); by++)
        for (int bx = 0; bx < (pw >> 3); bx++) {
            size_t base = ((size_t)by * (pw >> 3) + bx) * 64;
            for (int j = 0; j < 64; j++) {
                int s = PERM[j];
                int sx = bx * 8 + (s % 8);
                int sy = by * 8 + (s / 8);
                size_t off = (base + j) * 4;
                uint8_t a = data[4 + off], b = data[4 + off + 1], g = data[4 + off + 2], r = data[4 + off + 3];
                if (sx < iw && sy < ih) {
                    img[sy * iw + sx] = RGB(r, g, b);
                }
            }
        }
    *spr = img; *w = iw; *h = ih;
    return true;
}

void drawSprite(int x, int y, const uint16_t *spr, int w, int h) {
    for (int yy = 0; yy < h; yy++) {
        if (y + yy < 0 || y + yy >= FB_H) continue;
        for (int xx = 0; xx < w; xx++) {
            if (x + xx < 0 || x + xx >= FB_W) continue;
            fb[(y + yy) * FB_W + (x + xx)] = spr[yy * w + xx];
        }
    }
}

// Sprite scaled to exactly dw x dh (nearest neighbour). Sprites must fit
// their box: drawing at full size bleeds over neighbouring rows.
void drawSpriteScaled(int x, int y, const uint16_t *spr, int w, int h, int dw, int dh) {
    for (int yy = 0; yy < dh; yy++) {
        if (y + yy < 0 || y + yy >= FB_H) continue;
        int sy = (int)((long)yy * h / dh);
        for (int xx = 0; xx < dw; xx++) {
            if (x + xx < 0 || x + xx >= FB_W) continue;
            int sx = (int)((long)xx * w / dw);
            fb[(y + yy) * FB_W + (x + xx)] = spr[sy * w + sx];
        }
    }
}

// ------------------------------------------------------------------ HTTP
struct HttpResp { int status; String body; };

HttpResp http_req(const char *method, const String &path, const String &formData = "") {
    HttpResp r = { -1, "" };
    WiFiClient c;
    if (!c.connect(SERVER_HOST.c_str(), SERVER_PORT)) return r;
    c.setTimeout(10000);
    c.print(method);
    c.print(' ');
    c.print(path);
    c.print(" HTTP/1.1\r\nHost: ");
    c.print(SERVER_HOST);
    c.print("\r\nUser-Agent: QuizFabric3DS-ESP32\r\nConnection: close\r\n");
    if (formData.length()) {
        c.print("Content-Type: application/x-www-form-urlencoded\r\n");
        c.print("Content-Length: ");
        c.print(formData.length());
        c.print("\r\n\r\n");
        c.print(formData);
    } else {
        c.print("\r\n");
    }
    // headers: read a raw byte stream until the full header terminator
    // shows up. NEVER use c.connected() to end this loop - on lwIP the
    // peer's FIN can arrive while response bytes are still buffered, and
    // through some NATs the FIN precedes the last header burst. That
    // race made the body start with leftover headers ("Content-Length:
    // 586 Date: ...") and broke the quiz list over the domain path.
    String head;
    unsigned long deadline = millis() + 10000;
    int sep = -1;
    while (millis() < deadline && sep < 0) {
        if (c.available()) {
            head += (char)c.read();
            sep = head.indexOf("\r\n\r\n");
            if (sep < 0) sep = head.indexOf("\n\n");
        } else {
            delay(1);
        }
    }
    int contentLen = -1;
    bool chunked = false;
    if (sep >= 0) {
        int ls = 0;
        while (ls < sep) {
            int nl = head.indexOf('\n', ls);
            if (nl < 0 || nl > sep) nl = sep;
            String l = head.substring(ls, nl);
            l.trim();
            if (l.startsWith("HTTP/")) r.status = l.substring(9, 12).toInt();
            if (l.startsWith("Content-Length:")) contentLen = l.substring(15).toInt();
            if (l.startsWith("Transfer-Encoding:") && l.indexOf("chunked") >= 0) chunked = true;
            ls = nl + 1;
        }
        int bodyStart = sep + 4;
        if (head.substring(sep, sep + 2) == "\n\n") bodyStart = sep + 2;
        r.body = head.substring(bodyStart);   // anything that already arrived
    }
    // Reserve the body up front: String += without reserve is O(n^2)
    // and freezes the ESP32 for seconds on large quiz JSON.
    if (contentLen > 0) r.body.reserve(contentLen + 1);
    // drain the rest of the body
    deadline = millis() + 15000;
    if (chunked) {
        while (millis() < deadline) {
            if (c.available()) {
                int n = 0;
                while (c.available()) {
                    r.body += (char)c.read();
                    if ((++n & 2047) == 0) delay(1);   // pace WiFi bursts
                }
            } else if (!c.connected()) break;
            else delay(1);
        }
        // decode the chunk framing from the raw body
        String decoded;
        decoded.reserve(r.body.length());
        int pos = 0;
        while (pos + 1 < (int)r.body.length()) {
            int nl = r.body.indexOf('\n', pos);
            if (nl < 0) break;
            long n = strtol(r.body.substring(pos, nl).c_str(), nullptr, 16);
            pos = nl + 1;
            if (n <= 0) break;   // 0-size chunk = end
            if (pos + n > (int)r.body.length()) n = r.body.length() - pos;
            decoded += r.body.substring(pos, pos + n);
            pos += n;
            if (pos < (int)r.body.length() && r.body[pos] == '\r') pos++;
            if (pos < (int)r.body.length() && r.body[pos] == '\n') pos++;
        }
        r.body = decoded;
    } else {
        while (millis() < deadline) {
            if (c.available()) {
                int n = 0;
                while (c.available()) {
                    r.body += (char)c.read();
                    if ((++n & 2047) == 0) delay(1);   // pace WiFi bursts
                }
            } else if (contentLen >= 0 && (int)r.body.length() >= contentLen) {
                break;   // got everything the server promised
            } else if (!c.connected()) {
                break;
            } else {
                delay(1);
            }
        }
    }
    c.stop();
    return r;
}

String urlEncode(const char *s) {
    String out;
    for (; *s; s++) {
        char ch = *s;
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') out += ch;
        else {
            char b[4];
            snprintf(b, 4, "%%%02X", (uint8_t)ch);
            out += b;
        }
    }
    return out;
}

bool fetchImage565(const String &path, uint16_t **spr, int *w, int *h, bool *isGif) {
    HttpResp r = http_req("GET", path);
    *isGif = false;
    if (r.status != 200 || r.body.length() < 6) return false;
    const uint8_t *d = (const uint8_t *)r.body.c_str();
    if (d[4] >= 2) { *isGif = true; return false; }
    return decode565(d, r.body.length(), spr, w, h);
}

// ------------------------------------------------------------------ keyboard
String keyboard(const char *hint, const char *initial) {
    // Numbers on top; shift turns them into symbols (1=! 2=@ 3=# 4=$ 5=%
    // 6=. 7=: 8=* 9=( 0=)) and letters into uppercase.
    static const char *rows[4] = { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" };
    static const char *syms = "!@#$%.:*()";
    String out = initial ? initial : "";
    bool shift = false;
    int baseY = 132;
    int cy = baseY + 4 * 72 + 12;
    String lastOut = "\x01";   // force first paint
    bool lastShift = !shift;
    bool first = true;
    while (true) {
        // Partial refresh: pressing a letter only redraws the text-entry
        // panel; pressing shift only redraws the keys themselves.
        bool textCh = out != lastOut;
        bool shiftCh = shift != lastShift;
        if (first || textCh || shiftCh) {
            if (first) { drawBackground(); blit(); }
            if (first || textCh) {
                lastOut = out;
                glassPanel(20, 16, FB_W - 40, 96, false);
                drawCentered(30, hint, C_FAINT, 1);
                drawCentered(62, out.c_str(), C_TEXT, 2);
                blitRect(20, 16, FB_W - 40, 96);
            }
            if (first || shiftCh) {
                lastShift = shift;
                for (int r = 0; r < 4; r++) {
                    const char *row = rows[r];
                    int n = strlen(row);
                    int kw = (FB_W - 60) / n;
                    for (int i = 0; i < n; i++) {
                        int x = 30 + i * kw;
                        char ch = row[i];
                        if (r == 0) { if (shift) ch = syms[i]; }
                        else if (shift && ch >= 'a' && ch <= 'z') ch = toupper(ch);
                        tile(x, baseY + r * 72, kw - 8, 64, r == 0 ? C_GLASS : ansColor(r));
                        char b[2] = { ch, 0 };
                        drawCenteredAt(x + (kw - 8) / 2, baseY + r * 72 + 20, b, C_TEXT, 2);
                    }
                }
                tile(30, cy, 230, 64, C_GLASS);       drawCenteredAt(30 + 115, cy + 20, "SPACE", C_TEXT, 2);
                tile(280, cy, 200, 64, C_GLASS_HI);    drawCenteredAt(280 + 100, cy + 20, shift ? "ABC" : "SHIFT", C_INK, 2);
                tile(500, cy, 200, 64, C_BAD);         drawCenteredAt(500 + 100, cy + 20, "DEL", C_TEXT, 2);
                tile(720, cy, 274, 64, C_GOOD);        drawCenteredAt(720 + 137, cy + 20, "DONE", C_INK, 2);
                blitRect(30, baseY, FB_W - 60, cy + 64 - baseY);
            }
            first = false;
        }

        touch_step();
        if (tapIn(30, cy, 230, 64)) out += ' ';
        if (tapIn(280, cy, 200, 64)) shift = !shift;
        if (tapIn(500, cy, 200, 64)) { if (out.length()) out.remove(out.length() - 1); }
        if (tapIn(720, cy, 274, 64)) { touch_wait_release(); return out; }
        for (int r = 0; r < 4; r++) {
            const char *row = rows[r];
            int n = strlen(row);
            int kw = (FB_W - 60) / n;
            for (int i = 0; i < n; i++) {
                int x = 30 + i * kw;
                if (tapIn(x, baseY + r * 72, kw - 8, 64)) {
                    char ch = row[i];
                    if (r == 0) { if (shift) ch = syms[i]; }
                    else if (shift && ch >= 'a' && ch <= 'z') ch = toupper(ch);
                    if (out.length() < 120) out += ch;
                }
            }
        }
        delay(10);
    }
}

// ------------------------------------------------------------------ screens
enum Scr { SCR_MENU, SCR_LIST, SCR_PLAY, SCR_STEAL, SCR_CONFIG };
Scr screen = SCR_MENU;
String statusLine = "";

// quiz list
#define MAX_QUIZZES 400
String quizNames[MAX_QUIZZES];
bool quizHasBanner[MAX_QUIZZES];
int nQuizzes = 0;
int listScroll = 0;
int listSel = -1;
#define LIST_ROWS 8
uint16_t *thumbSpr[LIST_ROWS] = { nullptr };
int thumbW[LIST_ROWS], thumbH[LIST_ROWS];
int thumbFor[LIST_ROWS];

void freeThumbs(void) {
    for (int i = 0; i < LIST_ROWS; i++) {
        if (thumbSpr[i]) { free(thumbSpr[i]); thumbSpr[i] = nullptr; }
        thumbFor[i] = -1;
    }
}

// Load one pending banner thumbnail, one per idle frame. Returns the slot
// that got a new thumbnail (caller redraws that row), or -1. Never call
// this while a finger is down: the fetch is blocking and would make taps
// freeze.
int pumpThumb(void) {
    for (int i = 0; i < LIST_ROWS; i++) {
        int qi = listScroll + i;
        if (qi < 0 || qi >= nQuizzes) continue;
        if (!quizHasBanner[qi] || thumbSpr[i] || thumbFor[i] == qi) continue;
        thumbFor[i] = qi;
        bool gif = false;
        uint16_t *s = nullptr; int w = 0, h = 0;
        String enc = urlEncode(quizNames[qi].c_str());
        if (fetchImage565("/api/quizbanner/" + enc, &s, &w, &h, &gif) && s) {
            thumbSpr[i] = s; thumbW[i] = w; thumbH[i] = h;
            return i;
        }
        return -1;
    }
    return -1;
}

void fetchQuizList(void) {
    nQuizzes = 0;
    freeThumbs();
    HttpResp r = http_req("GET", "/api/quizzes");
    if (r.status != 200) { statusLine = "Could not load the quiz list (" + String(r.status) + ")"; return; }
    JsonDocument doc;
    if (deserializeJson(doc, r.body)) {
        // Show what actually came back so a non-JSON answer (router admin
        // page, challenge page, ...) is visible on screen.
        statusLine = "Bad list response: " + r.body.substring(0, 60);
        return;
    }
    JsonArray arr = doc["quizzes"];
    for (JsonObject q : arr) {
        if (nQuizzes >= MAX_QUIZZES) break;
        quizNames[nQuizzes] = q["name"].as<String>();
        quizHasBanner[nQuizzes] = q["banner"].as<bool>();
        nQuizzes++;
    }
    statusLine = nQuizzes ? "" : "Server has no quizzes yet";
}

// ------------------------------------------------------------------ menu
void drawMenu(void) {
    drawBackground();
    drawCenteredShadow(70, "QuizFabric", C_GOLD, 4);
    drawCentered(140, String("Server: " + SERVER_HOST + ":" + String(SERVER_PORT)).c_str(), C_FAINT, 1);
    tile(80, 220, FB_W - 160, 90, ansColor(0));
    drawCenteredShadow(252, "Play Solo", C_TEXT, 3);
    tile(80, 340, FB_W - 160, 90, ansColor(2));
    drawCenteredShadow(372, "Steal from Kahoot", C_TEXT, 3);
    glassPanel(80, 460, FB_W - 160, 90, true);
    drawCentered(496, "Server settings", C_TEXT, 2);
    if (statusLine.length()) drawCentered(FB_H - 30, statusLine.c_str(), C_BAD, 1);
    blit();
}

void menuLoop(void) {
    drawMenu();
    while (screen == SCR_MENU) {
        touch_step();
        if (tapIn(80, 220, FB_W - 160, 90)) { screen = SCR_LIST; fetchQuizList(); return; }
        if (tapIn(80, 340, FB_W - 160, 90)) { screen = SCR_STEAL; return; }
        if (tapIn(80, 460, FB_W - 160, 90)) { screen = SCR_CONFIG; return; }
        delay(10);
    }
}

// ------------------------------------------------------------------ list
void drawListRow(int i) {
    int qi = listScroll + i;
    int rowY = 84 + i * 62;
    if (qi >= nQuizzes) return;
    glassPanel(16, rowY, FB_W - 32, 56, (i % 2) == 0);
    if (thumbSpr[i]) {
        // Fit the banner inside the 48x48 box, centred, so it can never
        // bleed over the neighbouring rows.
        int m = thumbW[i] > thumbH[i] ? thumbW[i] : thumbH[i];
        float sc = 48.0f / m;
        int dw = thumbW[i] * sc, dh = thumbH[i] * sc;
        if (dw < 1) dw = 1; if (dh < 1) dh = 1;
        drawSpriteScaled(22 + (48 - dw) / 2, rowY + 4 + (48 - dh) / 2,
                         thumbSpr[i], thumbW[i], thumbH[i], dw, dh);
    } else {
        fillRect(22, rowY + 4, 48, 48, C_INK);
        drawCenteredAt(22 + 24, rowY + 18, "?", C_TEXT, 2);
    }
    drawString(90, rowY + 16, quizNames[qi].c_str(), C_TEXT, 2);
}

// Just the rows area (background under them + rows), no header/back.
// Callers blitRect the same region.
void drawListRows(void) {
    drawBackgroundRect(0, 84, FB_W, LIST_ROWS * 62);
    for (int i = 0; i < LIST_ROWS; i++) drawListRow(i);
}

void drawList(void) {
    drawBackground();
    drawCenteredShadow(14, "Choose a quiz", C_TEXT, 2);
    drawCentered(50, "Drag to scroll, tap to play", C_FAINT, 1);
    drawListRows();
    drawBack();
    blit();
}

void listLoop(void) {
    drawList();
    bool dragging = false;
    int dragStartY = 0;
    int lastScroll = listScroll;
    unsigned long lastPaintMs = 0;
    bool thumbDirty = false;
    while (screen == SCR_LIST) {
        // Load banners one per idle frame (never during a touch: the HTTP
        // fetch is blocking and was freezing taps). Repaint the rows only
        // once all visible banners are in, so the list doesn't strobe
        // row-by-row while the LCD scans the framebuffer.
        if (!tpActive) {
            if (pumpThumb() >= 0) thumbDirty = true;
            if (thumbDirty) {
                bool pending = false;
                for (int i = 0; i < LIST_ROWS; i++) {
                    int qi = listScroll + i;
                    if (qi >= 0 && qi < nQuizzes && quizHasBanner[qi] && !thumbSpr[i] && thumbFor[i] != qi)
                        { pending = true; break; }
                }
                if (!pending) {
                    drawListRows();
                    blitRect(0, 84, FB_W, LIST_ROWS * 62);
                    thumbDirty = false;
                }
            }
        }

        touch_step();
        if (backHit(12, 12, 120, 44) && tapIn(12, 12, 120, 44)) { screen = SCR_MENU; return; }
        for (int i = 0; i < LIST_ROWS; i++) {
            int qi = listScroll + i;
            if (qi >= nQuizzes) break;
            int rowY = 84 + i * 62;
            if (tapIn(16, rowY, FB_W - 32, 56)) {
                listSel = qi;
                screen = SCR_PLAY;
                return;
            }
        }
        if (tpActive) {
            if (!dragging) { dragging = true; dragStartY = tpY; }
            int dy = tpY - dragStartY;
            if (dy < -25) { listScroll += 3; dragStartY = tpY; }
            if (dy > 25) { listScroll -= 3; dragStartY = tpY; }
            if (listScroll < 0) listScroll = 0;
            if (listScroll > nQuizzes - LIST_ROWS) listScroll = nQuizzes - LIST_ROWS;
            if (listScroll < 0) listScroll = 0;
            // Redraw the rows only when the scroll moved AND at most every
            // 50ms - fast repaints while dragging tear the framebuffer.
            if (listScroll != lastScroll && millis() - lastPaintMs >= 50) {
                lastScroll = listScroll;
                lastPaintMs = millis();
                drawListRows();
                blitRect(0, 84, FB_W, LIST_ROWS * 62);
                thumbDirty = false;
            }
        } else if (dragging) {
            dragging = false;
            drawList();
        }
        delay(10);
    }
}

// ------------------------------------------------------------------ play
#define MAX_Q 300
#define MAX_A 8
struct QuizQ {
    String question;
    String answers[MAX_A];
    int nAnswers;
    int correct[MAX_A];
    int qtype;   // 0 mc, 1 type, 2 slider, 3 puzzle
    String typeAnswer;
    float sMin, sMax, sAnswer, sTol;
    String imgUrl;
};
QuizQ *qs = nullptr;
int nQ = 0;
int qi = 0;
int selAns = -1;
String typed;
float sVal = 0;
int pOrder[MAX_A];
bool revealing = false;
int score = 0;
int lastGain = 0;
bool lastRight = false;
unsigned long qStart = 0;
uint16_t *qImgSpr = nullptr;
int qImgW = 0, qImgH = 0;
bool qImgGif = false;
int playControlsY = 150;   // y where the answer controls begin
bool playNeedsFull = true; // next loop iteration must do a full drawPlay()
bool qImgPending = false;  // question image loads lazily in the play loop

void freeQuestionImage(void) {
    if (qImgSpr) { free(qImgSpr); qImgSpr = nullptr; }
}

void loadQuestionImage(const String &url) {
    freeQuestionImage();
    qImgGif = false;
    fetchImage565(url + ".565", &qImgSpr, &qImgW, &qImgH, &qImgGif);
}

bool fetchQuizQuestions(void) {
    String enc = urlEncode(quizNames[listSel].c_str());
    HttpResp r = http_req("GET", "/api/quiz/" + enc);
    if (r.status != 200) return false;
    JsonDocument doc;
    if (deserializeJson(doc, r.body)) return false;
    JsonArray arr = doc.as<JsonArray>();
    nQ = arr.size();
    if (nQ > MAX_Q) nQ = MAX_Q;
    if (nQ <= 0) return false;   // empty/invalid quiz - don't crash on it
    // new[] runs the String constructors - malloc left them uninitialised
    // and the first assignment corrupted the heap (crash + reboot).
    if (qs) delete[] qs;
    qs = new QuizQ[nQ];
    for (int i = 0; i < nQ; i++) {
        JsonObject q = arr[i];
        QuizQ *qq = &qs[i];
        qq->question = q["question"].as<String>();
        qq->nAnswers = 0;
        for (JsonVariant a : q["answers"].as<JsonArray>()) {
            if (qq->nAnswers >= MAX_A) break;
            qq->answers[qq->nAnswers++] = a.as<String>();
        }
        for (int k = 0; k < MAX_A; k++) qq->correct[k] = 0;
        for (JsonVariant ci : q["correct_indices"].as<JsonArray>()) {
            int idx = ci.as<int>();
            if (idx >= 0 && idx < qq->nAnswers) qq->correct[idx] = 1;
        }
        const char *t = q["type"] | "type";
        if (!strcmp(t, "mc")) qq->qtype = 0;
        else if (!strcmp(t, "slider")) qq->qtype = 2;
        else if (!strcmp(t, "puzzle")) qq->qtype = 3;
        else qq->qtype = 1;
        if (qq->nAnswers <= 0) qq->qtype = 1;   // never divide by an empty answer list
        qq->typeAnswer = q["answer"].as<String>();
        qq->sMin = q["min"] | 0.0f;
        qq->sMax = q["max"] | 100.0f;
        qq->sAnswer = q["answer"] | 0.0f;
        qq->sTol = q["tolerance"] | 0.0f;
        qq->imgUrl = "";
        JsonArray imgs = q["images"].as<JsonArray>();
        if (imgs.size() > 0) qq->imgUrl = imgs[0]["url"].as<String>();
    }
    return true;
}

void startQuestion(void) {
    selAns = -1;
    typed = "";
    revealing = false;
    qStart = millis();
    freeQuestionImage();
    QuizQ *q = &qs[qi];
    if (q->qtype == 2) sVal = (q->sMin + q->sMax) / 2.0f;
    if (q->qtype == 3) {
        for (int i = 0; i < q->nAnswers; i++) pOrder[i] = i;
        for (int i = q->nAnswers - 1; i > 0; i--) {
            int j = random(i + 1);
            int t = pOrder[i]; pOrder[i] = pOrder[j]; pOrder[j] = t;
        }
    }
    // Load the question image lazily inside the play loop (after the
    // question itself is on screen) so a big image fetch can't block
    // quiz entry or starve the LCD bus all at once.
    qImgPending = q->imgUrl.length() > 0;
    playNeedsFull = true;
}

void submit(void) {
    QuizQ *q = &qs[qi];
    float secs = (millis() - qStart) / 1000.0f;
    lastGain = (int)(1000.0f - secs * 50.0f);
    if (lastGain < 0) lastGain = 0;
    lastRight = false;
    if (q->qtype == 0) {
        if (selAns >= 0 && selAns < q->nAnswers) lastRight = q->correct[selAns] != 0;
    } else if (q->qtype == 2) {
        float d = sVal - q->sAnswer;
        if (d < 0) d = -d;
        lastRight = d <= q->sTol;
    } else if (q->qtype == 3) {
        lastRight = true;
        for (int i = 0; i < q->nAnswers; i++)
            if (pOrder[i] != i) { lastRight = false; break; }
    } else {
        lastRight = typed.equalsIgnoreCase(q->typeAnswer);
    }
    if (lastRight) score += lastGain;
    revealing = true;
    playNeedsFull = true;
}

// Top half of the play screen: background, header, question, image.
// Stores where the controls begin in playControlsY.
void drawPlayTop(void) {
    drawBackground();
    QuizQ *q = &qs[qi];
    char hdr[64];
    snprintf(hdr, 64, "Q%d/%d      Score: %d", qi + 1, nQ, score);
    drawCentered(14, hdr, C_FAINT, 1);
    drawBack();

    // question (top-screen content, fitted)
    int y = 66;
    String lines[4];
    int nl = wrapText(q->question.c_str(), FB_W - 48, 2, lines, 4);
    for (int i = 0; i < nl; i++) {
        drawString(24, y, lines[i].c_str(), C_TEXT, 2);
        y += 22;
    }
    y += 4;

    if (qImgGif) {
        drawCentered(y + 14, "GIF skipped", C_FAINT, 2);
        y += 44;
    } else if (qImgSpr) {
        float sc = 1.0f;
        int maxw = FB_W - 48, maxh = 200;
        if (qImgW > maxw) sc = (float)maxw / qImgW;
        if (qImgH * sc > maxh) sc = (float)maxh / qImgH;
        int dw = qImgW * sc, dh = qImgH * sc;
        if (dw < 1) dw = 1; if (dh < 1) dh = 1;
        drawSpriteScaled((FB_W - dw) / 2, y, qImgSpr, qImgW, qImgH, dw, dh);
        y += dh + 8;
    }
    playControlsY = y;
}

// Correct/Nope box + Next button.
void drawPlayReveal(void) {
    QuizQ *q = &qs[qi];
    int y = playControlsY;
    tile(24, y, FB_W - 48, 70, lastRight ? C_GOOD : C_BAD);
    char txt[80];
    snprintf(txt, 80, lastRight ? "Correct! +%d" : "Nope", lastGain);
    drawCenteredShadow(y + 16, txt, C_INK, 2);
    String correct;
    if (q->qtype == 0) {
        for (int k = 0; k < q->nAnswers; k++)
            if (q->correct[k]) { correct = q->answers[k]; break; }
    } else if (q->qtype == 1) correct = q->typeAnswer;
    else if (q->qtype == 2) correct = String(q->sAnswer);
    else correct = "the listed order";
    drawCentered(y + 44, String("Answer: " + correct).c_str(), C_INK, 1);
    tile(24, FB_H - 76, FB_W - 48, 60, C_GOLD);
    drawCenteredShadow(FB_H - 50, qi + 1 < nQ ? "Next" : "Finish", C_INK, 2);
}

// Geometry for one mc answer tile (shared by drawing and tap handling).
void mcTileRect(int i, int *ty, int *th) {
    QuizQ *q = &qs[qi];
    int avail = FB_H - 90 - playControlsY;
    int bh = avail / q->nAnswers;
    if (bh > 68) bh = 68;
    int totalH = q->nAnswers * bh + (q->nAnswers - 1) * 8;
    int ay = playControlsY + (avail - totalH) / 2;
    if (ay < playControlsY) ay = playControlsY;
    *ty = ay + i * (bh + 8);
    *th = bh;
}

// Draw one mc answer tile, no blit (caller flushes).
void drawMcTileOnly(int i) {
    QuizQ *q = &qs[qi];
    int ty, th;
    mcTileRect(i, &ty, &th);
    tile(24, ty, FB_W - 48, th, selAns == i ? C_GOLD : ansColor(i));
    drawString(40, ty + th / 2 - 8, q->answers[i].c_str(), selAns == i ? C_INK : C_TEXT, 2);
}

void puzzleRowRect(int i, int *ry, int *rh) {
    QuizQ *q = &qs[qi];
    int h = (FB_H - 90 - playControlsY) / q->nAnswers;
    if (h > 44) h = 44;
    *ry = playControlsY + i * (h + 6);
    *rh = h;
}

// Draw one puzzle row (tile + arrow), no blit.
void drawPuzzleRowOnly(int i) {
    QuizQ *q = &qs[qi];
    int ry, rh;
    puzzleRowRect(i, &ry, &rh);
    tile(24, ry, FB_W - 110, rh, ansColor(i));
    drawString(40, ry + rh / 2 - 8, q->answers[pOrder[i]].c_str(), C_TEXT, 2);
    glassPanel(FB_W - 76, ry, 52, rh, true);
    drawCenteredAt(FB_W - 50, ry + rh / 2 - 8, "^", C_TEXT, 2);
}

// Draw the slider value text + track, no blit.
void drawSliderOnly(void) {
    QuizQ *q = &qs[qi];
    int y = playControlsY;
    char txt[64];
    snprintf(txt, 64, "%.1f    (%.1f - %.1f)", sVal, q->sMin, q->sMax);
    drawBackgroundRect(24, y + 4, FB_W - 48, 26);
    drawCentered(y + 8, txt, C_TEXT, 2);
    glassPanel(60, y + 50, FB_W - 120, 46, false);
    float t = (sVal - q->sMin) / (q->sMax - q->sMin);
    fillRect(60, y + 50, (FB_W - 120) * t, 46, C_GOLD);
}

// Answer controls only (everything from playControlsY down). Callers
// blitRect the same region.
void drawPlayControls(void) {
    QuizQ *q = &qs[qi];
    int y = playControlsY;
    if (q->qtype == 0) {
        for (int i = 0; i < q->nAnswers; i++) drawMcTileOnly(i);
        tile(24, FB_H - 76, FB_W - 48, 60, C_GOOD);
        drawCenteredShadow(FB_H - 50, "Submit", C_INK, 2);
    } else if (q->qtype == 1) {
        glassPanel(24, y, FB_W - 48, 70, true);
        drawCentered(y + 16, String("Your answer: " + typed).c_str(), C_TEXT, 2);
        tile(24, y + 90, FB_W - 48, 60, ansColor(3));
        drawCenteredShadow(y + 106, "Type answer", C_INK, 2);
        tile(24, y + 162, FB_W - 48, 60, C_GOOD);
        drawCenteredShadow(y + 178, "Submit", C_INK, 2);
    } else if (q->qtype == 2) {
        drawSliderOnly();
        tile(24, FB_H - 76, FB_W - 48, 60, C_GOOD);
        drawCenteredShadow(FB_H - 50, "Submit", C_INK, 2);
    } else if (q->qtype == 3) {
        for (int i = 0; i < q->nAnswers; i++) drawPuzzleRowOnly(i);
        tile(24, FB_H - 76, FB_W - 48, 60, C_GOOD);
        drawCenteredShadow(FB_H - 50, "Submit", C_INK, 2);
    }
}

void drawPlay(void) {
    drawPlayTop();
    if (revealing) drawPlayReveal();
    else drawPlayControls();
    blit();
}

void playLoop(void) {
    drawBackground();
    drawCentered(FB_H / 2 - 20, "Loading quiz...", C_TEXT, 2);
    blit();
    if (!fetchQuizQuestions()) {
        statusLine = "Could not load that quiz";
        screen = SCR_MENU;
        return;
    }
    qi = 0;
    score = 0;
    startQuestion();   // sets playNeedsFull
    while (screen == SCR_PLAY) {
        touch_step();
        QuizQ *q = &qs[qi];
        if (backHit(12, 12, 120, 44) && tapIn(12, 12, 120, 44) && !revealing) {
            freeQuestionImage();
            screen = SCR_MENU;
            return;
        }
        // Full redraw only when the question (or reveal state) changed.
        if (playNeedsFull) { drawPlay(); playNeedsFull = false; }
        // Lazy question image: fetch it once the question is on screen
        // and no finger is down, then redraw with the image included.
        if (qImgPending && !tpActive) {
            qImgPending = false;
            if (q->imgUrl.length()) loadQuestionImage(q->imgUrl);
            playNeedsFull = true;
        }
        if (revealing) {
            if (tapIn(24, FB_H - 76, FB_W - 48, 60)) {
                if (qi + 1 < nQ) { qi++; startQuestion(); }
                else {
                    freeQuestionImage();
                    statusLine = "Quiz complete! Score: " + String(score);
                    screen = SCR_MENU;
                    return;
                }
            }
            delay(10);
            continue;
        }
        int y = playControlsY;
        if (q->qtype == 0) {
            for (int i = 0; i < q->nAnswers; i++) {
                int ty, th;
                mcTileRect(i, &ty, &th);
                if (tapIn(24, ty, FB_W - 48, th) && selAns != i) {
                    // Repaint only the two tiles that change (old gold
                    // one + newly tapped one) - tiny repaints, no tearing.
                    int oldSel = selAns;
                    selAns = i;
                    if (oldSel >= 0) {
                        int oy, oh;
                        mcTileRect(oldSel, &oy, &oh);
                        drawMcTileOnly(oldSel);
                        blitRect(24, oy, FB_W - 48, oh);
                    }
                    drawMcTileOnly(i);
                    blitRect(24, ty, FB_W - 48, th);
                }
            }
        } else if (q->qtype == 1) {
            // The keyboard overwrites the whole screen, so it needs a full
            // redraw on return, not just the controls region.
            if (tapIn(24, y + 90, FB_W - 48, 60)) { typed = keyboard("Type your answer", typed.c_str()); playNeedsFull = true; }
            if (tapIn(24, y + 162, FB_W - 48, 60)) submit();
        } else if (q->qtype == 2) {
            if (tpActive && tpX >= 60 && tpX <= FB_W - 60 && tpY >= y + 50 && tpY <= y + 96) {
                float t = (float)(tpX - 60) / (FB_W - 120);
                sVal = q->sMin + t * (q->sMax - q->sMin);
                // Repaint just the value text strip + slider track.
                drawSliderOnly();
                blitRect(24, y + 4, FB_W - 48, 92);
            }
            if (tapIn(24, FB_H - 76, FB_W - 48, 60)) submit();
        } else if (q->qtype == 3) {
            for (int i = 1; i < q->nAnswers; i++) {
                int ry, rh;
                puzzleRowRect(i, &ry, &rh);
                if (tapIn(FB_W - 76, ry, 52, rh)) {
                    int t = pOrder[i]; pOrder[i] = pOrder[i - 1]; pOrder[i - 1] = t;
                    // Repaint only the two rows that swapped.
                    int ry0, rh0;
                    puzzleRowRect(i - 1, &ry0, &rh0);
                    drawPuzzleRowOnly(i - 1);
                    drawPuzzleRowOnly(i);
                    blitRect(24, ry0, FB_W - 48, rh0 + 6 + rh);
                }
            }
            if (tapIn(24, FB_H - 76, FB_W - 48, 60)) submit();
        }
        delay(10);
    }
}

// ------------------------------------------------------------------ steal
#define MAX_RESULTS 20
String sUuid[MAX_RESULTS], sTitle[MAX_RESULTS], sCover[MAX_RESULTS];
int nResults = 0;
uint16_t *sCovSpr[MAX_RESULTS] = { nullptr };
int sCovW[MAX_RESULTS], sCovH[MAX_RESULTS];
String stealSearch = "";
String stealMsg = "";

void freeStealCovers(void) {
    for (int i = 0; i < MAX_RESULTS; i++)
        if (sCovSpr[i]) { free(sCovSpr[i]); sCovSpr[i] = nullptr; }
}

void stealSearchNow(void) {
    nResults = 0;
    freeStealCovers();
    String form = "q=" + urlEncode(stealSearch.c_str()) + "&count=20";
    HttpResp r = http_req("POST", "/api/searchkahoot", form);
    if (r.status != 200) { stealMsg = "Search failed (" + String(r.status) + ")"; return; }
    JsonDocument doc;
    if (deserializeJson(doc, r.body)) { stealMsg = "Bad response"; return; }
    for (JsonObject res : doc["results"].as<JsonArray>()) {
        if (nResults >= MAX_RESULTS) break;
        sUuid[nResults] = res["uuid"].as<String>();
        sTitle[nResults] = res["title"].as<String>();
        sCover[nResults] = res["cover"].as<String>();
        nResults++;
    }
    stealMsg = nResults ? "" : "No public quizzes found";
    for (int i = 0; i < nResults; i++) {
        String p = "/api/kahootcover?id=" + urlEncode(sUuid[i].c_str())
                 + "&url=" + urlEncode(sCover[i].c_str()) + "&size=64";
        bool gif = false;
        fetchImage565(p, &sCovSpr[i], &sCovW[i], &sCovH[i], &gif);
    }
}

void drawSteal(void) {
    drawBackground();
    drawCenteredShadow(14, "Steal from Kahoot", C_TEXT, 2);
    drawBack();
    glassPanel(16, 66, FB_W - 230, 60, false);
    drawString(28, 82, stealSearch.c_str(), C_TEXT, 2);
    tile(FB_W - 200, 66, 184, 60, ansColor(0));
    drawCenteredAt(FB_W - 108, 84, "Search", C_INK, 2);
    drawCenteredAt(FB_W - 108, 82, "Search", C_INK, 2);
    drawCentered(150, "Tap a result to steal it", C_FAINT, 1);
    for (int i = 0; i < nResults && i < 7; i++) {
        int ry = 176 + i * 58;
        glassPanel(16, ry, FB_W - 32, 52, (i % 2) == 0);
        if (sCovSpr[i]) {
            int m = sCovW[i] > sCovH[i] ? sCovW[i] : sCovH[i];
            float sc = 46.0f / m;
            int dw = sCovW[i] * sc, dh = sCovH[i] * sc;
            if (dw < 1) dw = 1; if (dh < 1) dh = 1;
            drawSpriteScaled(22 + (46 - dw) / 2, ry + 3 + (46 - dh) / 2,
                             sCovSpr[i], sCovW[i], sCovH[i], dw, dh);
        } else {
            fillRect(22, ry + 3, 46, 46, C_INK);
            drawCenteredAt(22 + 23, ry + 18, "?", C_TEXT, 2);
        }
        drawString(86, ry + 16, sTitle[i].c_str(), C_TEXT, 2);
    }
    if (stealMsg.length()) drawCentered(FB_H - 28, stealMsg.c_str(), C_GOLD, 1);
    blit();
}

void stealLoop(void) {
    stealMsg = "";
    drawSteal();
    while (screen == SCR_STEAL) {
        touch_step();
        if (backHit(12, 12, 120, 44) && tapIn(12, 12, 120, 44)) { screen = SCR_MENU; return; }
        if (tapIn(16, 66, FB_W - 230, 60)) {
            stealSearch = keyboard("Search Kahoot", stealSearch.c_str());
            stealSearchNow();
            drawSteal();
        }
        if (tapIn(FB_W - 200, 66, 184, 60)) {
            if (stealSearch.length()) { stealSearchNow(); drawSteal(); }
        }
        for (int i = 0; i < nResults && i < 7; i++) {
            int ry = 176 + i * 58;
            if (tapIn(16, ry, FB_W - 32, 52)) {
                drawBackground();
                drawCentered(FB_H / 2 - 20, "Stealing...", C_TEXT, 2);
                blit();
                String form = "input=" + urlEncode(sUuid[i].c_str());
                HttpResp r = http_req("POST", "/api/stealquiz", form);
                if (r.status == 200) {
                    JsonDocument doc;
                    if (!deserializeJson(doc, r.body)) {
                        String nm = doc["quiz_name"].as<String>();
                        if (nm.length()) { statusLine = "Stole: " + nm; screen = SCR_MENU; return; }
                    }
                    stealMsg = "Stole it!";
                } else {
                    stealMsg = "Steal failed (" + String(r.status) + ")";
                }
                drawSteal();
            }
        }
        delay(10);
    }
}

// ------------------------------------------------------------------ config
void configLoop(void) {
    drawBackground();
    drawCenteredShadow(70, "Server settings", C_TEXT, 2);
    drawCentered(150, String("Current: " + SERVER_HOST + ":" + String(SERVER_PORT)).c_str(), C_FAINT, 1);
    tile(80, 240, FB_W - 160, 90, ansColor(3));
    drawCenteredShadow(272, "Change address", C_INK, 2);
    tile(80, 370, FB_W - 160, 90, C_GOLD);
    drawCenteredShadow(402, "Back", C_INK, 2);
    blit();
    while (screen == SCR_CONFIG) {
        touch_step();
        if (tapIn(80, 240, FB_W - 160, 90)) {
            String v = keyboard("Server address (host:port)", SERVER_HOST.c_str());
            int ci = v.indexOf(':');
            if (ci > 0) {
                SERVER_HOST = v.substring(0, ci);
                SERVER_PORT = v.substring(ci + 1).toInt();
            } else if (v.length()) {
                SERVER_HOST = v;
            }
            drawBackground();
            drawCenteredShadow(70, "Server settings", C_TEXT, 2);
            drawCentered(150, String("Current: " + SERVER_HOST + ":" + String(SERVER_PORT)).c_str(), C_FAINT, 1);
            tile(80, 240, FB_W - 160, 90, ansColor(3));
            drawCenteredShadow(272, "Change address", C_INK, 2);
            tile(80, 370, FB_W - 160, 90, C_GOLD);
            drawCenteredShadow(402, "Back", C_INK, 2);
            blit();
        }
        if (tapIn(80, 370, FB_W - 160, 90)) { screen = SCR_MENU; return; }
        delay(10);
    }
}

// ------------------------------------------------------------------ setup/loop
void setup() {
    Serial.begin(115200);
    randomSeed(analogRead(0) + millis());

    // touch_gt911_init() initialises I2C and the IO expander internally.
    touch_gt911_init();
    waveshare_esp32_s3_rgb_lcd_init();
    wavesahre_rgb_lcd_bl_on();

    void *fb1 = nullptr, *fb2 = nullptr;
    waveshare_get_frame_buffer(&fb1, &fb2);
    fb = (uint16_t *)fb1;
    memset(fb, 0, FB_W * FB_H * 2);

    drawBackground();
    drawCenteredShadow(FB_H / 2 - 30, "QuizFabric", C_GOLD, 3);
    drawCentered(FB_H / 2 + 24, "connecting to wifi...", C_FAINT, 1);
    blit();

    WiFi.mode(WIFI_STA);
    // No modem sleep: the radio's sleep/wake cycles disturb the RGB LCD
    // DMA timing and make the screen flicker, especially during transfers.
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    for (int i = 0; WiFi.status() != WL_CONNECTED && i < 40; i++) delay(500);
    if (WiFi.status() != WL_CONNECTED) {
        drawCentered(FB_H / 2 + 50, "wifi failed - check settings", C_BAD, 1);
        blit();
    }
}

void loop() {
    switch (screen) {
        case SCR_MENU: menuLoop(); break;
        case SCR_LIST: listLoop(); break;
        case SCR_PLAY: playLoop(); break;
        case SCR_STEAL: stealLoop(); break;
        case SCR_CONFIG: configLoop(); break;
    }
    delay(10);
}
