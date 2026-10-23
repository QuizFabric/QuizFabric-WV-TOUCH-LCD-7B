/*
 * QuizFabric client for the Waveshare ESP32-S3-Touch-LCD-7B.
 *
 * Inspired by the 3DS port (E:/3DSTHINGY). Features:
 *  - Solo play: quiz list with banners, questions (mc / type-in / slider /
 *    puzzle), scoring identical to the 3DS client (max(0, 1000 - secs*50))
 *  - Steal from Kahoot: search, cover previews, one-tap steal
 *  - Server address configurable on-device
 *
 * GIFs / animated question images are NOT loaded (the ESP32 is too slow
 * for them) - they show a "GIF skipped" placeholder instead.
 *
 * Talks plain HTTP to the QuizFabric server (default: the Pi on the LAN).
 */
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "rgb_lcd_port.h"
#include "i2c.h"
#include "io_extension.h"
#include "gt911.h"
#include "font8x8_basic.h"

// ------------------------------------------------------------------ config
const char *WIFI_SSID = "VM6842809";
const char *WIFI_PASS = "h2hFnrycpprn";

String SERVER_HOST = "192.168.50.65";
int SERVER_PORT = 5710;   // plain HTTP port on the Pi

// ------------------------------------------------------------------ display
static const int FB_W = EXAMPLE_LCD_H_RES;   // 1024
static const int FB_H = EXAMPLE_LCD_V_RES;   // 600
uint16_t *fb = nullptr;

// ------------------------------------------------------------------ drawing
#define RGB(r, g, b) ((uint16_t)((((r) * 31 + 127) / 255) << 11) | \
                       ((((g) * 63 + 127) / 255) << 5) | \
                       (((b) * 31 + 127) / 255))

const uint16_t C_BG     = RGB(24, 16, 40);
const uint16_t C_PANEL  = RGB(46, 30, 70);
const uint16_t C_BTN    = RGB(79, 70, 229);
const uint16_t C_BTN2   = RGB(34, 197, 94);
const uint16_t C_RED    = RGB(220, 38, 38);
const uint16_t C_TEXT   = RGB(255, 255, 255);
const uint16_t C_DIM    = RGB(160, 160, 175);
const uint16_t C_GOLD   = RGB(250, 204, 21);
const uint16_t C_CYAN   = RGB(34, 211, 238);
const uint16_t C_SEL    = RGB(250, 204, 21);
const uint16_t C_GOOD   = RGB(74, 222, 128);
const uint16_t C_BAD    = RGB(248, 113, 113);

void fillRect(int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_W) w = FB_W - x;
    if (y + h > FB_H) h = FB_H - y;
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) fb[yy * FB_W + xx] = c;
    }
}

void drawRect(int x, int y, int w, int h, uint16_t c) {
    fillRect(x, y, w, 1, c);
    fillRect(x, y + h - 1, w, 1, c);
    fillRect(x, y, 1, h, c);
    fillRect(x + w - 1, y, 1, h, c);
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

int drawString(int x, int y, const char *s, uint16_t color, int scale) {
    int cx = x;
    while (*s) {
        if (cx + 8 * scale > FB_W) break;
        drawChar(cx, y, *s, color, scale);
        cx += 8 * scale;
        s++;
    }
    return cx;
}

int textWidth(const char *s, int scale) { return strlen(s) * 8 * scale; }

void drawCentered(int y, const char *s, uint16_t color, int scale) {
    drawString((FB_W - textWidth(s, scale)) / 2, y, s, color, scale);
}

void blit() { wavesahre_rgb_lcd_display((uint8_t *)fb); }

// ------------------------------------------------------------------ touch
touch_gt911_point_t tp;
bool tpActive = false;
int tpX = 0, tpY = 0, tpStartX = 0, tpStartY = 0;
unsigned long tpDownMs = 0;
bool tapConsumed = false;

void touch_step(void) {
    tp = touch_gt911_read_point(1);
    bool active = tp.cnt > 0;
    if (active && !tpActive) {          // press
        tpActive = true;
        tpStartX = tp.x[0];
        tpStartY = tp.y[0];
        tpDownMs = millis();
        tapConsumed = false;
    }
    if (active) { tpX = tp.x[0]; tpY = tp.y[0]; }
    if (!active && tpActive) {          // release
        tpActive = false;
    }
}

bool tapIn(int x, int y, int w, int h) {
    // True on the release edge inside the rect, once.
    if (tapConsumed) return false;
    if (!tpActive && tpX >= x && tpX < x + w && tpY >= y && tpY < y + h) {
        unsigned long held = millis() - tpDownMs;
        tapConsumed = true;
        return held < 800;
    }
    return false;
}

void touch_wait_release(void) { while (touch_gt911_read_point(1).cnt > 0) delay(5); }

// ------------------------------------------------------------------ 565 decode
static const uint8_t PERM[64] = {
    0, 1, 8, 9, 16, 17, 10, 2, 3, 11, 18, 19, 24, 25, 20, 12,
    4, 5, 14, 13, 22, 23, 28, 21, 6, 7, 15, 26, 27, 30, 31, 29,
    32, 33, 40, 41, 48, 49, 42, 34, 35, 43, 50, 51, 56, 57, 52, 44,
    36, 37, 46, 45, 54, 55, 60, 53, 38, 39, 47, 58, 59, 62, 63, 61
};

static int pow2up(int v) {
    int p = 8;
    while (p < v) p <<= 1;
    return p;
}

// Decodes a static .565 into a RGB565 sprite. Returns false for animated
// (GIF) payloads - the ESP32 skips those.
bool decode565(const uint8_t *data, size_t len, uint16_t **spr, int *w, int *h) {
    if (len < 6) return false;
    int iw = data[0] | (data[1] << 8);
    int ih = data[2] | (data[3] << 8);
    if (iw < 1 || ih < 1 || iw > 1024 || ih > 1024) return false;
    int pw = pow2up(iw), ph = pow2up(ih);
    size_t fbSize = (size_t)pw * ph * 4;
    if (len == 4 + fbSize) {
        // static
    } else if (len >= 6) {
        int n = data[4];
        if (n >= 2 && len == 6 + (size_t)n * fbSize) return false;   // animated: skip
        return false;
    } else {
        return false;
    }
    uint16_t *img = (uint16_t *)heap_caps_malloc((size_t)iw * ih * 2, MALLOC_CAP_SPIRAM);
    if (!img) return false;
    for (int by = 0; by < (ph >> 3); by++) {
        for (int bx = 0; bx < (pw >> 3); bx++) {
            size_t base = ((size_t)by * (pw >> 3) + bx) * 64;
            for (int j = 0; j < 64; j++) {
                int s = PERM[j];
                int sx = bx * 8 + (s % 8);
                int sy = by * 8 + (s / 8);
                size_t off = (base + j) * 4;
                uint8_t a = data[4 + off], b = data[4 + off + 1], g = data[4 + off + 2], r = data[4 + off + 3];
                if (sx < iw && sy < ih) {
                    // composite over the bg colour (fillRect handles blending
                    // poorly; simple alpha blend with a dark panel)
                    int bgR = 46, bgG = 30, bgB = 70;
                    r = (r * a + bgR * (255 - a)) / 255;
                    g = (g * a + bgG * (255 - a)) / 255;
                    b = (b * a + bgB * (255 - a)) / 255;
                    img[sy * iw + sx] = RGB(r, g, b);
                }
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

// ------------------------------------------------------------------ HTTP
struct HttpResp { int status; String body; };

HttpResp http_req(const char *method, const String &path, const String &formData = "") {
    HttpResp r = { -1, "" };
    WiFiClient c;
    if (!c.connect(SERVER_HOST.c_str(), SERVER_PORT)) return r;
    c.setTimeout(15000);
    c.print(method);
    c.print(' ');
    c.print(path);
    c.print(" HTTP/1.1\r\nHost: ");
    c.print(SERVER_HOST);
    c.print("\r\nConnection: close\r\n");
    if (formData.length()) {
        c.print("Content-Type: application/x-www-form-urlencoded\r\n");
        c.print("Content-Length: ");
        c.print(formData.length());
        c.print("\r\n\r\n");
        c.print(formData);
    } else {
        c.print("\r\n");
    }
    // skip headers
    String line = "";
    while (c.connected()) {
        String l = c.readStringUntil('\n');
        if (l.startsWith("HTTP/")) r.status = l.substring(9, 12).toInt();
        if (l == "\r" || l == "") break;
    }
    while (c.available()) r.body += (char)c.read();
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

// Fetches a .565 image over HTTP and decodes it into a sprite.
// Returns false for GIFs (skipped) or any failure.
bool fetchImage565(const String &path, uint16_t **spr, int *w, int *h, bool *isGif) {
    HttpResp r = http_req("GET", path);
    *isGif = false;
    if (r.status != 200 || r.body.length() < 6) return false;
    const uint8_t *d = (const uint8_t *)r.body.c_str();
    int n = d[4];
    if (n >= 2) { *isGif = true; return false; }   // animated: never load
    return decode565(d, r.body.length(), spr, w, h);
}

// ------------------------------------------------------------------ keyboard
String keyboard(const char *hint, const char *initial) {
    static const char *rows[3] = {
        "qwertyuiop",
        "asdfghjkl",
        "zxcvbnm1234567890"
    };
    String out = initial ? initial : "";
    bool shift = false;
    while (true) {
        fillRect(0, 0, FB_W, FB_H, C_BG);
        fillRect(40, 30, FB_W - 80, 90, C_PANEL);
        drawRect(40, 30, FB_W - 80, 90, C_DIM);
        drawCentered(40, hint, C_DIM, 1);
        drawCentered(70, out.c_str(), C_TEXT, 2);
        // keys
        int baseY = 150;
        for (int r = 0; r < 3; r++) {
            const char *row = rows[r];
            int n = strlen(row);
            int kw = (FB_W - 80) / (n + 1);
            for (int i = 0; i < n; i++) {
                int kx = 40 + (i + 1) * (kw + 10) / 2;
                int x = 40 + (i + 1) * 10 + i * kw;
                char ch = row[i];
                if (shift && ch >= 'a' && ch <= 'z') ch = toupper(ch);
                fillRect(x, baseY + r * 70, kw, 60, C_BTN);
                char b[2] = { ch, 0 };
                drawString(x + kw / 2 - 4, baseY + r * 70 + 22, b, C_TEXT, 2);
            }
        }
        // controls
        fillRect(40, baseY + 3 * 70 + 10, 240, 60, C_BTN2);
        drawCentered(baseY + 3 * 70 + 32, "SPACE", C_TEXT, 2);
        fillRect(300, baseY + 3 * 70 + 10, 240, 60, C_CYAN);
        drawCentered(baseY + 3 * 70 + 32, shift ? "ABC" : "SHIFT", C_BG, 2);
        fillRect(560, baseY + 3 * 70 + 10, 200, 60, C_RED);
        drawCentered(baseY + 3 * 70 + 32, "DEL", C_TEXT, 2);
        fillRect(780, baseY + 3 * 70 + 10, 200, 60, C_GOOD);
        drawCentered(baseY + 3 * 70 + 32, "DONE", C_BG, 2);
        blit();

        touch_step();
        if (tapIn(40, baseY + 3 * 70 + 10, 240, 60)) { out += ' '; }
        if (tapIn(300, baseY + 3 * 70 + 10, 240, 60)) { shift = !shift; }
        if (tapIn(560, baseY + 3 * 70 + 10, 200, 60)) { if (out.length()) out.remove(out.length() - 1); }
        if (tapIn(780, baseY + 3 * 70 + 10, 200, 60)) { touch_wait_release(); return out; }
        for (int r = 0; r < 3; r++) {
            const char *row = rows[r];
            int n = strlen(row);
            int kw = (FB_W - 80) / (n + 1);
            for (int i = 0; i < n; i++) {
                int x = 40 + (i + 1) * 10 + i * kw;
                if (tapIn(x, baseY + r * 70, kw, 60)) {
                    char ch = row[i];
                    if (shift && ch >= 'a' && ch <= 'z') ch = toupper(ch);
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

// thumbnails for the visible list rows
#define MAX_THUMBS LIST_ROWS
uint16_t *thumbSpr[MAX_THUMBS] = { nullptr };
int thumbW[MAX_THUMBS], thumbH[MAX_THUMBS];
int thumbFor[MAX_THUMBS];

void freeThumbs(void) {
    for (int i = 0; i < MAX_THUMBS; i++) {
        if (thumbSpr[i]) { free(thumbSpr[i]); thumbSpr[i] = nullptr; }
        thumbFor[i] = -1;
    }
}

void queueThumb(int slot, int quizIdx) {
    if (thumbSpr[slot] || thumbFor[slot] == quizIdx) return;
    thumbFor[slot] = quizIdx;
    String enc = urlEncode(quizNames[quizIdx].c_str());
    bool gif = false;
    uint16_t *s = nullptr; int w = 0, h = 0;
    if (fetchImage565("/api/quizbanner/" + enc, &s, &w, &h, &gif) && s) {
        thumbSpr[slot] = s; thumbW[slot] = w; thumbH[slot] = h;
    }
}

void fetchQuizList(void) {
    nQuizzes = 0;
    freeThumbs();
    HttpResp r = http_req("GET", "/api/quizzes");
    if (r.status != 200) { statusLine = "Could not load the quiz list"; return; }
    JsonDocument doc;
    if (deserializeJson(doc, r.body)) { statusLine = "Bad quiz list response"; return; }
    JsonArray arr = doc["quizzes"];
    for (JsonObject q : arr) {
        if (nQuizzes >= MAX_QUIZZES) break;
        quizNames[nQuizzes] = q["name"].as<String>();
        quizHasBanner[nQuizzes] = q["banner"].as<bool>();
        nQuizzes++;
    }
    statusLine = "";
}

void drawMenu(void) {
    fillRect(0, 0, FB_W, FB_H, C_BG);
    drawCentered(80, "QuizFabric", C_GOLD, 4);
    drawCentered(150, String("Server: " + SERVER_HOST + ":" + String(SERVER_PORT)).c_str(), C_DIM, 1);
    fillRect(80, 230, FB_W - 160, 90, C_BTN);
    drawCentered(265, "Play Solo", C_TEXT, 3);
    fillRect(80, 350, FB_W - 160, 90, C_BTN2);
    drawCentered(385, "Steal from Kahoot", C_TEXT, 3);
    fillRect(80, 470, FB_W - 160, 90, C_PANEL);
    drawCentered(505, "Server settings", C_TEXT, 2);
    if (statusLine.length()) {
        drawCentered(FB_H - 40, statusLine.c_str(), C_BAD, 1);
    }
    blit();
}

void menuLoop(void) {
    if (statusLine.length() == 0) statusLine = "";
    drawMenu();
    while (screen == SCR_MENU) {
        touch_step();
        if (tapIn(80, 230, FB_W - 160, 90)) { screen = SCR_LIST; fetchQuizList(); return; }
        if (tapIn(80, 350, FB_W - 160, 90)) { screen = SCR_STEAL; return; }
        if (tapIn(80, 470, FB_W - 160, 90)) { screen = SCR_CONFIG; return; }
        delay(10);
    }
}

// ------------------------------------------------------------------ list
void drawList(void) {
    fillRect(0, 0, FB_W, FB_H, C_BG);
    drawCentered(16, "Choose a quiz", C_TEXT, 2);
    drawCentered(52, "Scroll: drag. Tap a quiz to play it.", C_DIM, 1);
    int y = 84;
    for (int i = 0; i < LIST_ROWS; i++) {
        int qi = listScroll + i;
        if (qi >= nQuizzes) break;
        int rowY = y + i * 62;
        uint16_t rowC = (i % 2) ? C_PANEL : RGB(38, 25, 58);
        fillRect(16, rowY, FB_W - 32, 56, rowC);
        int slot = i;
        if (thumbSpr[slot]) {
            float scale = 56.0f / thumbH[slot];
            int dw = thumbW[slot] * scale;
            if (dw > 96) dw = 96;
            drawSprite(20, rowY + 4, thumbSpr[slot], thumbW[slot], thumbH[slot]);
        } else {
            fillRect(20, rowY + 4, 48, 48, C_DIM);
            drawCentered(rowY + 22, "?", C_BG, 2);
        }
        drawString(140, rowY + 18, quizNames[qi].c_str(), C_TEXT, 2);
    }
    blit();
}

void listLoop(void) {
    drawList();
    int dragStartY = 0;
    bool dragging = false;
    while (screen == SCR_LIST) {
        touch_step();
        // tap a row
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
        // drag scroll
        if (tpActive) {
            if (!dragging) { dragging = true; dragStartY = tpY; }
            int dy = tpY - dragStartY;
            if (dy < -30) { listScroll += 3; dragStartY = tpY; }
            if (dy > 30) { listScroll -= 3; dragStartY = tpY; }
            if (listScroll < 0) listScroll = 0;
            if (listScroll > nQuizzes - LIST_ROWS) listScroll = nQuizzes - LIST_ROWS;
            if (listScroll < 0) listScroll = 0;
            drawList();
            // queue thumbnails for the visible rows
            for (int i = 0; i < LIST_ROWS; i++) {
                int qi = listScroll + i;
                if (qi >= 0 && qi < nQuizzes && quizHasBanner[qi] && !thumbSpr[i] && thumbFor[i] != qi)
                    queueThumb(i, qi);
            }
        } else {
            if (dragging) { dragging = false; drawList(); }
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
    int qtype;            // 0 mc, 1 type, 2 slider, 3 puzzle
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

void freeQuestionImage(void) {
    if (qImgSpr) { free(qImgSpr); qImgSpr = nullptr; }
}

void loadQuestionImage(const String &url) {
    freeQuestionImage();
    qImgGif = false;
    // url is like /UploadedImages/<quiz>/<file> - the 3DS format swaps ext to .565
    String path = url + ".565";
    fetchImage565(path, &qImgSpr, &qImgW, &qImgH, &qImgGif);
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
    if (qs) free(qs);
    qs = (QuizQ *)malloc(sizeof(QuizQ) * nQ);
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
    if (q->imgUrl.length()) loadQuestionImage(q->imgUrl);
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
}

void drawPlay(void) {
    fillRect(0, 0, FB_W, FB_H, C_BG);
    QuizQ *q = &qs[qi];
    char hdr[64];
    snprintf(hdr, 64, "Q%d/%d   Score: %d", qi + 1, nQ, score);
    drawString(16, 12, hdr, C_DIM, 1);

    // question text (wrapped naively by words)
    String line;
    int y = 40;
    drawString(16, y, q->question.c_str(), C_TEXT, 2);
    y += 48;

    // image
    if (qImgGif) {
        drawCentered(y + 20, "GIF skipped", C_DIM, 2);
        y += 56;
    } else if (qImgSpr) {
        float sc = 1.0f;
        int maxw = FB_W - 32, maxh = 220;
        if (qImgW > maxw) sc = (float)maxw / qImgW;
        if (qImgH * sc > maxh) sc = (float)maxh / qImgH;
        int dw = qImgW * sc, dh = qImgH * sc;
        drawSprite((FB_W - dw) / 2, y, qImgSpr, qImgW, qImgH);
        y += dh + 8;
    }

    if (revealing) {
        fillRect(16, y, FB_W - 32, 70, lastRight ? C_GOOD : C_BAD);
        char txt[80];
        snprintf(txt, 80, lastRight ? "Correct! +%d" : "Nope", lastGain);
        drawCentered(y + 24, txt, C_BG, 2);
        if (!lastRight) {
            String correct = q->qtype == 0 ? q->answers[q->correct[0] == 1 ? 0 : 0] : "";
            if (q->qtype == 0) {
                for (int k = 0; k < q->nAnswers; k++)
                    if (q->correct[k]) { correct = q->answers[k]; break; }
            } else if (q->qtype == 1) correct = q->typeAnswer;
            else if (q->qtype == 2) correct = String(q->sAnswer);
            else correct = "(the listed order)";
            drawCentered(y + 50, String("Answer: " + correct).c_str(), C_BG, 1);
        }
        fillRect(16, FB_H - 70, FB_W - 32, 54, C_BTN);
        drawCentered(FB_H - 46, qi + 1 < nQ ? "Next" : "Finish", C_TEXT, 2);
        blit();
        return;
    }

    if (q->qtype == 0) {
        int bh = (FB_H - y - 90) / q->nAnswers;
        if (bh > 64) bh = 64;
        for (int i = 0; i < q->nAnswers; i++) {
            int by = y + i * (bh + 6);
            uint16_t c = (selAns == i) ? C_SEL : C_BTN;
            fillRect(16, by, FB_W - 32, bh, c);
            drawString(32, by + bh / 2 - 8, q->answers[i].c_str(), C_TEXT, 2);
        }
        fillRect(16, FB_H - 70, FB_W - 32, 54, C_BTN2);
        drawCentered(FB_H - 46, "Submit", C_BG, 2);
    } else if (q->qtype == 1) {
        drawString(16, y, String("Your answer: " + typed).c_str(), C_TEXT, 2);
        fillRect(16, y + 40, FB_W - 32, 54, C_BTN2);
        drawCentered(y + 56, "Type answer", C_BG, 2);
        fillRect(16, y + 106, FB_W - 32, 54, C_BTN2);
        drawCentered(y + 122, "Submit", C_BG, 2);
    } else if (q->qtype == 2) {
        char txt[64];
        snprintf(txt, 64, "%.1f  (%.1f - %.1f)", sVal, q->sMin, q->sMax);
        drawCentered(y + 10, txt, C_TEXT, 2);
        fillRect(60, y + 60, FB_W - 120, 40, C_PANEL);
        drawRect(60, y + 60, FB_W - 120, 40, C_DIM);
        float t = (sVal - q->sMin) / (q->sMax - q->sMin);
        fillRect(60, y + 60, (FB_W - 120) * t, 40, C_GOLD);
        fillRect(16, FB_H - 70, FB_W - 32, 54, C_BTN2);
        drawCentered(FB_H - 46, "Submit", C_BG, 2);
    } else if (q->qtype == 3) {
        int rh = (FB_H - y - 90) / q->nAnswers;
        if (rh > 40) rh = 40;
        for (int i = 0; i < q->nAnswers; i++) {
            int ry = y + i * (rh + 5);
            fillRect(16, ry, FB_W - 32, rh, C_BTN);
            drawString(32, ry + rh / 2 - 8, q->answers[pOrder[i]].c_str(), C_TEXT, 2);
            fillRect(FB_W - 70, ry, 54, rh, C_PANEL);
            drawCentered(ry + rh / 2 - 8, "^", C_TEXT, 2);
        }
        fillRect(16, FB_H - 70, FB_W - 32, 54, C_BTN2);
        drawCentered(FB_H - 46, "Submit", C_BG, 2);
    }
    blit();
}

void playLoop(void) {
    fillRect(0, 0, FB_W, FB_H, C_BG);
    drawCentered(FB_H / 2 - 20, "Loading quiz...", C_TEXT, 2);
    blit();
    if (!fetchQuizQuestions()) {
        statusLine = "Could not load that quiz";
        screen = SCR_MENU;
        return;
    }
    qi = 0;
    score = 0;
    startQuestion();
    while (screen == SCR_PLAY) {
        touch_step();
        QuizQ *q = &qs[qi];
        if (revealing) {
            if (tapIn(16, FB_H - 70, FB_W - 32, 54)) {
                if (qi + 1 < nQ) { qi++; startQuestion(); }
                else {
                    freeQuestionImage();
                    statusLine = "Quiz complete! Score: " + String(score);
                    screen = SCR_MENU;
                    return;
                }
            }
            drawPlay();
            delay(10);
            continue;
        }
        bool dirty = false;
        if (q->qtype == 0) {
            int bh = (FB_H - 170 - 40) / q->nAnswers;
            if (bh > 64) bh = 64;
            for (int i = 0; i < q->nAnswers; i++) {
                int by = 170 + i * (bh + 6);
                if (tapIn(16, by, FB_W - 32, bh)) { selAns = i; dirty = true; }
            }
            if (tapIn(16, FB_H - 70, FB_W - 32, 54)) {
                if (selAns >= 0) submit();
            }
        } else if (q->qtype == 1) {
            if (tapIn(16, 170 + 40, FB_W - 32, 54)) { typed = keyboard("Type your answer", typed.c_str()); dirty = true; }
            if (tapIn(16, 170 + 106, FB_W - 32, 54)) { submit(); }
        } else if (q->qtype == 2) {
            if (tpActive && tpX >= 60 && tpX <= FB_W - 60 && tpY >= 170 + 60 && tpY <= 170 + 100) {
                float t = (float)(tpX - 60) / (FB_W - 120);
                sVal = q->sMin + t * (q->sMax - q->sMin);
                dirty = true;
            }
            if (tapIn(16, FB_H - 70, FB_W - 32, 54)) submit();
        } else if (q->qtype == 3) {
            int rh = (FB_H - 170 - 90) / q->nAnswers;
            if (rh > 40) rh = 40;
            for (int i = 1; i < q->nAnswers; i++) {
                int ry = 170 + i * (rh + 5);
                if (tapIn(FB_W - 70, ry, 54, rh)) {
                    int t = pOrder[i]; pOrder[i] = pOrder[i - 1]; pOrder[i - 1] = t;
                    dirty = true;
                }
            }
            if (tapIn(16, FB_H - 70, FB_W - 32, 54)) submit();
        }
        if (dirty) drawPlay();
        delay(10);
    }
}

// ------------------------------------------------------------------ steal
#define MAX_RESULTS 20
String sUuid[MAX_RESULTS];
String sTitle[MAX_RESULTS];
String sCover[MAX_RESULTS];
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
    if (r.status != 200) { stealMsg = "Search failed"; return; }
    JsonDocument doc;
    if (deserializeJson(doc, r.body)) { stealMsg = "Bad response"; return; }
    for (JsonObject res : doc["results"].as<JsonArray>()) {
        if (nResults >= MAX_RESULTS) break;
        sUuid[nResults] = res["uuid"].as<String>();
        sTitle[nResults] = res["title"].as<String>();
        sCover[nResults] = res["cover"].as<String>();
        nResults++;
    }
    if (nResults == 0) stealMsg = "No public quizzes found";
    else stealMsg = "";
    // fetch covers
    for (int i = 0; i < nResults; i++) {
        String p = "/api/kahootcover?id=" + urlEncode(sUuid[i].c_str())
                 + "&url=" + urlEncode(sCover[i].c_str()) + "&size=64";
        bool gif = false;
        fetchImage565(p, &sCovSpr[i], &sCovW[i], &sCovH[i], &gif);
    }
}

void drawSteal(void) {
    fillRect(0, 0, FB_W, FB_H, C_BG);
    drawCentered(16, "Steal from Kahoot", C_TEXT, 2);
    fillRect(16, 56, FB_W - 220, 54, C_PANEL);
    drawString(28, 72, stealSearch.c_str(), C_TEXT, 2);
    fillRect(FB_W - 190, 56, 174, 54, C_BTN);
    drawCentered(74, "Search", C_TEXT, 2);
    fillRect(16, 122, FB_W - 32, 40, C_PANEL);
    drawString(28, 130, "Tap a result to steal it.", C_DIM, 1);
    for (int i = 0; i < nResults && i < 8; i++) {
        int ry = 170 + i * 56;
        fillRect(16, ry, FB_W - 32, 50, (i % 2) ? C_PANEL : RGB(38, 25, 58));
        if (sCovSpr[i]) {
            float sc = 44.0f / sCovH[i];
            drawSprite(20, ry + 3, sCovSpr[i], sCovW[i], sCovH[i]);
        } else {
            fillRect(20, ry + 3, 44, 44, C_DIM);
            drawCentered(ry + 18, "?", C_BG, 2);
        }
        drawString(76, ry + 16, sTitle[i].c_str(), C_TEXT, 2);
    }
    if (stealMsg.length()) drawCentered(FB_H - 40, stealMsg.c_str(), C_GOLD, 1);
    blit();
}

void stealLoop(void) {
    stealMsg = "";
    drawSteal();
    while (screen == SCR_STEAL) {
        touch_step();
        if (tapIn(16, 56, FB_W - 220, 54)) {
            stealSearch = keyboard("Search Kahoot", stealSearch.c_str());
            stealSearchNow();
            drawSteal();
        }
        if (tapIn(FB_W - 190, 56, 174, 54)) {
            if (stealSearch.length()) { stealSearchNow(); drawSteal(); }
        }
        for (int i = 0; i < nResults && i < 8; i++) {
            int ry = 170 + i * 56;
            if (tapIn(16, ry, FB_W - 32, 50)) {
                fillRect(0, 0, FB_W, FB_H, C_BG);
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
                    stealMsg = "Steal failed";
                }
                drawSteal();
            }
        }
        delay(10);
    }
}

// ------------------------------------------------------------------ config
void configLoop(void) {
    fillRect(0, 0, FB_W, FB_H, C_BG);
    drawCentered(80, "Server settings", C_TEXT, 2);
    drawCentered(160, String("Current: " + SERVER_HOST + ":" + String(SERVER_PORT)).c_str(), C_DIM, 1);
    fillRect(80, 240, FB_W - 160, 90, C_BTN);
    drawCentered(275, "Change address", C_TEXT, 2);
    fillRect(80, 370, FB_W - 160, 90, C_BTN2);
    drawCentered(405, "Back", C_BG, 2);
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
            fillRect(0, 0, FB_W, FB_H, C_BG);
            drawCentered(160, String("Current: " + SERVER_HOST + ":" + String(SERVER_PORT)).c_str(), C_DIM, 1);
            fillRect(80, 240, FB_W - 160, 90, C_BTN);
            drawCentered(275, "Change address", C_TEXT, 2);
            fillRect(80, 370, FB_W - 160, 90, C_BTN2);
            drawCentered(405, "Back", C_BG, 2);
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

    // touch_gt911_init() initialises I2C and the IO expander internally -
    // calling DEV_I2C_Init() again would double-acquire the bus and crash.
    touch_gt911_init();
    waveshare_esp32_s3_rgb_lcd_init();
    wavesahre_rgb_lcd_bl_on();

    fb = (uint16_t *)heap_caps_malloc(FB_W * FB_H * 2, MALLOC_CAP_SPIRAM);
    memset(fb, 0, FB_W * FB_H * 2);

    fillRect(0, 0, FB_W, FB_H, C_BG);
    drawCentered(FB_H / 2 - 30, "QuizFabric", C_GOLD, 3);
    drawCentered(FB_H / 2 + 20, "connecting to wifi...", C_DIM, 1);
    blit();

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    for (int i = 0; WiFi.status() != WL_CONNECTED && i < 40; i++) delay(500);
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
