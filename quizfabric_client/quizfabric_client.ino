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
#include <Preferences.h>
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
Preferences qfPrefs;

// Loads the saved server address (NVS) once at boot.
void loadServerPrefs(void) {
    qfPrefs.begin("quizfabric", true);
    String h = qfPrefs.getString("host", "");
    if (h.length()) { SERVER_HOST = h; SERVER_PORT = qfPrefs.getInt("port", 80); }
    qfPrefs.end();
}

// Persists the server address after the config screen edits it.
void saveServerPrefs(void) {
    qfPrefs.begin("quizfabric", false);
    qfPrefs.putString("host", SERVER_HOST);
    qfPrefs.putInt("port", SERVER_PORT);
    qfPrefs.end();
}

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
// Fill one row segment with 32-bit stores (2 pixels at a time) - about
// half the time of the per-pixel loop. Every millisecond the main loop
// spends drawing starves the LCD bounce ISR on the same core and shows
// as screen glitching.
static inline void fillRow(int x, int y, int w, uint16_t c) {
    uint32_t cc = c | ((uint32_t)c << 16);
    uint16_t *row = &fb[y * FB_W];
    int xx = x;
    if (xx & 1) row[xx++] = c;
    uint32_t *p32 = (uint32_t *)(row + xx);
    int n = (x + w - xx) >> 1;
    while (n-- > 0) *p32++ = cc;
    if ((x + w - xx) & 1) row[x + w - 1] = c;
}

void fillRect(int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_W) w = FB_W - x;
    if (y + h > FB_H) h = FB_H - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) fillRow(x, yy, w, c);
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
    for (int y = 0; y < FB_H; y++)
        fillRow(0, y, FB_W, lerpColor(top, bot, (float)y / FB_H));
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
    for (int yy = y; yy < y + h; yy++)
        fillRow(x, yy, w, lerpColor(top, bot, (float)yy / FB_H));
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
static bool tpPending = false;
static bool tpPendingState = false;

void touch_step(void) {
    tp = touch_gt911_read_point(1);
    bool active = tp.cnt > 0;
    if (active && tpActive) { tpX = tp.x[0]; tpY = tp.y[0]; return; }
    if (!active && !tpActive) { tpPending = false; return; }
    // Edge: the GT911 occasionally drops points mid-touch while polling,
    // which fakes releases (and re-presses) and makes taps unreliable.
    // Confirm every edge with one more identical read before committing.
    if (!tpPending) { tpPending = true; tpPendingState = active; return; }
    if (tpPendingState != active) { tpPendingState = active; return; }
    tpPending = false;
    if (active) {
        tpActive = true;
        tpDownMs = millis();
        tapConsumed = false;
        tpX = tp.x[0]; tpY = tp.y[0];
    } else {
        tpActive = false;
    }
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
// PICA200 8x8 swizzle: PERM[j] = row-major source position stored at
// output position j (devkitPro tex3ds, matches imgconv.py on the server).
static const uint8_t PERM[64] = {
    0, 1, 8, 9, 2, 3, 10, 11, 16, 17, 24, 25, 18, 19, 26, 27,
    4, 5, 12, 13, 6, 7, 14, 15, 20, 21, 28, 29, 22, 23, 30, 31,
    32, 33, 40, 41, 34, 35, 42, 43, 48, 49, 56, 57, 50, 51, 58, 59,
    36, 37, 44, 45, 38, 39, 46, 47, 52, 53, 60, 61, 54, 55, 62, 63
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

// ------------------------------------------------------------------ async fetch
// Banners / question images / covers are downloaded on this background
// task. The touch loops used to block on those fetches and ate taps (the
// quiz list needed ~60 taps to register; Submit was dead while a question
// image loaded). One job at a time; job IDs let callers discard results
// from a superseded job.
static TaskHandle_t fetchTaskH = NULL;
static SemaphoreHandle_t fetchLock = NULL;
static String fetchPath;
static volatile uint32_t fetchJobId = 0;
static String fetchBody;
static int fetchStatus = -1;
static volatile uint32_t fetchDoneId = 0;
static uint16_t *fetchSpr = nullptr;
static int fetchSprW = 0, fetchSprH = 0;
static bool fetchSprGif = false;

void fetchTaskFn(void *arg) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(fetchLock, portMAX_DELAY);
        String p = fetchPath;
        uint32_t jid = fetchJobId;
        xSemaphoreGive(fetchLock);
        HttpResp r = http_req("GET", p);
        // Decode .565 payloads HERE on core 0: the per-pixel PERM loop
        // starves the LCD bounce-copy ISR when it runs on the UI core
        // (core 1) and that is what made the screen glitch during and
        // after image downloads. Detect .565 by shape (some image URLs
        // don't end in .565 - banners/covers), never by the path.
        uint16_t *spr = nullptr; int sw = 0, sh = 0; bool isGif = false;
        if (r.status == 200 && r.body.length() >= 6) {
            const uint8_t *d = (const uint8_t *)r.body.c_str();
            int iw = d[0] | (d[1] << 8);
            int ih = d[2] | (d[3] << 8);
            if (iw >= 1 && ih >= 1 && iw <= 1024 && ih <= 1024) {
                int pw = pow2up(iw), ph = pow2up(ih);
                size_t fb = (size_t)pw * ph * 4;
                if (r.body.length() == 4 + fb) {
                    if (!decode565(d, r.body.length(), &spr, &sw, &sh)) { /* not decodable */ }
                } else if (r.body.length() >= 6) {
                    int n = d[4];
                    if (n >= 2 && r.body.length() == 6 + (size_t)n * fb) isGif = true;
                }
            }
            if (spr || isGif) r.body = "";   // free the raw body immediately
        }
        xSemaphoreTake(fetchLock, portMAX_DELAY);
        if (jid == fetchJobId) {   // not superseded by a newer job
            fetchBody = r.body;
            fetchStatus = r.status;
            fetchDoneId = jid;
            fetchSpr = spr; fetchSprW = sw; fetchSprH = sh; fetchSprGif = isGif;
        } else if (spr) {
            free(spr);   // stale job - discard its sprite
        }
        xSemaphoreGive(fetchLock);
    }
}

void fetchTaskStart(void) {
    fetchLock = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(fetchTaskFn, "fetch", 10240, NULL, 1, &fetchTaskH, 0);
}

// Ask the task to fetch a path. Returns the job id to pass to fetchTake.
uint32_t fetchStart(const String &path) {
    xSemaphoreTake(fetchLock, portMAX_DELAY);
    fetchPath = path;
    fetchJobId++;
    xSemaphoreGive(fetchLock);
    xTaskNotifyGive(fetchTaskH);
    return fetchJobId;
}

// True when the result for jobId (or a newer one) is ready; copies it out.
bool fetchTake(uint32_t jobId, String &body, int *status) {
    xSemaphoreTake(fetchLock, portMAX_DELAY);
    bool ok = jobId != 0 && fetchDoneId >= jobId;
    if (ok) { body = fetchBody; *status = fetchStatus; }
    xSemaphoreGive(fetchLock);
    return ok;
}

// Like fetchTake but hands over the decoded sprite (the caller owns it).
bool fetchTakeSprite(uint32_t jobId, int *status, uint16_t **spr, int *w, int *h, bool *isGif) {
    xSemaphoreTake(fetchLock, portMAX_DELAY);
    bool ok = jobId != 0 && fetchDoneId >= jobId;
    if (ok) {
        *status = fetchStatus;
        *spr = fetchSpr; *w = fetchSprW; *h = fetchSprH; *isGif = fetchSprGif;
        fetchSpr = nullptr;   // caller owns it now
    }
    xSemaphoreGive(fetchLock);
    return ok;
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
enum Scr { SCR_MENU, SCR_LIST, SCR_PLAY, SCR_STEAL, SCR_CONFIG, SCR_JOIN };
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

// Non-blocking banner loader: keeps one fetch in flight on the background
// task and polls it. Returns the slot that just got its thumbnail (caller
// repaints), or -1. The touch loop never blocks.
static int thumbFetchSlot = -1;
static uint32_t thumbJobId = 0;

int pumpThumb(void) {
    if (thumbJobId) {
        int status = -1;
        uint16_t *s = nullptr; int w = 0, h = 0; bool gif = false;
        if (!fetchTakeSprite(thumbJobId, &status, &s, &w, &h, &gif)) return -1;   // still loading
        thumbJobId = 0;
        int slot = thumbFetchSlot;
        if (s) {
            thumbSpr[slot] = s; thumbW[slot] = w; thumbH[slot] = h;
            return slot;
        }
        return -1;
    }
    for (int i = 0; i < LIST_ROWS; i++) {
        int qi = listScroll + i;
        if (qi < 0 || qi >= nQuizzes) continue;
        if (!quizHasBanner[qi] || thumbSpr[i] || thumbFor[i] == qi) continue;
        thumbFor[i] = qi;
        thumbFetchSlot = i;
        String enc = urlEncode(quizNames[qi].c_str());
        thumbJobId = fetchStart("/api/quizbanner/" + enc);
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
    tile(80, 190, FB_W - 160, 85, ansColor(0));
    drawCenteredShadow(220, "Play Solo", C_TEXT, 3);
    tile(80, 290, FB_W - 160, 85, ansColor(1));
    drawCenteredShadow(320, "Join game", C_TEXT, 3);
    tile(80, 390, FB_W - 160, 85, ansColor(2));
    drawCenteredShadow(420, "Steal from Kahoot", C_TEXT, 3);
    glassPanel(80, 490, FB_W - 160, 85, true);
    drawCentered(520, "Server settings", C_TEXT, 2);
    if (statusLine.length()) drawCentered(FB_H - 30, statusLine.c_str(), C_BAD, 1);
    blit();
}

void menuLoop(void) {
    drawMenu();
    while (screen == SCR_MENU) {
        touch_step();
        if (tapIn(80, 190, FB_W - 160, 85)) {
            // Show feedback immediately so the tap visibly registered,
            // then block on the quiz-list fetch (one-time, at transition).
            drawBackground();
            drawCentered(FB_H / 2 - 20, "Loading quizzes...", C_TEXT, 2);
            blit();
            screen = SCR_LIST;
            fetchQuizList();
            return;
        }
        if (tapIn(80, 290, FB_W - 160, 85)) { screen = SCR_JOIN; return; }
        if (tapIn(80, 390, FB_W - 160, 85)) { screen = SCR_STEAL; return; }
        if (tapIn(80, 490, FB_W - 160, 85)) { screen = SCR_CONFIG; return; }
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
uint32_t qImgJobId = 0;    // background fetch job for the question image

void freeQuestionImage(void) {
    if (qImgSpr) { free(qImgSpr); qImgSpr = nullptr; }
}

// Poll the background question-image fetch; redraws when it lands.
void pollQuestionImage(void) {
    if (!qImgJobId) return;
    int status = -1;
    uint16_t *s = nullptr; int w = 0, h = 0; bool gif = false;
    if (!fetchTakeSprite(qImgJobId, &status, &s, &w, &h, &gif)) return;   // still loading
    qImgJobId = 0;
    if (s) { qImgSpr = s; qImgW = w; qImgH = h; }
    else if (gif) qImgGif = true;   // animated - skipped
    playNeedsFull = true;
}

bool parseQuizQuestions(const String &body) {
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
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
    // Question image downloads on the background task; the play loop
    // polls it so touch stays responsive.
    qImgJobId = 0;
    qImgGif = false;
    if (q->imgUrl.length()) qImgJobId = fetchStart(q->imgUrl + ".565");
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
// The whole block MUST fit inside avail: the 68px cap used to make the
// block taller than avail for 5+ answers, so the last tiles covered the
// Submit button and taps on Submit hit an answer tile instead.
void mcTileRect(int i, int *ty, int *th) {
    QuizQ *q = &qs[qi];
    int avail = FB_H - 90 - playControlsY;
    int bh = avail / q->nAnswers;
    if (bh > 68) bh = 68;
    int totalH = q->nAnswers * bh + (q->nAnswers - 1) * 8;
    while (totalH > avail && bh > 14) { bh--; totalH = q->nAnswers * bh + (q->nAnswers - 1) * 8; }
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
    int avail = FB_H - 90 - playControlsY;
    int h = avail / q->nAnswers;
    if (h > 44) h = 44;
    while (q->nAnswers * (h + 6) > avail && h > 10) h--;   // keep clear of Submit
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
    // The quiz JSON downloads on the background task too: a synchronous
    // big download right here made the screen glitch for seconds while
    // the WiFi bursts fought the LCD DMA.
    String enc = urlEncode(quizNames[listSel].c_str());
    uint32_t quizJobId = fetchStart("/api/quiz/" + enc);
    while (screen == SCR_PLAY && quizJobId) {
        touch_step();
        int status = -1;
        String body;
        if (!fetchTake(quizJobId, body, &status)) { delay(10); continue; }
        quizJobId = 0;
        if (status != 200 || !parseQuizQuestions(body)) {
            statusLine = "Could not load that quiz";
            screen = SCR_MENU;
            return;
        }
    }
    if (screen != SCR_PLAY) return;
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
        // Poll the background question-image fetch (non-blocking).
        pollQuestionImage();
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
            // Submit is checked first so it wins even if an answer tile
            // edge overlaps it.
            if (tapIn(24, FB_H - 76, FB_W - 48, 60)) submit();
            else {
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
            if (tapIn(24, FB_H - 76, FB_W - 48, 60)) submit();
            else {
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
            }
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

static int sCovIdx = 0;
static uint32_t sCovJobId = 0;

// One cover at a time on the background task; returns the row whose
// cover just arrived (caller repaints it), or -1. Never blocks.
int pumpStealCovers(void) {
    if (sCovJobId) {
        int status = -1;
        uint16_t *s = nullptr; int w = 0, h = 0; bool gif = false;
        if (!fetchTakeSprite(sCovJobId, &status, &s, &w, &h, &gif)) return -1;   // still loading
        sCovJobId = 0;
        int idx = sCovIdx;
        sCovIdx++;
        if (s) {
            sCovSpr[idx] = s; sCovW[idx] = w; sCovH[idx] = h;
            return idx;
        }
        return -1;
    }
    while (sCovIdx < nResults) {
        if (sCover[sCovIdx].length()) {
            String p = "/api/kahootcover?id=" + urlEncode(sUuid[sCovIdx].c_str())
                     + "&url=" + urlEncode(sCover[sCovIdx].c_str()) + "&size=64";
            sCovJobId = fetchStart(p);
            break;
        }
        sCovIdx++;
    }
    return -1;
}

void stealSearchNow(void) {
    nResults = 0;
    freeStealCovers();
    sCovJobId = 0;
    sCovIdx = 0;
    drawBackground();
    drawCentered(FB_H / 2 - 20, "Searching Kahoot...", C_TEXT, 2);
    blit();
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
}

void drawStealRow(int i) {
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
    for (int i = 0; i < nResults && i < 7; i++) drawStealRow(i);
    if (stealMsg.length()) drawCentered(FB_H - 28, stealMsg.c_str(), C_GOLD, 1);
    blit();
}

void stealLoop(void) {
    stealMsg = "";
    drawSteal();
    while (screen == SCR_STEAL) {
        // Covers download on the background task; repaint each row as its
        // cover lands (never while a finger is down).
        if (!tpActive) {
            int slot = pumpStealCovers();
            if (slot >= 0 && slot < 7) {
                drawStealRow(slot);
                blitRect(16, 176 + slot * 58, FB_W - 32, 52);
            }
        }
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
            String init = SERVER_HOST + ":" + String(SERVER_PORT);
            String v = keyboard("Server address (host:port)", init.c_str());
            int ci = v.indexOf(':');
            if (ci > 0) {
                SERVER_HOST = v.substring(0, ci);
                SERVER_PORT = v.substring(ci + 1).toInt();
            } else if (v.length()) {
                SERVER_HOST = v;
            }
            saveServerPrefs();
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

// ------------------------------------------------------------------ game join
// Minimal Engine.IO v4 + Socket.IO v4 client over a plain WebSocket, just
// enough for QuizFabric's live-game events (join_game / new_question /
// submit_answer / leaderboard). The Pi's plain-HTTP dispatcher lets the
// QuizFabric3DS UA through on port 80/5710 including WebSocket upgrades.
WiFiClient sock;
String sockSid = "";
bool sockOpen = false;
unsigned long sockLastIn = 0;
static uint32_t wsNeed = 0;
static uint8_t wsOp = 0;
static bool wsMasked = false;
static uint8_t wsMaskKey[4];
static String wsFrame;

String sioCode = "";
String sioName = "";
String sioQuiz = "";
int sioState = 0;   // 0 idle, 1 lobby, 2 question, 3 submitted, 4 reveal, 5 leaderboard, 6 finished
String sioMsg = "";
String sioQText = "";
String sioAns[8];
int sioNAns = 0;
int sioQType = 0;
float sioMin = 0, sioMax = 100, sioVal = 50;
int sioSel = -1;
String sioTyped = "";
int sioPOrder[8];
unsigned long sioQStart = 0;
bool sioRight = false;
String sioRightAns = "";
uint32_t sioImgJobId = 0;
uint16_t *sioImgSpr = nullptr;
int sioImgW = 0, sioImgH = 0;
String sioLb[12];
int sioLbN = 0;

// --- compact SHA1 (for the WebSocket handshake key) ---
static void sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint64_t bits = (uint64_t)len * 8;
    size_t plen = ((len + 8) / 64 + 1) * 64;
    uint8_t *buf = (uint8_t *)malloc(plen);
    if (!buf) return;
    memset(buf, 0, plen);
    memcpy(buf, data, len);
    buf[len] = 0x80;
    for (int i = 0; i < 8; i++) buf[plen - 1 - i] = (bits >> (8 * i)) & 0xFF;
    for (size_t off = 0; off < plen; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)buf[off + i * 4] << 24) | ((uint32_t)buf[off + i * 4 + 1] << 16)
                 | ((uint32_t)buf[off + i * 4 + 2] << 8) | buf[off + i * 4 + 3];
        for (int i = 16; i < 80; i++) {
            uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (v << 1) | (v >> 31);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    free(buf);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = h[i] >> 24; out[i * 4 + 1] = h[i] >> 16;
        out[i * 4 + 2] = h[i] >> 8; out[i * 4 + 3] = h[i];
    }
}

static const char B64C[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64encode(const uint8_t *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = in[i] << 16;
        if (i + 1 < n) v |= in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = B64C[(v >> 18) & 63];
        out[o++] = B64C[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? B64C[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? B64C[v & 63] : '=';
    }
    out[o] = 0;
}

void wsSend(int opcode, const char *data, size_t len) {
    if (!sockOpen || !sock.connected()) return;
    uint8_t hdr[14];
    size_t h = 2;
    hdr[0] = 0x80 | (opcode & 0x0F);
    if (len < 126) { hdr[1] = 0x80 | len; }
    else if (len < 65536) { hdr[1] = 0x80 | 126; hdr[2] = len >> 8; hdr[3] = len & 0xFF; h = 4; }
    else {
        hdr[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++) hdr[2 + i] = ((uint64_t)len >> (56 - 8 * i)) & 0xFF;
        h = 10;
    }
    uint8_t mask[4];
    for (int i = 0; i < 4; i++) mask[i] = esp_random();
    memcpy(hdr + h, mask, 4);
    sock.write(hdr, h + 4);
    for (size_t i = 0; i < len; i++) sock.write((uint8_t)(data[i] ^ mask[i & 3]));
    sock.flush();
}

// Returns one complete WebSocket text frame in out (true) or nothing yet.
bool wsPoll(String &out) {
    if (!sockOpen || !sock.connected()) return false;
    if (wsNeed == 0 && sock.available() >= 2) {
        int b0 = sock.read(), b1 = sock.read();
        wsOp = b0 & 0x0F;
        wsMasked = b1 & 0x80;
        uint64_t len = b1 & 0x7F;
        if (len == 126) {
            if (sock.available() < 2) return false;
            len = (sock.read() << 8) | sock.read();
        } else if (len == 127) {
            if (sock.available() < 8) return false;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | sock.read();
        }
        if (wsMasked) {
            if (sock.available() < 4) return false;
            for (int i = 0; i < 4; i++) wsMaskKey[i] = sock.read();
        }
        if (len > 200000) { sockOpen = false; return false; }   // sanity
        wsNeed = len;
        if (wsOp == 0x8) { sockOpen = false; wsNeed = 0; return false; }
        wsFrame = "";
    }
    if (wsNeed == 0) return false;
    while (sock.available() && wsNeed > 0) {
        uint8_t ch = sock.read();
        if (wsMasked) ch ^= wsMaskKey[wsFrame.length() & 3];
        wsFrame += (char)ch;
        wsNeed--;
    }
    if (wsNeed > 0) return false;
    String p = wsFrame;
    wsFrame = "";
    if (wsOp == 0x9) { wsSend(0xA, p.c_str(), p.length()); return false; }   // ping -> pong
    if (wsOp == 0x1 || wsOp == 0x0) { out = p; return true; }
    return false;
}

// Process one socket.io message.
void sioHandle(const String &m) {
    sockLastIn = millis();
    if (m == "2") { wsSend(0x1, "3", 1); return; }   // engine ping -> pong
    if (!m.startsWith("42")) return;
    JsonDocument doc;
    if (deserializeJson(doc, m.c_str() + 2)) return;
    String name = doc[0].as<String>();
    JsonVariant d = doc[1];
    if (name == "joined") {
        sioQuiz = d["quiz_name"].as<String>();
        sioState = 1;
        sioMsg = "";
    } else if (name == "error") {
        sioMsg = d["message"].as<String>();
        if (!sioMsg.length()) sioMsg = "Join failed";
        sioState = 0;
    } else if (name == "new_question") {
        sioQText = d["question"].as<String>();
        sioNAns = 0;
        for (JsonVariant a : d["answers"].as<JsonArray>()) {
            if (sioNAns >= 8) break;
            sioAns[sioNAns++] = a.as<String>();
        }
        const char *t = d["type"] | "mc";
        if (!strcmp(t, "mc")) sioQType = 0;
        else if (!strcmp(t, "slider")) {
            sioQType = 2;
            sioMin = d["min"] | 0.0f;
            sioMax = d["max"] | 100.0f;
            sioVal = (sioMin + sioMax) / 2.0f;
        } else if (!strcmp(t, "puzzle")) {
            sioQType = 3;
            for (int i = 0; i < sioNAns; i++) sioPOrder[i] = i;
        } else sioQType = 1;
        sioSel = -1;
        sioTyped = "";
        sioQStart = millis();
        sioState = 2;
        if (sioImgSpr) { free(sioImgSpr); sioImgSpr = nullptr; }
        sioImgJobId = 0;
        JsonArray imgs = d["images"].as<JsonArray>();
        if (imgs.size() > 0) {
            String u = imgs[0]["url"].as<String>();
            if (u.length()) sioImgJobId = fetchStart(u + ".565");
        }
    } else if (name == "answer_revealed") {
        sioRight = d["correct"] | false;
        sioRightAns = d["correct_answer"].as<String>();
        sioState = 4;
    } else if (name == "answers_revealed") {
        sioRightAns = d["correct_answer"].as<String>();
    } else if (name == "leaderboard") {
        sioLbN = 0;
        for (JsonVariant e : d["leaderboard"].as<JsonArray>()) {
            if (sioLbN >= 12) break;
            sioLb[sioLbN++] = e["name"].as<String>() + ": " + e["score"].as<String>();
        }
        sioRightAns = d["correct_answer"].as<String>();
        sioState = 5;
    } else if (name == "final_leaderboard") {
        sioLbN = 0;
        for (JsonVariant e : d["leaderboard"].as<JsonArray>()) {
            if (sioLbN >= 12) break;
            sioLb[sioLbN++] = e["name"].as<String>() + ": " + e["score"].as<String>();
        }
        sioState = 6;
    }
}

bool sioConnect(void) {
    // Engine.IO handshake over polling, then upgrade to a WebSocket.
    HttpResp r = http_req("GET", "/socket.io/?EIO=4&transport=polling");
    if (r.status != 200) return false;
    int ss = r.body.indexOf("\"sid\":\"");
    if (ss < 0) return false;
    ss += 7;
    int se = r.body.indexOf('"', ss);
    if (se < 0) return false;
    sockSid = r.body.substring(ss, se);
    if (!sock.connect(SERVER_HOST.c_str(), SERVER_PORT)) return false;
    sock.setTimeout(10000);
    uint8_t key[16];
    for (int i = 0; i < 16; i++) key[i] = esp_random();
    char kb64[32];
    b64encode(key, 16, kb64);
    String req = "GET /socket.io/?EIO=4&transport=websocket&sid=" + sockSid
               + " HTTP/1.1\r\nHost: " + SERVER_HOST
               + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + kb64
               + "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: QuizFabric3DS-ESP32\r\n\r\n";
    sock.print(req);
    String head;
    unsigned long dl = millis() + 10000;
    while (millis() < dl && head.indexOf("\r\n\r\n") < 0) {
        if (sock.available()) head += (char)sock.read();
        else delay(1);
    }
    if (head.indexOf("101") < 0) { sock.stop(); return false; }
    sockOpen = true;
    sockLastIn = millis();
    wsFrame = ""; wsNeed = 0;
    wsSend(0x1, "40", 2);   // socket.io namespace open
    return true;
}

void sioSubmit(const String &answerJson) {
    float secs = (millis() - sioQStart) / 1000.0f;
    String ev = "42[\"submit_answer\",{\"code\":\"" + sioCode + "\",\"answer\":"
              + answerJson + ",\"time_taken\":" + String(secs, 1) + "}]";
    wsSend(0x1, ev.c_str(), ev.length());
    sioState = 3;
}

void drawJoin(void) {
    drawBackground();
    drawCentered(14, String("Game " + sioCode).c_str(), C_FAINT, 1);
    drawBack();
    if (sioState <= 1) {
        drawCentered(120, sioQuiz.length() ? String("Quiz: " + sioQuiz).c_str() : "Joining game...", C_TEXT, 2);
        drawCentered(180, "Waiting for the host to start...", C_FAINT, 1);
        if (sioMsg.length()) drawCentered(240, sioMsg.c_str(), C_BAD, 1);
        blit();
        return;
    }
    if (sioState == 3) {
        drawCentered(FB_H / 2 - 10, "Answer sent - waiting for others...", C_TEXT, 2);
        blit();
        return;
    }
    if (sioState == 4) {
        tile(24, 200, FB_W - 48, 120, sioRight ? C_GOOD : C_BAD);
        drawCenteredShadow(228, sioRight ? "Correct!" : "Nope", C_INK, 2);
        drawCentered(280, String("Answer: " + sioRightAns).c_str(), C_INK, 2);
        blit();
        return;
    }
    if (sioState == 5 || sioState == 6) {
        drawCenteredShadow(100, sioState == 6 ? "Final scores" : "Leaderboard", C_TEXT, 2);
        for (int i = 0; i < sioLbN; i++) {
            glassPanel(80, 140 + i * 36, FB_W - 160, 32, (i % 2) == 0);
            drawString(100, 140 + i * 36 + 10, sioLb[i].c_str(), C_TEXT, 2);
        }
        if (sioState == 5) drawCentered(FB_H - 30, String("Answer: " + sioRightAns).c_str(), C_FAINT, 1);
        blit();
        return;
    }
    // state 2: live question
    int y = 66;
    String lines[3];
    int nl = wrapText(sioQText.c_str(), FB_W - 48, 2, lines, 3);
    for (int i = 0; i < nl; i++) { drawString(24, y, lines[i].c_str(), C_TEXT, 2); y += 22; }
    y += 4;
    if (sioImgSpr) {
        float sc = 1.0f;
        int maxw = FB_W - 48, maxh = 120;
        if (sioImgW > maxw) sc = (float)maxw / sioImgW;
        if (sioImgH * sc > maxh) sc = (float)maxh / sioImgH;
        int dw = sioImgW * sc, dh = sioImgH * sc;
        if (dw < 1) dw = 1; if (dh < 1) dh = 1;
        drawSpriteScaled((FB_W - dw) / 2, y, sioImgSpr, sioImgW, sioImgH, dw, dh);
        y += dh + 6;
    }
    int cy = y;
    if (sioQType == 0) {
        int avail = FB_H - 80 - cy;
        int bh = avail / sioNAns;
        if (bh > 64) bh = 64;
        int totalH = sioNAns * bh + (sioNAns - 1) * 6;
        while (totalH > avail && bh > 12) { bh--; totalH = sioNAns * bh + (sioNAns - 1) * 6; }
        int ay = cy + (avail - totalH) / 2;
        if (ay < cy) ay = cy;
        for (int i = 0; i < sioNAns; i++) {
            int by = ay + i * (bh + 6);
            tile(24, by, FB_W - 48, bh, sioSel == i ? C_GOLD : ansColor(i));
            drawString(40, by + bh / 2 - 8, sioAns[i].c_str(), sioSel == i ? C_INK : C_TEXT, 2);
        }
    } else if (sioQType == 2) {
        char txt[48];
        snprintf(txt, 48, "%.1f    (%.1f - %.1f)", sioVal, sioMin, sioMax);
        drawCentered(cy + 8, txt, C_TEXT, 2);
        glassPanel(60, cy + 50, FB_W - 120, 46, false);
        float t = (sioMax > sioMin) ? (sioVal - sioMin) / (sioMax - sioMin) : 0.0f;
        fillRect(60, cy + 50, (FB_W - 120) * t, 46, C_GOLD);
        tile(24, FB_H - 90, FB_W - 48, 60, C_GOOD);
        drawCenteredShadow(FB_H - 64, "Send", C_INK, 2);
    } else if (sioQType == 3) {
        for (int i = 0; i < sioNAns; i++) {
            int ry = cy + i * 46;
            tile(24, ry, FB_W - 110, 40, ansColor(i));
            drawString(40, ry + 12, sioAns[sioPOrder[i]].c_str(), C_TEXT, 2);
            glassPanel(FB_W - 76, ry, 52, 40, true);
            drawCenteredAt(FB_W - 50, ry + 12, "^", C_TEXT, 2);
        }
        tile(24, FB_H - 90, FB_W - 48, 60, C_GOOD);
        drawCenteredShadow(FB_H - 64, "Send", C_INK, 2);
    } else {
        drawCentered(cy + 10, String("Your answer: " + sioTyped).c_str(), C_TEXT, 2);
        tile(24, cy + 60, FB_W - 48, 60, ansColor(3));
        drawCenteredShadow(cy + 76, "Type answer", C_INK, 2);
        tile(24, FB_H - 90, FB_W - 48, 60, C_GOOD);
        drawCenteredShadow(FB_H - 64, "Send", C_INK, 2);
    }
    blit();
}

void joinLoop(void) {
    sioState = 0;
    sioMsg = "";
    sioQuiz = "";
    if (sioImgSpr) { free(sioImgSpr); sioImgSpr = nullptr; }
    sioImgJobId = 0;
    String code = keyboard("Enter the game code", sioCode.c_str());
    if (!code.length()) { screen = SCR_MENU; return; }
    sioCode = code;
    String name = keyboard("Enter your name", sioName.length() ? sioName.c_str() : "ESP32");
    if (!name.length()) { screen = SCR_MENU; return; }
    sioName = name;

    drawBackground();
    drawCentered(FB_H / 2 - 20, "Joining game...", C_TEXT, 2);
    blit();
    if (!sioConnect()) { sioMsg = "Could not connect to the game"; sioState = 0; }
    else {
        String ev = "42[\"join_game\",{\"code\":\"" + sioCode + "\",\"name\":\"" + sioName + "\",\"avatar\":\"\"}]";
        wsSend(0x1, ev.c_str(), ev.length());
    }

    while (screen == SCR_JOIN) {
        touch_step();
        if (backHit(12, 12, 120, 44) && tapIn(12, 12, 120, 44)) {
            sockOpen = false;
            sock.stop();
            screen = SCR_MENU;
            return;
        }
        String out;
        while (wsPoll(out)) sioHandle(out);
        if (sockOpen && millis() - sockLastIn > 60000) { sockOpen = false; sioMsg = "Connection lost"; sioState = 0; }
        if (sioImgJobId) {
            int status = -1;
            uint16_t *s = nullptr; int w = 0, h = 0; bool gif = false;
            if (fetchTakeSprite(sioImgJobId, &status, &s, &w, &h, &gif)) {
                sioImgJobId = 0;
                if (s) { sioImgSpr = s; sioImgW = w; sioImgH = h; }
            }
        }
        if (sioState == 2) {
            int cy = 66;
            String lines[3];
            int nl = wrapText(sioQText.c_str(), FB_W - 48, 2, lines, 3);
            cy += nl * 22 + 4;
            if (sioImgSpr) {
                float sc = 1.0f;
                if (sioImgW > FB_W - 48) sc = (float)(FB_W - 48) / sioImgW;
                if (sioImgH * sc > 120) sc = 120.0f / sioImgH;
                cy += (int)(sioImgH * sc) + 6;
            }
            if (sioQType == 0) {
                int avail = FB_H - 80 - cy;
                int bh = avail / sioNAns;
                if (bh > 64) bh = 64;
                int totalH = sioNAns * bh + (sioNAns - 1) * 6;
                while (totalH > avail && bh > 12) { bh--; totalH = sioNAns * bh + (sioNAns - 1) * 6; }
                int ay = cy + (avail - totalH) / 2;
                if (ay < cy) ay = cy;
                for (int i = 0; i < sioNAns; i++) {
                    int by = ay + i * (bh + 6);
                    if (tapIn(24, by, FB_W - 48, bh)) {
                        sioSel = i;
                        sioSubmit(String(i));   // tap = answer, Kahoot style
                    }
                }
            } else if (sioQType == 2) {
                if (tpActive && tpX >= 60 && tpX <= FB_W - 60 && tpY >= cy + 50 && tpY <= cy + 96) {
                    float t = (float)(tpX - 60) / (FB_W - 120);
                    sioVal = sioMin + t * (sioMax - sioMin);
                    drawJoin();
                }
                if (tapIn(24, FB_H - 90, FB_W - 48, 60)) sioSubmit(String(sioVal, 1));
            } else if (sioQType == 3) {
                for (int i = 1; i < sioNAns; i++) {
                    int ry = cy + i * 46;
                    if (tapIn(FB_W - 76, ry, 52, 40)) {
                        int t = sioPOrder[i]; sioPOrder[i] = sioPOrder[i - 1]; sioPOrder[i - 1] = t;
                        drawJoin();
                    }
                }
                if (tapIn(24, FB_H - 90, FB_W - 48, 60)) {
                    String list = "[";
                    for (int i = 0; i < sioNAns; i++) {
                        if (i) list += ",";
                        list += "\"" + sioAns[sioPOrder[i]] + "\"";
                    }
                    list += "]";
                    sioSubmit(list);
                }
            } else {
                if (tapIn(24, cy + 60, FB_W - 48, 60)) { sioTyped = keyboard("Type your answer", sioTyped.c_str()); drawJoin(); }
                if (tapIn(24, FB_H - 90, FB_W - 48, 60) && sioTyped.length()) sioSubmit("\"" + sioTyped + "\"");
            }
        }
        drawJoin();
        delay(10);
    }
}

// ------------------------------------------------------------------ setup/loop
void setup() {
    Serial.begin(115200);
    randomSeed(analogRead(0) + millis());
    loadServerPrefs();   // saved server address from NVS

    // touch_gt911_init() initialises I2C and the IO expander internally.
    touch_gt911_init();
    fetchTaskStart();   // background downloads so touch never blocks
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
        case SCR_JOIN: joinLoop(); break;
    }
    delay(10);
}
