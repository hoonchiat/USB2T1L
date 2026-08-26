# STM32F407 ⇄ ADIN2111 ⇄ USB Ethernet Bridge (FreeRTOS)

Firmware that turns an **STM32F407** into a transparent bridge between an
**Analog Devices ADIN2111** 10BASE‑T1L Ethernet MAC‑PHY (over SPI) and a
**Linux host over USB 2.0**. The device enumerates as a **CDC‑ECM** network
adapter, so Linux binds it with the in‑tree `cdc_ether` driver and it shows up
as an ordinary Ethernet interface (`enx…` / `usb0`) — no custom host driver.

```
     10BASE-T1L (SPE)          SPI1 @ 21 MHz            USB 2.0 Full Speed
   ┌───────────────┐   INT   ┌───────────────────┐   Bulk IN/OUT  ┌───────────┐
   │   ADIN2111    │◀──────▶│    STM32F407       │◀─────────────▶│ Linux host │
   │ 2× SPE PHY +  │  SPI    │  FreeRTOS bridge   │  CDC-ECM +    │ cdc_ether │
   │ switch + MAC  │◀──────▶│  (this firmware)   │  Interrupt IN │  → ethX    │
   └───────────────┘   RST   └───────────────────┘  (link notify) └───────────┘
```

## How it works

Two independent data paths, each with its own pre‑allocated frame‑buffer pool,
run as FreeRTOS tasks:

| Direction | Path |
|-----------|------|
| **Uplink** (wire → host) | ADIN asserts `INT` → `net_rx_task` reads frames from the ADIN RX FIFO over SPI → transmits them on the USB **bulk IN** endpoint (CDC‑ECM). |
| **Downlink** (host → wire) | Host sends a frame on the USB **bulk OUT** endpoint (zero‑copy into a pool buffer) → `net_tx_task` writes it into the ADIN TX FIFO over SPI. |

A third `link_task` polls the PHY link state and reports it to the host over
the ECM **interrupt IN** endpoint (`NETWORK_CONNECTION` /
`CONNECTION_SPEED_CHANGE`), so the host's carrier tracks the SPE link.

The ADIN2111 appends/verifies the Ethernet FCS in hardware. The device's unicast
MAC (advertised to the USB host **and** programmed into the ADIN address filter)
is derived from the STM32 96‑bit unique ID, so multiple boards don't collide. How
frames are forwarded depends on `ADIN_DAISY_CHAIN_MODE` — see below.

## Forwarding modes (`ADIN_DAISY_CHAIN_MODE`)

The ADIN2111 is a **filter‑table switch, not an auto‑learning flood switch**: a
frame is forwarded from one T1L port to the other only if its destination
matches a filter‑table entry with the *forward‑to‑other‑port* bit set. There is
no hardware flooding of unknown unicast between ports. Two modes are built on
that:

**Endpoint** (`ADIN_DAISY_CHAIN_MODE = 0`) — promiscuous NIC. `FWD_UNK2HOST` is
set so all traffic reaches the host; nothing is switched port‑to‑port. Use this
for a leaf node / single‑port ADIN1110.

**Daisy‑chain switch** (`= 1`, default) — for line topologies where this node
sits mid‑chain (Port 1 ↔ upstream, Port 2 ↔ downstream) and must pass through
traffic for other nodes without burdening the host/USB:

- **Cut‑through** (`PORT_CUT_THRU_EN`) is enabled — minimal per‑hop latency.
- **Broadcast + multicast** (one group catch‑all filter, `01:…` mask) are
  flooded to the other T1L port *and* copied to the host, so they propagate
  down the chain and the host still sees them.
- **Frames for us** (own‑MAC filter) go to the host only.
- **Unicast for other nodes** is switched port‑to‑port in hardware via a
  **host‑learned forwarding table**: `net_rx_task` learns the source MAC +
  ingress port of every frame the host sees (broadcasts, multicasts, frames for
  us) and programs an ADIN filter slot so later unicast to that node is
  forwarded without host involvement. Entries age out (`NET_FDB_AGE_MS`) and are
  bounded by the **14 free filter slots** (`NET_FDB_MAX_ENTRIES`). Host‑injected
  frames egress the learned port, or flood both ports when the destination is
  broadcast/multicast or not yet learned.

Because learning is seeded from broadcast/multicast (e.g. ARP/ND), unicast that
is *never* preceded by any broadcast from its target — and networks with more
than 14 active peers per node — will not be fully hardware‑switched; the classic
options are static entries, or a software‑forwarding fallback. Ring topologies
need a loop‑prevention protocol (out of scope). This node still enumerates and
works as a normal USB Ethernet endpoint in either mode.

## Repository layout

```
USB2T1L/                (repository root)
├── Core/               MCU bring-up
│   ├── Inc/app_config.h    ← EDIT THIS: pin map, clocks, buffer sizes, MAC
│   ├── Src/bsp.c           clocks (168 MHz), SPI1, GPIO, LEDs
│   ├── Src/main.c          boot + FreeRTOS start + kernel hooks
│   ├── Src/stm32f4xx_it.c  ISRs (EXTI for ADIN INT, USB OTG, TIM6 tick)
│   └── Src/stm32f4xx_hal_timebase_tim.c   HAL tick on TIM6 (SysTick → RTOS)
├── Drivers/ADIN2111/   Portable ADIN2111/ADIN1110 driver + STM32 SPI port
├── Net/                Frame pool + the bridge tasks/queues
├── Middlewares/USB_ECM/ CDC-ECM class, descriptors, PCD glue
├── App/usb_device.c    USB device core bring-up
├── config/             FreeRTOSConfig.h, stm32f4xx_hal_conf.h, usbd_conf.h
├── ldscripts/          STM32F407VGTx linker script
├── Makefile            build (references STM32CubeF4)
└── scripts/get_deps.sh fetch STM32CubeF4
```

## Hardware / wiring

Default pin map (all configurable in `Core/Inc/app_config.h`):

| Signal          | STM32F407 pin | Notes |
|-----------------|---------------|-------|
| SPI1 SCK        | PA5           | AF5, ≤ 25 MHz (21 MHz default) |
| SPI1 MISO       | PA6           | AF5 |
| SPI1 MOSI       | PA7           | AF5 |
| ADIN CS         | PA4           | GPIO, software NSS (active low) |
| ADIN INT        | PC4           | EXTI4, falling edge, pull‑up (INT is open‑drain, active low) |
| ADIN RST        | PC5           | GPIO, active low |
| USB OTG_FS D−   | PA11          | AF10 → host USB |
| USB OTG_FS D+   | PA12          | AF10 → host USB |
| Link LED        | PD12          | optional (STM32F4‑Discovery green) |
| Activity LED    | PD14          | optional (STM32F4‑Discovery red) |

The ADIN2111 SPI must be strapped for the **generic** (non‑OPEN‑Alliance) SPI
protocol. If you strap it for SPI CRC, keep `ADIN_SPI_USE_CRC = 1`
(default); otherwise set it to `0` — the two **must** match or every SPI
transaction fails.

## Building

Requires the GNU Arm Embedded toolchain (`arm-none-eabi-gcc`) and `make`.

```sh
make deps        # clone STM32CubeF4 into third_party/ (once)
make             # -> build/stm32f407-adin2111-ecm.{elf,hex,bin}
```

Point at an existing Cube checkout instead of cloning:

```sh
make CUBE=/path/to/STM32CubeF4
```

Flash with an ST‑LINK (uses `openocd.cfg`):

```sh
make flash
```

## Bring‑up on Linux

Plug the STM32 USB port into the Linux host. You should see:

```sh
$ dmesg | tail
cdc_ether 1-1:1.0 usb0: register 'cdc_ether' ... CDC Ethernet Device
$ ip link show usb0
usb0: <BROADCAST,MULTICAST> mtu 1500 ... link/ether 02:11:xx:xx:xx:xx
```

Bring it up and use it like any NIC:

```sh
sudo ip link set usb0 up
sudo ip addr add 192.0.2.2/24 dev usb0     # or dhclient usb0
ping 192.0.2.1                              # a peer on the 10BASE-T1L segment
```

The interface name may be `usb0` or a predictable `enxNN…` name derived from
the advertised MAC, depending on your distro's `systemd-udev` naming rules.

## Production provisioning

Per‑unit data (MAC, serial, batch, firmware version, hardware rev, date) lives
in a **128‑byte record in the last flash sector** (sector 11 @ `0x080E0000`,
reserved by the linker, so a firmware update never touches it). The record is
CRC‑32 protected. `bsp_get_mac_address()` returns the provisioned MAC — which
feeds **both** the USB CDC‑ECM descriptor and the ADIN2111 filter — and falls
back to the UID‑derived MAC when the record is absent/invalid, so unprovisioned
boards still work. The USB `iSerialNumber` also comes from the record.

**Write the MAC / serial / batch at production test** (over SWD —
`tools/prodinfo.py`, stdlib only; needs `st-flash` or `openocd`):

```sh
# 1. flash the firmware first (sector-erase, NOT a full-chip mass-erase)
# 2. inject the per-unit record to 0x080E0000 in one step (build + flash + verify)
python3 tools/prodinfo.py write --mac 02:11:22:33:44:55 --serial SN0001 \
        --batch B2026-08 --fwver v0.1 --hwrev A1 --date 2026-08-25

# update just one field, keeping whatever is already on the device (read-modify-write)
python3 tools/prodinfo.py write --keep --serial SN0002

# OpenOCD instead of st-flash:
python3 tools/prodinfo.py write --programmer openocd --mac 02:11:22:33:44:55 --serial SN0001
```

`write` builds the 128‑byte record in memory, flashes it, then reads it back
and verifies (`--no-verify` to skip, `--dry-run` to preview without flashing).
The lower‑level `build` (make a `.bin`) and `flash` (write a `.bin`) commands
are still available for an offline file workflow.

> Order matters: program the firmware first (or use sector erase), then write
> the record — a chip **mass‑erase** would wipe sector 11.

**Read the MAC / serial / batch back:**

- **Over SWD** (no firmware needed — reads sector 11 directly):
  ```sh
  python3 tools/prodinfo.py dump         # st-flash / OpenOCD read of 0x080E0000
  ```
- **Over USB, from Linux, with the firmware running:**
  - **MAC** → the interface MAC: `ip link show usb0`.
  - **Serial** → `cat /sys/bus/usb/devices/*/serial` or `lsusb -v`.
  - **Full record** (all fields) → a vendor USB control request:
    ```sh
    python3 tools/prodinfo.py read       # pyusb: ctrl_transfer(0xC0, 0x50, …)
    ```

The firmware's CRC (`prodinfo_crc32`) is byte‑identical to Python's
`zlib.crc32`, so a record the tool builds always validates on‑device.

## Tuning

Everything worth changing lives in `Core/Inc/app_config.h`:

- **Clocks** — `BOARD_HSE_HZ` and the PLL factors. Defaults assume an 8 MHz
  crystal; the PLL yields 168 MHz SYSCLK and exactly 48 MHz for USB.
- **Pins / SPI instance** — remap any signal, change `ADIN_SPI_PRESCALER`.
- **Buffering** — `NET_RX_POOL_COUNT` / `NET_TX_POOL_COUNT` trade RAM for burst
  tolerance (each buffer is `NET_MAX_FRAME_LEN` ≈ 1.5 KiB).
- **MAC address** — provisioned in flash (see **Production provisioning**), or
  override `BOARD_MAC_*` / `bsp_get_mac_address()` for the UID‑derived fallback.
- **Forwarding** — `ADIN_DAISY_CHAIN_MODE` (endpoint vs daisy‑chain switch),
  `NET_FDB_MAX_ENTRIES` (learned entries, ≤ 14), `NET_FDB_AGE_MS` (entry idle
  timeout).
- **Task priorities / stacks** — the `TASK_*` macros.

## Design notes & limitations

- **Full Speed only.** The STM32F407 OTG_FS PHY is USB 2.0 Full Speed
  (12 Mb/s). That comfortably exceeds the 10 Mb/s 10BASE‑T1L line rate, so USB
  is not the bottleneck. (An STM32F407 has no HS PHY; use an F411/F7/H7 with
  ULPI if you need USB HS.)
- **SPI moves frames by DMA.** Frame‑sized SPI bursts to/from the ADIN2111 run
  on the SPI TX/RX DMA streams (DMA2); `net_rx_task`/`net_tx_task` block on a
  completion semaphore instead of busy‑polling the SPI data register, so the
  CPU is free during the ~0.6 ms per‑frame transfer. Small register accesses
  (< `ADIN_SPI_DMA_MIN_LEN`) stay on the polled path where DMA setup would cost
  more than it saves; device bring‑up (before the scheduler) is polled too. Set
  `ADIN_SPI_USE_DMA = 0` in `app_config.h` to force the fully polled transport.
  Note the **USB** side cannot use DMA here: the STM32F407 OTG_FS core has no
  DMA engine (only OTG_HS does), so USB packets are moved by the OTG ISR — this
  is not a bottleneck at 10 Mb/s. Moving the USB path to DMA would require
  switching to OTG_HS (different pins/PHY).
- **One in‑flight uplink frame.** `net_rx_task` sends each frame and waits for
  the USB transfer to complete before the next. Simple and correct; more than
  enough for 10 Mb/s. Raise throughput further by pipelining bulk‑IN transfers.
- **CDC‑ECM** was chosen for zero‑driver Linux support. For Windows you'd add
  RNDIS or NCM; the class layer (`usbd_ecm.c`) is the only part that changes.
- **VID/PID** in `usbd_desc.h` are placeholders (ST's VID + an arbitrary PID).
  Replace them with a pair you're licensed to ship.
- The ADIN2111 register map / SPI generic protocol in `Drivers/ADIN2111` was
  implemented against the ADI datasheet and cross‑checked with the mainline
  Linux `adin1110` driver and ADI's no‑OS driver. The same driver also
  supports the single‑port **ADIN1110** (auto‑detected by PHY ID).

## License

Application code (`Core`, `Net`, `Drivers/ADIN2111`, `Middlewares/USB_ECM`,
`App`) is provided as‑is for your project. The STM32 HAL, CMSIS, ST USB Device
Library and FreeRTOS fetched into `third_party/` carry their own licenses
(ST Ultimate Liberty / Apache‑2.0 / MIT respectively).
