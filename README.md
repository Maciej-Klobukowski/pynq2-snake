# PYNQ2-Snake – HDMI/UDP Snake op de PYNQ-Z2

![Overzicht](./docs/images/Overview.jpg)
> **⚠️ Vervang deze afbeelding door een foto van jullie project**

## Beschrijving

Een IoT-platform op een **PYNQ-Z2** (Zynq-7000) dat een Snake-spel voor twee spelers toont op een **HDMI-scherm**. De spelers sturen hun slang aan via **twee UDP-verbindingen**, elk vanaf een eigen toestel (laptop of een ander PYNQ-board).

- De **Processing System (PS)**, een ARM Cortex-A9, draait **FreeRTOS** met lwIP. Die ontvangt de UDP-pakketten en voert de spellogica uit.
- De **Programmable Logic (PL)** maakt het HDMI-beeld: video-timing, AXI4-Stream naar video en TMDS-encoding.

### Vereisten van de opdracht

| Vereiste | Invulling |
|---|---|
| Ontwikkeld op een PYNQ-board | PYNQ-Z2 (xc7z020clg400-1) |
| HDMI via de programmable logic | VTC + AXI4-Stream to Video Out + Digilent `rgb2dvi` in de PL |
| 2 UDP-communicaties | lwIP-sockets op poort 5000 (speler 1) en 5001 (speler 2) |
| FreeRTOS | `freertos` domain in Vitis |
| *Nice-to-have:* 2 verschillende devices | elke speler stuurt vanaf een eigen toestel |
| *Nice-to-have:* andere PYNQ-boards | controller-boards sturen UDP via de knoppen op het board |

### Sequence of operation

1. Verbind de PYNQ-Z2 via micro-USB met de pc. Zet de boot-jumper op JTAG en de power-jumper op USB.
2. Sluit een monitor aan op **HDMI OUT**.
3. Start de applicatie vanuit Vitis. De seriële terminal (115200 baud) toont een opstartbericht.
4. Het scherm toont het speelveld *(fase 1: kleurbalken als test)*.
5. De spelers sturen `U`/`D`/`L`/`R` via UDP naar het board *(fase 3)*.

## Architectuur

```
Zynq PS (ARM) ──AXI-Lite──► Video Test Pattern Generator        (fase 1)
                             │ AXI4-Stream (24-bit RGB)
Video Timing Controller ──► AXI4-Stream to Video Out ──► rgb2dvi ──► HDMI OUT
Clocking Wizard: FCLK_CLK0 100 MHz → 74,25 MHz pixelklok (720p60)
```

Fase 2 vervangt de TPG door een **AXI VDMA** die een framebuffer uit DDR leest (via S_AXI_HP0).

## Materialen en methodes

- **Hardware**: PYNQ-Z2, HDMI-monitor, micro-USB-kabel, ethernetkabel + switch
- **FPGA-design**: AMD Vivado 2026.1 (IP Integrator, block design)
- **Firmware**: AMD Vitis 2026.1 (Unified IDE), C, standalone → FreeRTOS + lwIP
- **Externe IP**: [Digilent vivado-library](https://github.com/Digilent/vivado-library) (`rgb2dvi`)

## Projectstructuur

```
snake_UDP_FREErtos/
├── snake_UDP_FREErtos.xpr          Vivado-project
├── snake_UDP_FREErtos.srcs/        block design (design_1.bd) + IP-configuratie (.xci)
├── constraints/hdmi_pynq_z2.xdc    HDMI TX-pinnen (uit de PYNQ-Z2 master XDC)
├── test_wrapper.xsa                hardware-export voor Vitis (incl. bitstream)
└── vitis_workspace/
    ├── pynq_hdmi_plat/             Vitis-platform (wordt uit de .xsa gegenereerd)
    └── hdmi_app/src/helloworld.c   applicatie
```

Build-output (`*.runs`, `*.gen`, `*.cache`, Vitis `build/`, enz.) en de `vivado-library-master` staan niet in git, zie `.gitignore`.

## Aan de slag

### Benodigdheden
1. **Vivado + Vitis 2026.1**
2. **PYNQ-Z2 board files**: kopieer de map `pynq-z2` naar
   `<install>\Vivado\2026.1\data\xhub\boards\XilinxBoardStore\boards\Xilinx\`
3. **Digilent vivado-library**: download de ZIP van GitHub en pak hem uit als
   `snake_UDP_FREErtos/vivado-library-master/`. Het project verwijst naar dat pad.

### Hardware bouwen (Vivado)
1. Open `snake_UDP_FREErtos/snake_UDP_FREErtos.xpr`.
2. **Generate Bitstream**. Controleer daarna dat de timing gehaald wordt (WNS ≥ 0).
3. **File → Export → Export Hardware** → *Include bitstream* → `test_wrapper.xsa`.

### Software bouwen en draaien (Vitis)
1. Open Vitis → **Set Workspace** → `snake_UDP_FREErtos/vitis_workspace`.
2. Bouw het platform `pynq_hdmi_plat` (Flow → Build). Bestaat het niet meer, maak het dan opnieuw aan: *New Component → Platform* met `test_wrapper.xsa`, standalone, `ps7_cortexa9_0`.
3. Bouw `hdmi_app` (Flow → Build).
4. Board: jumper op **JTAG**, voeding op **USB**, HDMI-kabel in **HDMI OUT**.
5. Open de seriële monitor (115200 baud) en klik op **Run**.

## Resultaten

- **Fase 1 ✅**: kleurbalken (TPG) op 1280×720 via HDMI vanuit de PL. Timing gehaald (WNS 4,337 ns, WHS 0,013 ns).
  UART-uitvoer:
  ```
  --- Fase 1: TPG naar HDMI ---
  TPG draait: kleurbalken op HDMI OUT
  ```

## Discussie

**Wat werkt al?**

- [x] HDMI-uitgang via de PL (720p, TPG-kleurbalken)
- [ ] Eigen beeld via VDMA + framebuffer in DDR
- [ ] FreeRTOS + lwIP, 2 UDP-poorten
- [ ] Snake-spellogica
- [ ] Controller-boards (andere PYNQ's) als UDP-zender

**Komende stappen**

1. **Fase 2**: TPG vervangen door AXI VDMA (framebuffer in DDR, S_AXI_HP0)
2. **Fase 3**: FreeRTOS-platform met lwIP, UDP-taken op poort 5000/5001 en een game-taak
3. **Fase 4**: andere PYNQ-boards als controller

**Bekende bugs of beperkingen**

- Vivado 2026.1 vond de Digilent TMDS-interface pas na `update_ip_catalog -rebuild` (block design eerst sluiten).
- In Vitis 2026.1 verschijnen de embedded voorbeelden pas nadat er een workspace is ingesteld.

## Documentatie

- [Design Document](./docs/design.md)
- [User Manual](./docs/user-manual.md)

## Personen

- Student 1 [@username](https://github.com/student1)
- Student 2 [@username](https://github.com/student2)
