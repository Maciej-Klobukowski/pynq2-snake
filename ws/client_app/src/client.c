/*  client.c - PYNQ2-Snake controller-board (fase 4)
 *
 *  Zelfde code voor beide controllers:
 *    SW0 uit = speler 1 (UDP poort 5000)   SW0 aan = speler 2 (UDP poort 5001)
 *
 *  Knoppen (BTN0..BTN3) -> richting, zie BTN_CMD hieronder.
 *  BTN0 + BTN3 samen     -> 'N' (terug naar het menu)
 *
 *  Automatisch op elk netwerk:
 *    - IP via DHCP (main.c van het template)
 *    - het hoofdboard broadcast elke seconde "PYNQSNAKE" op UDP 5050;
 *      wij halen zijn IP uit het afzenderadres (discovery_task)
 *    - elke seconde een heartbeat 'J' zodat het hoofdboard weet dat we er zijn
 *
 *  LEDs (alleen als er een tweede AXI GPIO met leds_4bits in de hardware zit):
 *    LD0 = hoofdboard gevonden   LD1 = knippert per heartbeat
 *    LD2 = speler 1              LD3 = speler 2
 *
 *  main.c (template "FreeRTOS lwIP UDP Perf Server") roept na het opstarten
 *  van het netwerk print_app_header() en start_application() aan, en vraagt
 *  vooraf board_mac_last_byte() op voor een uniek MAC-adres.
 */
#include <string.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lwipopts.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "xparameters.h"
#include "xgpio.h"
#include "xil_printf.h"

/* ================= instellingen ================= */
#define DISCOVERY_PORT     5050
#define DISCOVERY_MSG      "PYNQSNAKE"
#define PORT_P1            5000
#define PORT_P2            5001
#define POLL_MS            20       /* knoppen uitlezen elke 20 ms (= ontdendering) */
#define HEARTBEAT_MS       1000     /* elke seconde 'J' */
#define SERVER_TIMEOUT_MS  3000     /* hoofdboard "weg" na 3 s geen broadcast */

/* Knop -> commando. Index = BTNx. Pas aan als je een andere volgorde wilt. */
static const char BTN_CMD[4] = { 'L', 'D', 'U', 'R' };
#define BTN_MENU_MASK  0x9          /* BTN0 + BTN3 tegelijk = 'N' */

/* GPIO-kanalen van axi_gpio_0 (uit Vivado):
 *   kanaal 1 = btns_4bits (4 knoppen)   kanaal 2 = sws_2bits (2 schakelaars) */
#define CH_BTN  1
#define CH_SW   2

#if defined(XPAR_AXI_GPIO_1_BASEADDR) || defined(XPAR_AXI_GPIO_1_DEVICE_ID)
#define HAVE_LEDS 1                 /* tweede AXI GPIO = leds_4bits */
#endif

/* ================= toestand ================= */
static XGpio gpio_in;
#ifdef HAVE_LEDS
static XGpio gpio_led;
#endif
static int gpio_ready = 0;

static volatile u32 server_ip = 0;          /* netwerk-bytevolgorde, 0 = onbekend */
static volatile TickType_t server_seen = 0;
static int tx_sock = -1;                    /* socket om commando's te sturen */

/* ============================================================
 *  GPIO
 * ============================================================ */
static void gpio_init(void)
{
    if (gpio_ready) return;

#ifdef SDT
    XGpio_Initialize(&gpio_in, XPAR_AXI_GPIO_0_BASEADDR);
#else
    XGpio_Initialize(&gpio_in, XPAR_AXI_GPIO_0_DEVICE_ID);
#endif
    XGpio_SetDataDirection(&gpio_in, CH_BTN, 0xF);    /* 1 = ingang */
    XGpio_SetDataDirection(&gpio_in, CH_SW,  0x3);

#ifdef HAVE_LEDS
#ifdef SDT
    XGpio_Initialize(&gpio_led, XPAR_AXI_GPIO_1_BASEADDR);
#else
    XGpio_Initialize(&gpio_led, XPAR_AXI_GPIO_1_DEVICE_ID);
#endif
    XGpio_SetDataDirection(&gpio_led, 1, 0x0);        /* 0 = uitgang */
    XGpio_DiscreteWrite(&gpio_led, 1, 0);
#endif
    gpio_ready = 1;
}

/* 0 = speler 1, 1 = speler 2 (schakelaar SW0) */
static int read_player(void)
{
    gpio_init();
    return (XGpio_DiscreteRead(&gpio_in, CH_SW) & 0x1) ? 1 : 0;
}

static u32 read_buttons(void)
{
    return XGpio_DiscreteRead(&gpio_in, CH_BTN) & 0xF;
}

static void set_leds(u32 v)
{
#ifdef HAVE_LEDS
    XGpio_DiscreteWrite(&gpio_led, 1, v & 0xF);
#else
    (void)v;
#endif
}

/* Opgeroepen door main.c VOOR het netwerk start: uniek MAC-adres per speler.
 * Hoofdboard = ...:02 (template), controller speler 1 = ...:11, speler 2 = ...:12 */
u8 board_mac_last_byte(void)
{
    return (u8)(0x11 + read_player());
}

/* ============================================================
 *  Netwerk
 * ============================================================ */
static int server_known(void)
{
    return server_ip != 0 &&
           (xTaskGetTickCount() - server_seen) < pdMS_TO_TICKS(SERVER_TIMEOUT_MS);
}

static void print_ip(const char *msg, u32 ip)
{
    xil_printf("%s%d.%d.%d.%d\r\n", msg,
               (int)(ip & 0xFF), (int)((ip >> 8) & 0xFF),
               (int)((ip >> 16) & 0xFF), (int)((ip >> 24) & 0xFF));
}

/* stuur 1 teken naar het hoofdboard, op de poort van deze speler */
static void send_cmd(char c, int player)
{
    struct sockaddr_in to;

    if (!server_known() || tx_sock < 0) return;

    memset(&to, 0, sizeof(to));
    to.sin_family      = AF_INET;
    to.sin_port        = htons(player ? PORT_P2 : PORT_P1);
    to.sin_addr.s_addr = server_ip;
    lwip_sendto(tx_sock, &c, 1, 0, (struct sockaddr *)&to, sizeof(to));
}

/* luistert naar de broadcast van het hoofdboard */
static void discovery_task(void *arg)
{
    (void)arg;
    struct sockaddr_in addr, from;
    socklen_t fromlen;
    char buf[32];

    int sock = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        xil_printf("Discovery: socket mislukt\r\n");
        vTaskDelete(NULL);
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(DISCOVERY_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (lwip_bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        xil_printf("Discovery: bind op poort %d mislukt\r\n", DISCOVERY_PORT);
        lwip_close(sock);
        vTaskDelete(NULL);
    }
    xil_printf("Hoofdboard zoeken (UDP %d)...\r\n", DISCOVERY_PORT);

    for (;;) {
        fromlen = sizeof(from);
        int n = lwip_recvfrom(sock, buf, sizeof(buf), 0,
                              (struct sockaddr *)&from, &fromlen);
        if (n < (int)strlen(DISCOVERY_MSG) ||
            memcmp(buf, DISCOVERY_MSG, strlen(DISCOVERY_MSG)) != 0)
            continue;

        u32 ip = from.sin_addr.s_addr;
        if (!server_known() || ip != server_ip)
            print_ip("Hoofdboard gevonden op ", ip);
        server_ip   = ip;
        server_seen = xTaskGetTickCount();
    }
}

/* leest knoppen + schakelaar, stuurt commando's en heartbeats */
static void control_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    TickType_t last_hb = 0;
    u32 prev_btn = 0;
    int player = read_player();
    int was_known = 0;
    int blink = 0;
    char dummy[16];

    xil_printf("Controller voor speler %d (poort %d)\r\n",
               player + 1, player ? PORT_P2 : PORT_P1);

    for (;;) {
        /* --- schakelaar: speler gewisseld? --- */
        int p = read_player();
        if (p != player) {
            send_cmd('X', player);                  /* afmelden op de oude poort */
            player = p;
            last_hb = 0;                            /* meteen heartbeat op de nieuwe */
            xil_printf("Gewisseld naar speler %d (poort %d)\r\n",
                       player + 1, player ? PORT_P2 : PORT_P1);
        }

        /* --- hoofdboard gevonden / kwijt --- */
        int known = server_known();
        if (known != was_known) {
            xil_printf(known ? "Verbonden met hoofdboard\r\n"
                             : "Hoofdboard kwijt - opnieuw zoeken...\r\n");
            was_known = known;
            last_hb = 0;
        }

        /* --- knoppen: alleen bij indrukken (flank) --- */
        u32 btn = read_buttons();
        u32 pressed = btn & ~prev_btn;
        if (pressed) {
            if ((btn & BTN_MENU_MASK) == BTN_MENU_MASK && (pressed & BTN_MENU_MASK)) {
                send_cmd('N', player);
                xil_printf("P%d: N (menu)\r\n", player + 1);
            } else {
                for (int i = 0; i < 4; i++)
                    if (pressed & (1u << i)) {
                        send_cmd(BTN_CMD[i], player);
                        xil_printf("P%d: BTN%d -> %c\r\n", player + 1, i, BTN_CMD[i]);
                    }
            }
        }
        prev_btn = btn;

        /* --- heartbeat elke seconde --- */
        TickType_t now = xTaskGetTickCount();
        if (known && (last_hb == 0 || now - last_hb >= pdMS_TO_TICKS(HEARTBEAT_MS))) {
            send_cmd('J', player);
            last_hb = now;
            blink = !blink;
        }

        /* --- antwoorden ("OK") van het hoofdboard weggooien --- */
        while (lwip_recvfrom(tx_sock, dummy, sizeof(dummy), MSG_DONTWAIT, NULL, NULL) > 0)
            ;

        /* --- LEDs --- */
        set_leds((known ? 0x1 : 0) | (blink ? 0x2 : 0) | (player ? 0x8 : 0x4));

        vTaskDelayUntil(&last, pdMS_TO_TICKS(POLL_MS));
    }
}

/* ============================================================
 *  Aangeroepen door main.c van het template
 * ============================================================ */
void print_app_header(void)
{
    xil_printf("========== PYNQ2-Snake controller ==========\r\n");
    xil_printf("SW0 uit = speler 1 (poort %d), SW0 aan = speler 2 (poort %d)\r\n",
               PORT_P1, PORT_P2);
    xil_printf("BTN0..3 = %c %c %c %c, BTN0+BTN3 = menu\r\n",
               BTN_CMD[0], BTN_CMD[1], BTN_CMD[2], BTN_CMD[3]);
#ifndef HAVE_LEDS
    xil_printf("(geen LED-GPIO in de hardware: status alleen via UART)\r\n");
#endif
}

void start_application(void)
{
    gpio_init();

    tx_sock = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (tx_sock < 0) {
        xil_printf("Socket mislukt\r\n");
        return;
    }

    sys_thread_new("discovery", discovery_task, NULL, 1024, DEFAULT_THREAD_PRIO);
    sys_thread_new("control",   control_task,   NULL, 1024, DEFAULT_THREAD_PRIO);
}
