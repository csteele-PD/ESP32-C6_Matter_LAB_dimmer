# MatterC6Lab

Minimal ESP32-C6 Matter-over-Thread lab firmware for Hubitat commissioning
tests.

The first target is an ESP32-C6FH8 Supermini-style board with 8 MB flash. It
exposes a single Matter Dimmable Light endpoint and logs commissioning, On/Off,
Level Control, app size, and heap information to serial.

The Matter Basic Information identity is intentionally unique to this lab
firmware:

```text
manufacturer: ACLYS_LAB
model: MatterC6Lab
vendor id: 0xFFF1
product id: 0x8004
```

That keeps Hubitat from matching the ESP32 Wall Keypad or earlier lab board
fingerprints while this board is only acting as a generic Matter-over-Thread
test device.

## Build And Flash

```sh
source /Users/csteele/.espressif/v6.0.2/esp-idf/export.sh
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/cu.usbmodem2344101 flash monitor
```

## Pairing

The firmware logs the manual pairing code on boot. With the current defaults it
should be:

```text
34970112332
```

## Reset Matter State

Before pairing the board to a different hub, erase the NVS partition so the
stored Matter fabric state is clean:

```sh
python -m esptool --chip esp32c6 -p /dev/your-port erase-region 0x9000 0x6000
```

Then reset or power-cycle the board and pair it again. Holding BOOT/GPIO9 low
during normal firmware boot also erases NVS and restarts.
