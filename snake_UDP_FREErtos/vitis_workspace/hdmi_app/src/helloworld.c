#include <stdio.h>
#include "platform.h"
#include "xil_printf.h"
#include "xparameters.h"
#include "xv_tpg.h"

XV_tpg tpg;

int main(void)
{
    int Status;

    init_platform();
    xil_printf("\r\n--- Fase 1: TPG naar HDMI ---\r\n");

    /* 1. Driver koppelen aan de TPG (adres uit xparameters.h) */
    Status = XV_tpg_Initialize(&tpg, XPAR_XV_TPG_0_BASEADDR);
    if (Status != XST_SUCCESS) {
        xil_printf("TPG init mislukt\r\n");
        return XST_FAILURE;
    }

    /* 2. Instellen: 1280x720, RGB, kleurbalken */
    XV_tpg_Set_width(&tpg, 1280);
    XV_tpg_Set_height(&tpg, 720);
    XV_tpg_Set_colorFormat(&tpg, 0);                 /* 0 = RGB */
    XV_tpg_Set_bckgndId(&tpg, XTPG_BKGND_COLOR_BARS);
    XV_tpg_Set_ovrlayId(&tpg, 0);                    /* geen overlay */

    /* 3. Starten, en automatisch blijven herhalen (elk frame opnieuw) */
    XV_tpg_EnableAutoRestart(&tpg);
    XV_tpg_Start(&tpg);

    xil_printf("TPG draait: kleurbalken op HDMI OUT\r\n");

    while (1) { }   /* programma laten doorlopen */
}