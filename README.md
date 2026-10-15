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

4. **Put the AtomS3U in download mode.** Unplug it, hold the side button, plug
   it back in, let go when the LED turns green. Use a USB port that passes
   data (not a dock's charge-only port). Check that a port appeared:

   ```bash
   ls /dev/cu.usbmodem*
   ```

5. **Flash** (from the `build` directory):

   ```bash
   cd build && python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem* -b 460800 write_flash @flash_args
   ```

6. **Unplug and replug** the AtomS3U (it stays in download mode after
   flashing). It now shows up as **"espkb BLE-to-USB keyboard"**.
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
4. The LED flashes green once the keyboard is connected and set up. From then
   on it reconnects by itself whenever it's switched on and in range.

Keychron K-series: slide the back switch to BT, then hold `Fn+1` (or `2`/`3`
for the other slots) for about 4 seconds until the light flashes quickly. To
switch back to that slot later, tap `Fn+1`.

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

Once this firmware runs, the board shows up as a keyboard plus a small serial
port, not as a flashable device. For updates, use [OTA](#updating-over-wi-fi-ota)
instead. For the first flash (or after changing the partition table), put it
into download mode first.
The reliable way, especially through a dock, is a **power-up with the button
held**:

- **AtomS3U**: unplug it, hold the side button, plug it back in, let go when
  the LED turns green. It now enumerates as "USB JTAG/serial debug unit".
- **DevKitC**: hold BOOT while plugging in (or hold BOOT, tap RST).

There is also a software route (the web UI's **Flash mode** button, or `B` on
the serial port, or opening the serial port at 1200 baud). The board does reboot
into download mode, but some USB docks don't re-enumerate it afterwards, so you
may end up replugging anyway.

Use a USB port that passes data: a dock's charge-only or "charging" port
won't show the device at all, in either mode.

With esptool (works with the Docker build, see Quick start):

```bash
cd build && python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem* -b 460800 write_flash @flash_args
```

Or with a local ESP-IDF install:

```bash
idf.py -p /dev/cu.usbmodem* flash
```

After flashing, the board stays in download mode: **unplug and replug it**
(no button) to start the new firmware.

On macOS, Docker can't reach USB serial ports, which is why the flash step runs
outside the container.

### Updating over Wi-Fi (OTA)

After the first flash, updates go over Wi-Fi, no button needed. The flash has
two app slots: an update is written to the spare one, verified, then booted.

1. Build as usual. The image is `build/espkb.bin`.
2. Turn the web UI on (tap the button) and join the `espkb` network.
3. Either use the **Firmware** section of the page (pick the `.bin`, tap
   Update), or from a computer on that network:

   ```bash
   curl -X POST --data-binary @build/espkb.bin http://192.168.4.1/update
   ```

4. The device reboots into the new image. Wi-Fi is off after a reboot, so tap
   the button again to get the page back.

Safety nets: only images whose project name is `espkb` are accepted, the image
is checksum-verified before it's made bootable, and a freshly booted update is
"pending" until it has run cleanly for a bit (USB up, then 5 seconds; or a
minute with no host). If it crashes before then, the bootloader rolls back to
the previous slot. `s` on the serial port shows `ver=` (git describe) and
`slot=` so you can see what's running.

Changing `partitions.csv` or the bootloader config needs a wired flash.

### Serial port: status and commands

While running, the board exposes a USB serial port (`/dev/cu.usbmodem*` on
macOS, 115200 baud). Single characters are commands:

| Key | Does |
|---|---|
| `s` | Status: version and boot slot, uptime, USB, Wi-Fi joins/drops, keyboards, HID delivery counters, BLE detail, last reset and crash summary |
| `w` / `x` | Wi-Fi on / off |
| `p` | Pairing mode |
| `B` | Reboot into download mode |
| `?` | Help |

For example:

```bash
python3 -c "import serial,glob,time; s=serial.Serial(glob.glob('/dev/cu.usbmodem*')[0],115200,timeout=2); s.write(b's'); time.sleep(1.5); print(s.read(4000).decode())"
```

The full ESP log can also be sent to this port (menuconfig → espkb →
*Also send the full ESP log to the USB serial port*). It's off by default
because it deepens the stack use of system tasks; by default the log goes to
UART0 (TX = GPIO43), which the AtomS3U doesn't break out.

### If it crashes

A crash writes a core dump to flash. The `s` status shows a one-line summary
after the next boot (task, PC, backtrace addresses). To decode addresses:

```bash
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.4.2 xtensa-esp32s3-elf-addr2line -pfiaC -e build/espkb.elf 0x40375b9d 0x40381101
```

If the board crash-loops so fast it never shows up on USB, put it in download
mode and pull the dump out with esptool (offset from `partitions.csv`):

```bash
cd build && python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem* read_flash 0x320000 0x10000 coredump.bin
```

```bash
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.4.2 bash -c '. /opt/esp/idf/export.sh >/dev/null; esp-coredump info_corefile -t raw -c build/coredump.bin build/espkb.elf'
```

"A stack overflow in task X" means that task's `xTaskCreate` stack size needs
raising. Mysterious crashes inside Wi-Fi or Bluetooth with garbage pointers
were, in this project's history, exactly that: a small task overflowing into
the heap block below it.

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
  discovers the HID service, reads the Report Map, switches the keyboard to
  **Boot Protocol** (a fixed 8-byte report that can go straight to USB
  unchanged) unless Report Protocol is needed for media keys, and subscribes to
  its input reports.
- `keyboard.c`: Each input (every BLE keyboard, web key presses, web typing,
  BIOS spam) is a separate source. The USB report is the union of all of them, so you can
  hold Ctrl on the real keyboard and tap a key on the phone.
- `usb_kbd.c`: TinyUSB, one boot-keyboard interface with no report IDs (the
  shape BIOSes expect) plus a consumer-control interface for media keys, and
  remote wakeup.
- `web.c` + `web/index.html`: Soft-AP and a single WebSocket, so key
  down/up events stay in order. If the socket drops, every web-held key is
  released.

## Limitations / ideas

- Typing from the phone is paced by how often the host collects USB reports:
  each key press and release goes out as soon as the host has taken the
  previous one (`tud_hid_report_complete_cb`). That's 1 ms on a direct port,
  up to 8 ms through some hubs/docks, so expect 60-500 characters a second.
  If a host drops characters, set an extra delay with menuconfig → espkb →
  "Extra delay between key events".

- Media keys (volume, play/pause, brightness...) are forwarded through a second
  USB HID interface (consumer control). The keyboard's Report Map is parsed to
  find its consumer report; if the map has both a standard keyboard report and a
  consumer report the keyboard is left in Report Protocol, otherwise it is put in
  Boot Protocol and media keys are not available. Only one media key at a time
  is sent, and keyboards whose Report Protocol keyboard report is a bitmap (NKRO)
  rather than the standard 8 bytes still need Boot Protocol, so lose media keys.
- Keyboards that use **resolvable private addresses** (rotating MACs) may not
  reconnect automatically after pairing. Most keyboards use a fixed address.
- Only one new keyboard pairs per pairing session (by design, so it doesn't
  grab your neighbour's).
- The web UI can type into your computer, so keep the AP password set. Wi-Fi is
  only on while you've turned it on.
