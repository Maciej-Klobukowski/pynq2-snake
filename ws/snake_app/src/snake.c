/*  snake.c - PYNQ2-Snake: FreeRTOS + lwIP (2x UDP) + VDMA framebuffer -> HDMI
 *
 *  Speler 1: UDP poort 5000 (groen)   Speler 2: UDP poort 5001 (oranje)
 *  Commando's (1 teken per pakket, meerdere mag ook):
 *     U = omhoog  D = omlaag  L = links  R = rechts  N = terug naar menu
 *     1 = 1 speler  2 = 2 spelers (in het menu werkt L/R ook)
 *     J = "ik ben verbonden" (heartbeat, de controller stuurt dit elke seconde)
 *     X = "ik ga weg"
 *
 *  2 spelers: het spel start pas als BEIDE spelers verbonden zijn (wachtscherm).
 *  Valt iemand weg (> 3 s niets gehoord), dan gaat het spel terug naar het
 *  wachtscherm.
 *
 *  Spelregels:
 *   - Eet je een appel: +1 punt, 1 cel langer en je slang DRAAIT OM
 *     (de kop wordt de staart en je gaat de andere kant op).
 *   - 2 spelers: bots je, dan kom je even later terug (punten blijven).
 *     Wie eerst WIN_SCORE appels heeft, wint.
 *   - 1 speler: bots je, dan is het game over. Haal zoveel mogelijk appels.
 *
 *  main.c (template "FreeRTOS lwIP UDP Perf Server") start het netwerk en
 *  roept daarna print_app_header() en start_application() hieronder aan.
 */
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "lwipopts.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "xparameters.h"
#include "xaxivdma.h"
#include "xil_cache.h"
#include "xil_printf.h"

/* ================= beeld ================= */
#define W       1280
#define H       720
#define BPP     3
#define STRIDE  (W * BPP)

/* ================= raster ================ */
#define CELL    40
#define COLS    (W / CELL)          /* 32 */
#define ROWS    (H / CELL)          /* 18 (rij 0 = scorebalk) */

/* ================= spel ================== */
#define UDP_PORT_P1   5000
#define UDP_PORT_P2   5001
#define TICK_MS       150           /* snelheid: 1 stap per 150 ms */
#define START_LEN     3
#define WIN_SCORE     3             /* 2 spelers: eerste tot 3 appels wint */
#define EAT_FREEZE    2             /* ticks pauze na omdraaien (reactietijd) */
#define RESPAWN_TICKS 6             /* ticks voor je terugkomt na een botsing */
#define MAXLEN        (COLS * ROWS)
#define TIMEOUT_MS    3000          /* speler "weg" na 3 s zonder pakket */
#define DISCOVERY_PORT 5050         /* hier roept het board "PYNQSNAKE" (broadcast) */
#define DISCOVERY_MSG  "PYNQSNAKE"

enum { EMPTY = 0, WALL, FOOD, SNAKE1, SNAKE2 };
enum { DIR_UP = 0, DIR_DOWN, DIR_LEFT, DIR_RIGHT };
enum { ST_MENU = 0, ST_LOBBY, ST_READY, ST_PLAY, ST_OVER };
static const int DX[4] = { 0, 0, -1, 1 };
static const int DY[4] = { -1, 1, 0, 0 };

typedef struct { u8 r, g, b; } rgb_t;

/* ================= kleuren ================= */
static const rgb_t BG_A    = { 16, 20, 28 };    /* dambord achtergrond */
static const rgb_t BG_B    = { 22, 27, 38 };
static const rgb_t BRICK   = { 150, 70, 45 };   /* muur */
static const rgb_t MORTAR  = { 90, 40, 28 };
static const rgb_t PANEL   = { 10, 12, 18 };    /* scorebalk / vensters */
static const rgb_t ACCENT  = { 60, 70, 90 };
static const rgb_t WHITE   = { 235, 235, 235 };
static const rgb_t GREY    = { 170, 170, 180 };
static const rgb_t BLACK   = { 0, 0, 0 };
static const rgb_t RED     = { 230, 50, 50 };

typedef struct {
    int x[MAXLEN], y[MAXLEN];       /* x[0],y[0] = kop */
    int len, dir, next_dir, score, alive, freeze;
    int sx, sy, sdir;               /* startpositie + richting */
    rgb_t body, dark, head;         /* kleuren */
} snake_t;

typedef struct {
    u8   player;                    /* 0 of 1 */
    char cmd;                       /* 'U','D','L','R','N','1','2' */
} cmd_t;

static u8 fb[H * STRIDE] __attribute__((aligned(64)));
static XAxiVdma vdma;
static u8 grid[ROWS][COLS];
static snake_t sn[2];
static QueueHandle_t cmdq;
static int state = ST_MENU;
static int nplayers = 2;
static int solo_player = 0;         /* wie in solo-modus stuurt (poort 5000 of 5001) */
static u32 seed = 12345;

/* verbindingsstatus, bijgewerkt door de UDP-taken */
static volatile TickType_t last_seen[2];
static volatile int seen[2];
static int lobby_shown[2] = { -1, -1 };   /* wat staat er nu op het wachtscherm */

/* ============================================================
 *  Basis-tekenen
 * ============================================================ */
static inline void put_px(int x, int y, rgb_t c)
{
    u8 *p = &fb[y * STRIDE + x * BPP];
    p[0] = c.g; p[1] = c.b; p[2] = c.r;     /* volgorde G, B, R */
}

static void fill_rect(int x0, int y0, int w, int h, rgb_t c)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            put_px(x, y, c);
}

static void flush_rect(int x0, int y0, int w, int h)
{
    for (int y = y0; y < y0 + h; y++)
        Xil_DCacheFlushRange((UINTPTR)&fb[y * STRIDE + x0 * BPP], w * BPP);
}

static void flush_cell(int cx, int cy)
{
    flush_rect(cx * CELL, cy * CELL, CELL, CELL);
}

/* kader: rand van 'b' pixels in kleur 'edge', binnenkant 'fill' */
static void draw_box(int x, int y, int w, int h, int b, rgb_t edge, rgb_t fill)
{
    fill_rect(x, y, w, h, edge);
    fill_rect(x + b, y + b, w - 2 * b, h - 2 * b, fill);
}

/* ============================================================
 *  Mini-lettertype 5x7 (alleen de tekens die we gebruiken)
 * ============================================================ */
typedef struct { char c; u8 rows[7]; } glyph_t;
static const glyph_t FONT[] = {
    {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}}, {'3',{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, {'5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}, {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, {'9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, {'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, {'.',{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
    {'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}}, {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}}, {'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},
    {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}}, {'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, {'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}}, {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}}, {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}}, {'W',{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
    {'!',{0x04,0x04,0x04,0x04,0x04,0x00,0x04}}, {':',{0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}},
};

static const u8 *glyph(char c)
{
    for (unsigned i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++)
        if (FONT[i].c == c) return FONT[i].rows;
    return NULL;                                /* spatie of onbekend */
}

static int text_w(const char *s, int scale)
{
    int n = (int)strlen(s);
    return n ? n * 6 * scale - scale : 0;
}

static void draw_text(int x, int y, const char *s, int scale, rgb_t c)
{
    for (; *s; s++, x += 6 * scale) {
        const u8 *g = glyph(*s);
        if (!g) continue;
        for (int r = 0; r < 7; r++)
            for (int col = 0; col < 5; col++)
                if (g[r] & (0x10 >> col))
                    fill_rect(x + col * scale, y + r * scale, scale, scale, c);
    }
}

/* tekst horizontaal gecentreerd rond cx */
static void draw_text_c(int cx, int y, const char *s, int scale, rgb_t c)
{
    draw_text(cx - text_w(s, scale) / 2, y, s, scale, c);
}

/* getal -> tekst achter een prefix, bv. "SCORE: " + 12 */
static void num_str(char *out, const char *prefix, int v)
{
    char tmp[12];
    int n = 0;
    strcpy(out, prefix);
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    int len = (int)strlen(out);
    while (n) out[len++] = tmp[--n];
    out[len] = '\0';
}

/* ============================================================
 *  Spelobjecten tekenen
 * ============================================================ */
static void cell_bg(int cx, int cy)
{
    fill_rect(cx * CELL, cy * CELL, CELL, CELL, ((cx + cy) & 1) ? BG_B : BG_A);
}

static void draw_empty(int cx, int cy)
{
    cell_bg(cx, cy);
    flush_cell(cx, cy);
}

/* bakstenen muur */
static void draw_wall(int cx, int cy)
{
    int x = cx * CELL, y = cy * CELL;
    int off = (cy & 1) ? 20 : 0;
    fill_rect(x, y, CELL, CELL, BRICK);
    fill_rect(x, y, CELL, 2, MORTAR);
    fill_rect(x, y + 20, CELL, 2, MORTAR);
    fill_rect(x + off, y, 2, 20, MORTAR);
    fill_rect(x + (off + 20) % 40, y + 20, 2, 20, MORTAR);
    flush_cell(cx, cy);
}

/* appel in een vierkant van 'size' pixels (filled=0 -> donker "leeg" slot) */
static void draw_apple_at(int x0, int y0, int size, int filled)
{
    int cxp = x0 + size / 2, cyp = y0 + size / 2 + size / 12;
    int r = size * 7 / 20, hr = r / 4;
    rgb_t red   = filled ? (rgb_t){ 220, 30, 40 }   : (rgb_t){ 55, 30, 35 };
    rgb_t shine = filled ? (rgb_t){ 255, 170, 170 } : red;
    rgb_t stem  = filled ? (rgb_t){ 110, 70, 30 }   : (rgb_t){ 50, 40, 30 };
    rgb_t leaf  = filled ? (rgb_t){ 60, 190, 60 }   : (rgb_t){ 30, 60, 30 };

    for (int y = y0; y < y0 + size; y++)
        for (int x = x0; x < x0 + size; x++) {
            int dx = x - cxp, dy = y - cyp;
            if (dx * dx + dy * dy <= r * r) {
                int sx = dx + r / 3, sy = dy + r / 3;
                put_px(x, y, (sx * sx + sy * sy <= hr * hr) ? shine : red);
            }
        }
    fill_rect(cxp - 1, cyp - r - size / 8, 3, size / 8 + 2, stem);
    fill_rect(cxp + 2, cyp - r - size / 10, size / 6, size / 12 + 1, leaf);
}

static void draw_apple(int cx, int cy)
{
    cell_bg(cx, cy);
    draw_apple_at(cx * CELL, cy * CELL, CELL, 1);
    flush_cell(cx, cy);
}

/* lichaamsdeel op pixelpositie (afgeronde look) */
static void draw_body_px(int x, int y, rgb_t c)
{
    fill_rect(x + 3, y + 5, CELL - 6, CELL - 10, c);
    fill_rect(x + 5, y + 3, CELL - 10, CELL - 6, c);
}

/* kop op pixelpositie, met oogjes die in de looprichting kijken */
static void draw_head_px(int x, int y, int dir, rgb_t c)
{
    int ex[2], ey[2];

    fill_rect(x + 2, y + 4, CELL - 4, CELL - 8, c);
    fill_rect(x + 4, y + 2, CELL - 8, CELL - 4, c);

    switch (dir) {
    case DIR_UP:    ex[0] = x + 8;  ey[0] = y + 6;  ex[1] = x + 24; ey[1] = y + 6;  break;
    case DIR_DOWN:  ex[0] = x + 8;  ey[0] = y + 26; ex[1] = x + 24; ey[1] = y + 26; break;
    case DIR_LEFT:  ex[0] = x + 6;  ey[0] = y + 8;  ex[1] = x + 6;  ey[1] = y + 24; break;
    default:        ex[0] = x + 26; ey[0] = y + 8;  ex[1] = x + 26; ey[1] = y + 24; break;
    }
    for (int i = 0; i < 2; i++) {
        fill_rect(ex[i], ey[i], 8, 8, WHITE);
        fill_rect(ex[i] + 2 + 2 * DX[dir], ey[i] + 2 + 2 * DY[dir], 4, 4, BLACK);
    }
}

static void draw_body(int p, int cx, int cy)
{
    snake_t *s = &sn[p];
    cell_bg(cx, cy);
    draw_body_px(cx * CELL, cy * CELL, ((cx + cy) & 1) ? s->dark : s->body);
    flush_cell(cx, cy);
}

static void draw_head(int p, int cx, int cy, int dir)
{
    cell_bg(cx, cy);
    draw_head_px(cx * CELL, cy * CELL, dir, sn[p].head);
    flush_cell(cx, cy);
}

/* klein slangetje (voor het menu): 'n' delen naar rechts */
static void draw_mini_snake(int x, int y, int n, int p)
{
    for (int i = 0; i < n - 1; i++)
        draw_body_px(x + i * CELL, y, (i & 1) ? sn[p].dark : sn[p].body);
    draw_head_px(x + (n - 1) * CELL, y, DIR_RIGHT, sn[p].head);
}

/* ============================================================
 *  Schermen: scorebalk, menu, einde
 * ============================================================ */
static void draw_panel(void)
{
    char mid[24];

    fill_rect(0, 0, W, CELL, PANEL);
    fill_rect(0, CELL - 3, W, 3, ACCENT);

    /* speler 1 links */
    draw_text(12, 6, "P1", 4, sn[0].head);

    if (nplayers == 2) {
        for (int i = 0; i < WIN_SCORE; i++)
            draw_apple_at(12 + 56 + i * 36, 1, 36, i < sn[0].score);

        draw_text(W - 12 - text_w("P2", 4), 6, "P2", 4, sn[1].head);
        for (int i = 0; i < WIN_SCORE; i++)
            draw_apple_at(W - 12 - 56 - (i + 1) * 36, 1, 36, i < sn[1].score);
    } else {
        draw_text(W - 12 - text_w("SOLO", 4), 6, "SOLO", 4, GREY);
    }

    /* midden: status */
    if (state == ST_OVER)
        strcpy(mid, "GAME OVER");
    else if (state == ST_READY)
        strcpy(mid, "DRUK EEN RICHTING");
    else if (nplayers == 2) {
        strcpy(mid, "EERSTE TOT X");
        mid[11] = (char)('0' + WIN_SCORE);
    } else
        num_str(mid, "SCORE: ", sn[0].score);
    draw_text_c(W / 2, 6, mid, 4, GREY);

    flush_rect(0, 0, W, CELL);
}

/* achtergrond met bakstenen rand (menu + wachtscherm) */
static void draw_frame_bg(void)
{
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++)
            cell_bg(x, y);
    for (int x = 0; x < COLS; x++) { draw_wall(x, 0); draw_wall(x, ROWS - 1); }
    for (int y = 1; y < ROWS - 1; y++) { draw_wall(0, y); draw_wall(COLS - 1, y); }
}

static void draw_menu(void)
{
    int bw = 400, bh = 270, by = 280;
    int lx = W / 2 - 30 - bw, rx = W / 2 + 30;

    draw_frame_bg();

    /* titel */
    draw_text_c(W / 2 + 6, 96, "SNAKE", 16, BLACK);          /* schaduw */
    draw_text_c(W / 2, 90, "SNAKE", 16, sn[0].head);
    draw_text_c(W / 2, 222, "KIES MODUS", 4, GREY);

    /* links: 1 speler */
    draw_box(lx, by, bw, bh, 6, sn[0].head, PANEL);
    draw_mini_snake(lx + (bw - 4 * CELL) / 2, by + 40, 4, 0);
    draw_text_c(lx + bw / 2, by + 120, "1 SPELER", 5, WHITE);
    draw_text_c(lx + bw / 2, by + 200, "Q OF 1", 4, GREY);

    /* rechts: 2 spelers */
    draw_box(rx, by, bw, bh, 6, sn[1].head, PANEL);
    draw_mini_snake(rx + (bw - 4 * CELL) / 2, by + 20, 4, 0);
    draw_mini_snake(rx + (bw - 4 * CELL) / 2, by + 65, 4, 1);
    draw_text_c(rx + bw / 2, by + 125, "2 SPELERS", 5, WHITE);
    draw_text_c(rx + bw / 2, by + 200, "D OF 2", 4, GREY);

    /* appels als versiering */
    draw_apple_at(W / 2 - 24, by + bh / 2 - 24, 48, 1);
    draw_text_c(W / 2, 600, "N = TERUG NAAR MENU", 3, GREY);

    Xil_DCacheFlushRange((UINTPTR)fb, sizeof(fb));
}

/* wachtscherm: toont per speler of hij verbonden is */
static void draw_lobby(int c0, int c1)
{
    int bw = 400, bh = 270, by = 280;
    int bx[2] = { W / 2 - 30 - bw, W / 2 + 30 };
    int con[2] = { c0, c1 };
    const char *name[2] = { "SPELER 1", "SPELER 2" };
    const char *port[2] = { "POORT 5000", "POORT 5001" };

    draw_frame_bg();
    draw_text_c(W / 2 + 5, 85, "2 SPELERS", 10, BLACK);       /* schaduw */
    draw_text_c(W / 2, 80, "2 SPELERS", 10, WHITE);
    draw_text_c(W / 2, 200, "WACHTEN OP SPELERS...", 4, GREY);

    for (int p = 0; p < 2; p++) {
        rgb_t edge = con[p] ? sn[p].head : ACCENT;
        draw_box(bx[p], by, bw, bh, 6, edge, PANEL);
        if (con[p])
            draw_mini_snake(bx[p] + (bw - 4 * CELL) / 2, by + 30, 4, p);
        draw_text_c(bx[p] + bw / 2, by + 105, name[p], 5, con[p] ? WHITE : GREY);
        draw_text_c(bx[p] + bw / 2, by + 165, con[p] ? "VERBONDEN" : "WACHTEN...", 4,
                    con[p] ? sn[p].head : GREY);
        draw_text_c(bx[p] + bw / 2, by + 220, port[p], 3, GREY);
    }
    draw_text_c(W / 2, 600, "SPEL START AUTOMATISCH", 3, GREY);

    Xil_DCacheFlushRange((UINTPTR)fb, sizeof(fb));
}

/* eindvenster in het midden */
static void draw_end_box(rgb_t edge, const char *title, rgb_t tcol, const char *line2)
{
    int bw = 900, bh = 260, bx = (W - bw) / 2, by = (H - bh) / 2;
    draw_box(bx - 6, by - 6, bw + 12, bh + 12, 6, edge, PANEL);
    draw_text_c(W / 2, by + 40, title, 7, tcol);
    if (line2) draw_text_c(W / 2, by + 125, line2, 5, WHITE);
    draw_text_c(W / 2, by + 200, "DRUK N VOOR MENU", 4, GREY);
    flush_rect(bx - 6, by - 6, bw + 12, bh + 12);
}

static void draw_winner(int p)
{
    char t[] = "SPELER X WINT!";
    t[7] = (char)('1' + p);
    draw_end_box(sn[p].head, t, sn[p].head, NULL);
    /* appels onder de titel */
    int by = (H - 260) / 2;
    for (int i = 0; i < WIN_SCORE; i++)
        draw_apple_at((W - WIN_SCORE * 48) / 2 + i * 48, by + 115, 48, 1);
    flush_rect(0, by + 115, W, 48);
}

static void draw_game_over_solo(void)
{
    char s[24];
    num_str(s, "SCORE: ", sn[0].score);
    draw_end_box(RED, "GAME OVER", RED, s);
}

/* ============================================================
 *  VDMA (zelfde als fase 2)
 * ============================================================ */
static int vdma_start(void)
{
    XAxiVdma_Config *cfg;
    XAxiVdma_DmaSetup rd;
    UINTPTR addr[XAXIVDMA_MAX_FRAMESTORE];

#ifdef SDT
    cfg = XAxiVdma_LookupConfig(XPAR_XAXIVDMA_0_BASEADDR);
#else
    cfg = XAxiVdma_LookupConfig(XPAR_AXI_VDMA_0_DEVICE_ID);
#endif
    if (!cfg) return XST_FAILURE;
    if (XAxiVdma_CfgInitialize(&vdma, cfg, cfg->BaseAddress) != XST_SUCCESS)
        return XST_FAILURE;

    memset(&rd, 0, sizeof(rd));
    rd.VertSizeInput     = H;
    rd.HoriSizeInput     = STRIDE;
    rd.Stride            = STRIDE;
    rd.EnableCircularBuf = 1;
    if (XAxiVdma_DmaConfig(&vdma, XAXIVDMA_READ, &rd) != XST_SUCCESS)
        return XST_FAILURE;

    for (int i = 0; i < vdma.MaxNumFrames; i++)
        addr[i] = (UINTPTR)fb;
    if (XAxiVdma_DmaSetBufferAddr(&vdma, XAXIVDMA_READ, addr) != XST_SUCCESS)
        return XST_FAILURE;

    return XAxiVdma_DmaStart(&vdma, XAXIVDMA_READ);
}

/* ============================================================
 *  Spellogica
 * ============================================================ */
static int rnd(int n)
{
    seed = seed * 1103515245u + 12345u;
    return (int)((seed >> 16) % (u32)n);
}

static void place_food(void)
{
    for (int tries = 0; tries < 1000; tries++) {
        int x = 1 + rnd(COLS - 2);
        int y = 1 + rnd(ROWS - 2);
        if (grid[y][x] == EMPTY) {
            grid[y][x] = FOOD;
            draw_apple(x, y);
            return;
        }
    }
}

/* slang op startpositie zetten (als er plaats is) */
static void spawn_snake(int p)
{
    snake_t *s = &sn[p];
    for (int i = 0; i < START_LEN; i++) {
        int x = s->sx - DX[s->sdir] * i;
        int y = s->sy - DY[s->sdir] * i;
        if (grid[y][x] != EMPTY) return;            /* bezet: volgende tick opnieuw */
    }
    s->len = START_LEN;
    s->dir = s->next_dir = s->sdir;
    s->freeze = 0;
    for (int i = 0; i < START_LEN; i++) {
        s->x[i] = s->sx - DX[s->sdir] * i;
        s->y[i] = s->sy - DY[s->sdir] * i;
        grid[s->y[i]][s->x[i]] = SNAKE1 + p;
        if (i == 0) draw_head(p, s->x[i], s->y[i], s->dir);
        else        draw_body(p, s->x[i], s->y[i]);
    }
    s->alive = 1;
}

static void kill_snake(int p)
{
    snake_t *s = &sn[p];

    if (nplayers == 1) {                            /* solo: game over */
        state = ST_OVER;
        draw_panel();
        draw_game_over_solo();
        xil_printf("GAME OVER - score %d. Druk N voor het menu\r\n", s->score);
        return;
    }

    xil_printf("Speler %d botst! (score blijft %d)\r\n", p + 1, s->score);
    for (int i = 0; i < s->len; i++) {
        grid[s->y[i]][s->x[i]] = EMPTY;
        draw_empty(s->x[i], s->y[i]);
    }
    s->alive = 0;
    s->freeze = RESPAWN_TICKS;
}

/* kop <-> staart wisselen en de andere kant op gaan */
static void reverse_snake(snake_t *s)
{
    for (int i = 0, j = s->len - 1; i < j; i++, j--) {
        int tx = s->x[i], ty = s->y[i];
        s->x[i] = s->x[j]; s->y[i] = s->y[j];
        s->x[j] = tx;      s->y[j] = ty;
    }
    /* nieuwe richting = weg van het lichaam */
    int dx = s->x[0] - s->x[1], dy = s->y[0] - s->y[1];
    for (int d = 0; d < 4; d++)
        if (DX[d] == dx && DY[d] == dy) { s->dir = s->next_dir = d; break; }
}

static void step_snake(int p)
{
    snake_t *s = &sn[p];

    if (s->freeze > 0) { s->freeze--; return; }
    if (!s->alive)     { spawn_snake(p); return; }

    s->dir = s->next_dir;
    int hx = s->x[0], hy = s->y[0];                 /* oude kop */
    int nx = hx + DX[s->dir];
    int ny = hy + DY[s->dir];
    int t  = grid[ny][nx];
    int grow = (t == FOOD);

    /* op je eigen staart stappen mag (die schuift weg) */
    int own_tail = (t == SNAKE1 + p &&
                    nx == s->x[s->len - 1] && ny == s->y[s->len - 1]);

    if (t != EMPTY && t != FOOD && !own_tail) { kill_snake(p); return; }

    if (grow) {
        if (s->len < MAXLEN) s->len++;
        s->score++;
    } else {
        int tx = s->x[s->len - 1], ty = s->y[s->len - 1];
        grid[ty][tx] = EMPTY;                       /* staart weg */
        draw_empty(tx, ty);
    }

    memmove(&s->x[1], &s->x[0], (s->len - 1) * sizeof(int));
    memmove(&s->y[1], &s->y[0], (s->len - 1) * sizeof(int));
    s->x[0] = nx;
    s->y[0] = ny;
    grid[ny][nx] = SNAKE1 + p;

    if (!grow) {
        draw_body(p, hx, hy);                       /* oude kop wordt lichaam */
        draw_head(p, nx, ny, s->dir);
        return;
    }

    /* appel gegeten: omdraaien! */
    reverse_snake(s);
    draw_body(p, nx, ny);                           /* waar we net aten = nu staart */
    draw_body(p, hx, hy);
    draw_head(p, s->x[0], s->y[0], s->dir);         /* oude staart = nieuwe kop */
    s->freeze = EAT_FREEZE;
    xil_printf("Speler %d eet een appel! Score %d - omgedraaid\r\n", p + 1, s->score);

    if (nplayers == 2 && s->score >= WIN_SCORE) {
        state = ST_OVER;
        draw_panel();
        draw_winner(p);
        xil_printf("*** SPELER %d WINT! Druk N voor het menu ***\r\n", p + 1);
        return;
    }
    draw_panel();
    place_food();
}

static void show_menu(void)
{
    state = ST_MENU;
    draw_menu();
    xil_printf("Menu: kies 1 speler (Q/1) of 2 spelers (D/2)\r\n");
}

static void start_game(int players)
{
    nplayers = players;
    memset(grid, EMPTY, sizeof(grid));
    state = ST_READY;

    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++)
            cell_bg(x, y);

    for (int x = 0; x < COLS; x++) {
        grid[0][x] = WALL;                          /* rij 0 = scorebalk */
        grid[ROWS - 1][x] = WALL;
        draw_wall(x, ROWS - 1);
    }
    for (int y = 1; y < ROWS - 1; y++) {
        grid[y][0] = grid[y][COLS - 1] = WALL;
        draw_wall(0, y);
        draw_wall(COLS - 1, y);
    }

    for (int p = 0; p < 2; p++) {
        sn[p].score = 0;
        sn[p].alive = 0;
        sn[p].freeze = 0;
    }
    for (int p = 0; p < nplayers; p++)
        spawn_snake(p);
    place_food();
    draw_panel();

    Xil_DCacheFlushRange((UINTPTR)fb, sizeof(fb));
    if (nplayers == 2)
        xil_printf("2 spelers - eerste tot %d appels wint. Druk een richting!\r\n", WIN_SCORE);
    else
        xil_printf("1 speler - haal zoveel mogelijk appels. Druk een richting!\r\n");
}

/* is speler p de laatste TIMEOUT_MS nog gehoord? */
static int is_connected(int p)
{
    return seen[p] &&
           (xTaskGetTickCount() - last_seen[p]) < pdMS_TO_TICKS(TIMEOUT_MS);
}

/* naar het wachtscherm (2 spelers) */
static void enter_lobby(void)
{
    nplayers = 2;
    state = ST_LOBBY;
    lobby_shown[0] = is_connected(0);
    lobby_shown[1] = is_connected(1);
    draw_lobby(lobby_shown[0], lobby_shown[1]);
    xil_printf("Wachten op spelers: P1 %s, P2 %s\r\n",
               lobby_shown[0] ? "verbonden" : "-", lobby_shown[1] ? "verbonden" : "-");
}

static void handle_cmd(cmd_t c)
{
    snake_t *s;
    int d;

    if (c.cmd == 'N') { show_menu(); return; }

    if (state == ST_MENU) {                         /* keuzescherm */
        if (c.cmd == '1' || c.cmd == 'L') {
            solo_player = c.player;                 /* wie kiest, speelt */
            start_game(1);
        } else if (c.cmd == '2' || c.cmd == 'R') {
            enter_lobby();
        }
        return;
    }
    if (state == ST_OVER || state == ST_LOBBY) return;

    if (nplayers == 1) {                            /* solo: alleen de kiezer stuurt */
        if (c.player != solo_player) return;
        s = &sn[0];
    } else {
        s = &sn[c.player];
    }

    switch (c.cmd) {
    case 'U': d = DIR_UP;    break;
    case 'D': d = DIR_DOWN;  break;
    case 'L': d = DIR_LEFT;  break;
    case 'R': d = DIR_RIGHT; break;
    default:  return;
    }

    /* niet 180 graden omkeren */
    if (DX[d] == -DX[s->dir] && DY[d] == -DY[s->dir]) return;
    s->next_dir = d;

    if (state == ST_READY) {
        state = ST_PLAY;
        draw_panel();
    }
}

/* ============================================================
 *  FreeRTOS-taken
 * ============================================================ */
static void game_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    cmd_t c;

    for (;;) {
        while (xQueueReceive(cmdq, &c, 0) == pdTRUE)
            handle_cmd(c);

        if (nplayers == 2 && state != ST_MENU) {
            int c0 = is_connected(0), c1 = is_connected(1);

            if (state == ST_LOBBY) {
                if (c0 != lobby_shown[0] || c1 != lobby_shown[1]) {
                    lobby_shown[0] = c0;
                    lobby_shown[1] = c1;
                    draw_lobby(c0, c1);             /* status veranderd */
                }
                if (c0 && c1) {
                    xil_printf("Beide spelers verbonden - spel start!\r\n");
                    start_game(2);
                }
            } else if (state != ST_OVER && !(c0 && c1)) {
                xil_printf("Speler %d is weg - terug naar wachtscherm\r\n", c0 ? 2 : 1);
                enter_lobby();
            }
        }

        if (state == ST_PLAY) {
            step_snake(0);
            if (state == ST_PLAY && nplayers == 2)  /* speler 1 kan net gewonnen hebben */
                step_snake(1);
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_MS));
    }
}

/* Discovery: elke seconde een broadcast "PYNQSNAKE" op poort 5050.
 * Controllers (andere PYNQ's of de pc) luisteren hierop en halen het IP van
 * dit board uit het afzenderadres. Zo hoeft niemand een IP in te typen en
 * werkt het op elk netwerk. */
static void beacon_task(void *arg)
{
    (void)arg;
    struct sockaddr_in to;
    int on = 1;

    int sock = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        xil_printf("Beacon: socket mislukt\r\n");
        vTaskDelete(NULL);
    }
    lwip_setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    memset(&to, 0, sizeof(to));
    to.sin_family      = AF_INET;
    to.sin_port        = htons(DISCOVERY_PORT);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);     /* 255.255.255.255 */

    xil_printf("Beacon: broadcast \"%s\" op poort %d\r\n", DISCOVERY_MSG, DISCOVERY_PORT);
    for (;;) {
        lwip_sendto(sock, DISCOVERY_MSG, strlen(DISCOVERY_MSG), 0,
                    (struct sockaddr *)&to, sizeof(to));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void udp_task(void *arg)
{
    int player = (int)(intptr_t)arg;
    int port = (player == 0) ? UDP_PORT_P1 : UDP_PORT_P2;
    struct sockaddr_in addr, from;
    socklen_t fromlen;
    char buf[64];

    int sock = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        xil_printf("Speler %d: socket mislukt\r\n", player + 1);
        vTaskDelete(NULL);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (lwip_bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        xil_printf("Speler %d: bind op poort %d mislukt\r\n", player + 1, port);
        lwip_close(sock);
        vTaskDelete(NULL);
    }
    xil_printf("Speler %d luistert op UDP poort %d\r\n", player + 1, port);

    for (;;) {
        fromlen = sizeof(from);
        int n = lwip_recvfrom(sock, buf, sizeof(buf), 0,
                              (struct sockaddr *)&from, &fromlen);
        if (n <= 0) continue;

        u32 ip = from.sin_addr.s_addr;                 /* netwerk-bytevolgorde */
        int was_connected = is_connected(player);
        int has_cmd = 0;

        last_seen[player] = xTaskGetTickCount();       /* elk pakket = "ik ben er" */
        seen[player] = 1;

        for (int i = 0; i < n; i++) {
            char ch = (char)toupper((unsigned char)buf[i]);
            if (ch == 'X') {                           /* speler meldt zich af */
                seen[player] = 0;
                xil_printf("P%d heeft afgemeld\r\n", player + 1);
            } else if (ch != '\0' && strchr("UDLRN12", ch)) {
                cmd_t c = { .player = (u8)player, .cmd = ch };
                xQueueSend(cmdq, &c, 0);
                has_cmd = 1;
                xil_printf("P%d van %d.%d.%d.%d: %c\r\n", player + 1,
                           (int)(ip & 0xFF), (int)((ip >> 8) & 0xFF),
                           (int)((ip >> 16) & 0xFF), (int)((ip >> 24) & 0xFF), ch);
            }
        }
        if (!was_connected && seen[player])
            xil_printf("P%d verbonden vanaf %d.%d.%d.%d\r\n", player + 1,
                       (int)(ip & 0xFF), (int)((ip >> 8) & 0xFF),
                       (int)((ip >> 16) & 0xFF), (int)((ip >> 24) & 0xFF));

        if (has_cmd)                                   /* geen antwoord op heartbeats */
            lwip_sendto(sock, "OK\n", 3, 0, (struct sockaddr *)&from, fromlen);
    }
}

/* ============================================================
 *  Aangeroepen door main.c van het template
 * ============================================================ */
void print_app_header(void)
{
    xil_printf("========== PYNQ2-Snake ==========\r\n");
    xil_printf("Speler 1: UDP poort %d   Speler 2: UDP poort %d\r\n",
               UDP_PORT_P1, UDP_PORT_P2);
    xil_printf("Commando's: U D L R (richting), N (menu), 1/2 (aantal spelers)\r\n");
    xil_printf("J = heartbeat, X = afmelden. 2 spelers start pas als beide verbonden zijn\r\n");
    xil_printf("Discovery: broadcast \"%s\" op UDP poort %d (controllers vinden dit board zelf)\r\n",
               DISCOVERY_MSG, DISCOVERY_PORT);
    xil_printf("Appel = +1 en omdraaien. 2 spelers: eerste tot %d wint\r\n", WIN_SCORE);
}

void start_application(void)
{
    /* speler 1: groen, start links, naar rechts */
    sn[0].sx = 5;        sn[0].sy = 4;        sn[0].sdir = DIR_RIGHT;
    sn[0].body = (rgb_t){ 40, 180, 70 };
    sn[0].dark = (rgb_t){ 30, 140, 55 };
    sn[0].head = (rgb_t){ 110, 240, 130 };
    /* speler 2: oranje, start rechts, naar links */
    sn[1].sx = COLS - 6; sn[1].sy = ROWS - 5; sn[1].sdir = DIR_LEFT;
    sn[1].body = (rgb_t){ 240, 130, 20 };
    sn[1].dark = (rgb_t){ 200, 100, 10 };
    sn[1].head = (rgb_t){ 255, 195, 70 };

    seed ^= (u32)xTaskGetTickCount();
    cmdq = xQueueCreate(32, sizeof(cmd_t));

    show_menu();
    if (vdma_start() != XST_SUCCESS) {
        xil_printf("VDMA start mislukt!\r\n");
        return;
    }
    xil_printf("VDMA draait - menu op HDMI\r\n");

    sys_thread_new("udp_p1", udp_task, (void *)0, 1024, DEFAULT_THREAD_PRIO);
    sys_thread_new("udp_p2", udp_task, (void *)1, 1024, DEFAULT_THREAD_PRIO);
    sys_thread_new("game",   game_task, NULL,     1024, DEFAULT_THREAD_PRIO);
    sys_thread_new("beacon", beacon_task, NULL,   1024, DEFAULT_THREAD_PRIO);
}
