/*  Fase 2: framebuffer in DDR -> AXI VDMA -> HDMI (1280x720)  */
#include <string.h>
#include "xparameters.h"
#include "xaxivdma.h"
#include "xil_cache.h"
#include "xil_printf.h"
#include "sleep.h"

/* ---------- beeldinstellingen ---------- */
#define W       1280
#define H       720
#define BPP     3                 /* bytes per pixel */
#define STRIDE  (W * BPP)         /* bytes per lijn = 3840 */

/* ---------- Snake-raster: 32 x 18 cellen van 40x40 pixels ---------- */
#define CELL    40
#define COLS    (W / CELL)        /* 32 */
#define ROWS    (H / CELL)        /* 18 */

/* achtergrondkleur (donkergrijs) */
#define BG      0x20

/* De framebuffer: 2,7 MB in DDR, uitgelijnd op 64 bytes */
static u8 fb[H * STRIDE] __attribute__((aligned(64)));
static XAxiVdma vdma;

/* =========================================================
 *  Tekenfuncties
 * ========================================================= */

/* Eén pixel zetten. Volgorde in het geheugen: G, B, R
   (Xilinx AXI4-Stream video + Digilent rgb2dvi).
   Kloppen de kleuren niet? Wissel dan p[0..2] om. */
static inline void put_px(int x, int y, u8 r, u8 g, u8 b)
{
    u8 *p = &fb[y * STRIDE + x * BPP];
    p[0] = g;
    p[1] = b;
    p[2] = r;
}

/* Rechthoek vullen */
static void fill_rect(int x0, int y0, int w, int h, u8 r, u8 g, u8 b)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            put_px(x, y, r, g, b);
}

/* Cache wegschrijven naar DDR voor een rechthoek, zodat de VDMA het ziet */
static void flush_rect(int x0, int y0, int w, int h)
{
    for (int y = y0; y < y0 + h; y++)
        Xil_DCacheFlushRange((UINTPTR)&fb[y * STRIDE + x0 * BPP], w * BPP);
}

/* Eén Snake-cel tekenen: gekleurd blok met 2 px zwarte rand */
static void draw_cell(int cx, int cy, u8 r, u8 g, u8 b)
{
    int x = cx * CELL, y = cy * CELL;
    fill_rect(x, y, CELL, CELL, 0, 0, 0);
    fill_rect(x + 2, y + 2, CELL - 4, CELL - 4, r, g, b);
    flush_rect(x, y, CELL, CELL);
}

/* Eén Snake-cel wissen (terug naar achtergrond) */
static void erase_cell(int cx, int cy)
{
    int x = cx * CELL, y = cy * CELL;
    fill_rect(x, y, CELL, CELL, BG, BG, BG);
    flush_rect(x, y, CELL, CELL);
}

/* =========================================================
 *  VDMA starten
 * ========================================================= */
static int vdma_start(void)
{
    XAxiVdma_Config *cfg;
    XAxiVdma_DmaSetup rd;
    UINTPTR addr[XAXIVDMA_MAX_FRAMESTORE];
    int status;

#ifdef SDT   /* Vitis 2024.1 en nieuwer (jij: 2026.1) */
    cfg = XAxiVdma_LookupConfig(XPAR_XAXIVDMA_0_BASEADDR);
#else
    cfg = XAxiVdma_LookupConfig(XPAR_AXI_VDMA_0_DEVICE_ID);
#endif
    if (cfg == NULL) {
        xil_printf("VDMA config niet gevonden\r\n");
        return XST_FAILURE;
    }

    status = XAxiVdma_CfgInitialize(&vdma, cfg, cfg->BaseAddress);
    if (status != XST_SUCCESS) {
        xil_printf("VDMA init mislukt (%d)\r\n", status);
        return status;
    }

    memset(&rd, 0, sizeof(rd));
    rd.VertSizeInput     = H;        /* aantal lijnen          */
    rd.HoriSizeInput     = STRIDE;   /* bytes per lijn         */
    rd.Stride            = STRIDE;   /* afstand tussen lijnen  */
    rd.EnableCircularBuf = 1;        /* blijven herhalen       */

    status = XAxiVdma_DmaConfig(&vdma, XAXIVDMA_READ, &rd);
    if (status != XST_SUCCESS) {
        xil_printf("VDMA DmaConfig mislukt (%d)\r\n", status);
        return status;
    }

    /* alle frame buffers (3) wijzen voorlopig naar dezelfde framebuffer */
    for (int i = 0; i < vdma.MaxNumFrames; i++)
        addr[i] = (UINTPTR)fb;

    status = XAxiVdma_DmaSetBufferAddr(&vdma, XAXIVDMA_READ, addr);
    if (status != XST_SUCCESS) {
        xil_printf("VDMA SetBufferAddr mislukt (%d)\r\n", status);
        return status;
    }

    status = XAxiVdma_DmaStart(&vdma, XAXIVDMA_READ);
    if (status != XST_SUCCESS) {
        xil_printf("VDMA start mislukt (%d)\r\n", status);
        return status;
    }

    return XST_SUCCESS;
}

/* =========================================================
 *  main
 * ========================================================= */
int main(void)
{
    xil_printf("\r\n--- Fase 2: VDMA framebuffer naar HDMI ---\r\n");
    xil_printf("Framebuffer op adres 0x%08x (%d bytes)\r\n",
               (unsigned)(UINTPTR)fb, (int)sizeof(fb));

    /* 1. Achtergrond + witte rand van 4 px */
    memset(fb, BG, sizeof(fb));
    fill_rect(0,     0,     W, 4, 255, 255, 255);
    fill_rect(0,     H - 4, W, 4, 255, 255, 255);
    fill_rect(0,     0,     4, H, 255, 255, 255);
    fill_rect(W - 4, 0,     4, H, 255, 255, 255);

    /* 2. Kleurtest linksboven: ROOD - GROEN - BLAUW */
    draw_cell(1, 1, 255, 0,   0);
    draw_cell(2, 1, 0,   255, 0);
    draw_cell(3, 1, 0,   0,   255);

    /* hele beeld naar DDR schrijven */
    Xil_DCacheFlushRange((UINTPTR)fb, sizeof(fb));

    /* 3. VDMA starten -> beeld verschijnt op HDMI */
    if (vdma_start() != XST_SUCCESS) {
        xil_printf("Stop: VDMA werkt niet\r\n");
        return XST_FAILURE;
    }
    xil_printf("VDMA draait. Kleurtest: rood-groen-blauw linksboven.\r\n");

    /* 4. Demo: een "slang" van 5 cellen beweegt over rij 9
          (kolom 1 t/m 30, zodat de witte rand heel blijft) */
    const int len = 5;
    const int span = COLS - 2;       /* 30 bruikbare kolommen */
    int step = 0;

    while (1) {
        int head = 1 + (step % span);
        draw_cell(head, 9, 0, 200, 0);              /* nieuwe kop       */

        if (step >= len) {
            int tail = 1 + ((step - len) % span);
            erase_cell(tail, 9);                    /* oude staart weg  */
        }

        step++;
        usleep(150000);                             /* 150 ms per stap  */
    }
}