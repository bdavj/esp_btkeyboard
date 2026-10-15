# espkb

Turns an ESP32-S3 into a USB keyboard for whatever machine it's plugged into
(say, your laptop dock). It does three things:

- **Bridges BLE keyboards to USB.** Up to 3 Bluetooth LE keyboards at once; their
  key presses are merged into one USB keyboard.
- **Works in BIOS/UEFI.** It shows up as a standard boot-protocol keyboard, and
  a key press can wake a sleeping host.
- **Has a phone web UI, on demand.** Tap the button and it starts a Wi-Fi AP with
  a page that has modifiers, F-keys, arrows, combos (Ctrl+Alt+Del…), a full
  touch keyboard, and a text box you can type or paste into with your phone's
  keyboard.
- **Spams BIOS keys.** Cycles Del/Esc/F2/F12 while a machine boots, so you land
  in setup without timing a key press.

Your BLE keyboard pairs with the ESP32 once and stays paired. When you move the
dock to another machine, the keyboard comes along.

## Quick start (macOS, AtomS3U)

You need Docker (for the build) and Python 3 (for the flasher). You don't need
to install ESP-IDF.

1. **Set your Wi-Fi password.** Edit `sdkconfig.defaults` and add
   `CONFIG_ESPKB_AP_PASSWORD="something-long"`. Add `CONFIG_ESPKB_LAYOUT_UK=y`
   as well if your machines use a UK layout. Then delete `sdkconfig` so the
   build picks the changes up:

   ```bash
   rm -f sdkconfig
   ```

2. **Build:**

   ```bash
   docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.4.2 idf.py set-target esp32s3 build
   ```

3. **Install the flasher:**

   ```bash
   python3 -m pip install --user 'esptool<5'
   ```

4. **Put the AtomS3U in download mode.** Plug it in and hold the side reset
   button for about 2s until the LED turns green. Check that a port appeared:

   ```bash
   ls /dev/cu.usbmodem*
   ```

5. **Flash** (from the `build` directory):

   ```bash
   cd build && python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem* -b 460800 write_flash @flash_args
   ```

6. **Unplug and replug** the AtomS3U, or let esptool reset it. It now shows up
   as **"espkb BLE-to-USB keyboard"**.
   - **macOS opens the Keyboard Setup Assistant.** Click Continue, press the key
     it asks for on your **BT keyboard** once that's paired, or just close it
     and pick the layout yourself: ANSI for US, ISO for UK.
   - Windows and Linux need no setup.

7. **Pair your keyboard.** See below.

## Hardware

You need an **ESP32-S3**. It's the only ESP32 with both Bluetooth and a native
USB device port:

| Chip | BLE | USB device (HID) | Works? |
|---|---|---|---|
| ESP32-S3 | ✅ | ✅ | ✅ |
| ESP32 (original) | ✅ (+Classic) | ❌ (only a USB-UART bridge) | ❌ |
| ESP32-S2 | ❌ | ✅ | ❌ |
| ESP32-C3 / C6 / H2 | ✅ | ❌ (USB-Serial-JTAG only) | ❌ |

The **M5Stack AtomS3U** is an ESP32-S3, and it's the default target. Its USB-A
plug is wired to the S3's native USB, so it works as-is. On a dev board with two
USB ports, plug the port labelled **USB** (not UART) into the computer.

**BLE only.** Classic Bluetooth (BR/EDR) keyboards won't work, because the S3
has no Classic radio. Most modern keyboards (Logitech MX/Keys, Keychron in BT
mode, etc.) do BLE. Multi-device keyboards often do BLE on some channels and
Classic on others.

| Board | Button | LED | Config |
|---|---|---|---|
| AtomS3U (default) | G41 | G35 (SK6812) | `sdkconfig.defaults` |
| ESP32-S3-DevKitC-1 | G0 (BOOT) | G48 (v1.0) / G38 (v1.1) | `+ sdkconfig.defaults.devkitc` |
| Anything else | `idf.py menuconfig` → **espkb** | | |

## Using it

| Action | Effect | LED |
|---|---|---|
| Plug in | Starts reconnecting to bonded keyboards | off; flashes green when a keyboard connects |
| **Tap** button | Toggles the Wi-Fi AP and web UI | solid blue while on |
| **Hold** button 3s (or "Pair keyboard" in the UI) | Pairing mode for 60s | blinks magenta |

### Pairing a keyboard

1. Hold the button for 3 seconds. Or open the web UI and tap **Pair keyboard**,
   which lets you see a passkey if one is needed.
2. Put the keyboard into pairing mode.
3. If the keyboard asks for a passkey, the web UI shows a 6-digit code (it also
   goes to the serial log). Type it on the BT keyboard, then press Enter.

### BIOS spam

The **BIOS spam** box at the top of the web UI is for getting into firmware
setup or a boot menu:

1. Open the web UI, tick **Spam boot keys**, then reboot the machine.
2. The ESP cycles the selected keys: Del, Esc, F2 and F12 by default, with F1,
   F10, F11 and Enter available. It taps one key about every 80ms.
3. It stops by itself after 60 seconds. You can also untick the box or tap
   **Release all**.

The spam runs on the ESP, not in your phone's browser. It carries on if your
phone locks or drops off Wi-Fi, and through the USB disconnect/reconnect that
happens during a reboot.

Tip: turn off the keys you don't need. For example, Esc can close the boot menu
that F12 just opened. Common keys:

| Key | Opens |
|---|---|
| Del / F2 | Setup on most desktops, Dell and ASUS |
| F12 | Boot menu on Dell and Lenovo |
| F10 | Setup on HP |
| F11 | Boot menu on MSI and ASRock |
| F1 / Enter | Setup on Lenovo ThinkPads |
| Esc | Menu on HP |

### The web UI

Join the Wi-Fi network `espkb` (password `espkb1234`; **change it**, see below),
then open **http://192.168.4.1**.

- **Modifiers** (top bar, always visible): one tap applies the modifier to the
  next key, a second tap locks it, a third tap turns it off.
- **Keys/combos/full keyboard**: pressing and holding a button holds the key on
  the host. Holding F2/Del/F12 while a machine boots gets you into setup.
- **Type text**: type or paste text, then press **Send**. Turn on **Live** to
  send as you type. Autocorrect edits become backspaces plus retyping, and iOS
  smart quotes and dashes are converted to plain ASCII.
- Wi-Fi turns itself off after 15 minutes with no page open.

Text is converted to key presses using the **host's** keyboard layout, US by
default. If your machines use a UK layout, set it to UK in menuconfig, or
`"`, `@`, `#`, `\`, `~`, `|` and `£` will come out wrong.

## Building

Uses ESP-IDF v5.4 (v5.1 or later should work). With a local install:

```bash
idf.py set-target esp32s3
```

```bash
idf.py menuconfig
```

Go to **espkb**. Set the Wi-Fi password there, plus the layout and pins if you
need to.

```bash
idf.py build
```

If you don't have ESP-IDF installed, use Docker:

```bash
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.4.2 idf.py set-target esp32s3 build
```

For a DevKitC (or your own overlay file):

```bash
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.devkitc" build
```

### Flashing

Once this firmware runs, TinyUSB owns the USB port, so the board no longer shows
up as a serial port. To flash, put it into download mode:

- **Easiest, any board running espkb**: in the web UI, tap **Flash mode**. The
  device reboots into USB download mode. Replugging it returns to normal.
- **AtomS3U**: hold the side reset button for about 2s until the LED turns
  green. It then enumerates as a USB-JTAG/serial device.
- **DevKitC**: hold BOOT, tap RST, release BOOT. Or flash through the UART port.

With esptool (works with the Docker build, see Quick start):

```bash
cd build && python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem* -b 460800 write_flash @flash_args
```

Or with a local ESP-IDF install:

```bash
idf.py -p /dev/cu.usbmodem* flash
```

On macOS, Docker can't reach USB serial ports, which is why the flash step runs
outside the container.

Logs go to UART0 (TX = GPIO43 on the S3) at 115200 baud. The AtomS3U doesn't
break that pin out conveniently, so for debugging, a DevKitC's UART port is
easier.

## How it works

```
BLE keyboard(s) ──GATT notify──▶ ble_host.c ─┐
                                              ├─▶ keyboard.c (merge sources) ─▶ usb_kbd.c ─▶ USB host
phone ──WebSocket──▶ web.c ──keys / text──────┘                                 ▲
                                                    host LED state (Caps etc.) ─┘─▶ back to BLE kbds
```

- `ble_host.c`: NimBLE central. It scans continuously and reconnects bonded
  keyboards whenever they advertise. In pairing mode it also accepts new
  devices that advertise the HID service. After encrypting the link, it
  discovers the HID service, switches the keyboard to **Boot Protocol** (a
  fixed 8-byte report that can go straight to USB unchanged), and subscribes to
  its input reports.
- `keyboard.c`: Each input (every BLE keyboard, web key presses, web typing,
  BIOS spam) is a separate source. The USB report is the union of all of them, so you can
  hold Ctrl on the real keyboard and tap a key on the phone.
- `usb_kbd.c`: TinyUSB, one boot-keyboard interface with no report IDs (the
  shape BIOSes expect), plus remote wakeup.
- `web.c` + `web/index.html`: Soft-AP and a single WebSocket, so key
  down/up events stay in order. If the socket drops, every web-held key is
  released.

## Limitations / ideas

- **Boot protocol** means only standard keys come through. Media and consumer
  keys (volume, brightness) are dropped. Supporting them would need report-map
  parsing and a second USB HID interface.
- Keyboards that use **resolvable private addresses** (rotating MACs) may not
  reconnect automatically after pairing. Most keyboards use a fixed address.
- Only one new keyboard pairs per pairing session (by design, so it doesn't
  grab your neighbour's).
- The web UI can type into your computer, so keep the AP password set. Wi-Fi is
  only on while you've turned it on.
