# DSi / DSi XL LEDs (facts only)
G = (local scratch) ; NB = external/nds-bootstrap ; CE = NB/retail/cardenginei/arm7/source

## Common interface
- I2C device 4Ah = BPTWL (DSi) / "BP UTL-1" (DSi XL) (G:31372, 31703-31709). Register map G:31594-31641. ARM7 side port (G:24107 "ARM7 DSi I2C Bus"; I2C_CNT 4004501h G:31405-31418). Camera I2C needs ARM9-side clock; BPTWL is ARM7 I2C.
- Timing (G:31588-31592, 31446-31452): BPTWL needs swiWaitByLoop(180h) after each I2C byte (90h if BPTWL[00h] says Fast, >=21h); same delay needed between SPI Powerman writes and BPTWL writes. Firmware does 8 retries per command (G:31455). No time in us/ms documented in GBATEK.
- libnds-derived code: CE/libnds/i2c.c:67-70 (I2C_PM delay = swiDelay(0x180)), :93-110 write = select device, select reg, delay, data, stop; 8 retries; i2cWaitBusy unbounded in i2cDelay (:~30). One write = 3 bytes (dev, reg, data) each followed by the delay. hb/arm7/include/i2c.h:47-60 lists I2C_PM=0x4A, I2CREGPM_CAMLED 0x31, BATTERY 0x20, VOL 0x40 (no WIFI/power LED defines).
- Repo estimate (not measured): research-hw.md:253 "about 0.2-0.4 ms per write (OR's estimate, unmeasured)"; WORKLIST.md:91,113 "unmeasured".

## 1. Power LED (DSi; DSi XL same BPTWL-style reg)
Physical: GBATEK DSi diagram G:350-368 marks "LEDS" at left of the hinge/gap row (lower-left of the hinge, "POWER o" by the lower-left of the bottom half). Colours implied by regs: green/blue, red, purple. iFixit/dsibrew not retrievable (iFixit URL returned unrelated page; 3dbrew 403). XL differences: not found.
Control: BPTWL[63h] "Force Power LED (0=Automatic, 1=Red, 2=Blue, 3=Purple)" R/W (G:31625). Automatic = MCU picks (charging/low battery/normal). Calibration mode BPTWL[60h] bit0=1 -> Purple LED (G:31624).
Old SPI path: PM reg0 bit4/5 power-LED blink enable/speed; "DSi: Power LED Blinking isn't supported, neither in NDS nor DSi mode" (G:13765-13767); SPI reg1 bit0 battery LED status (G:13771). G:23876-23878: LED control moved to BPTWL via I2C; power-LED blink for wifi no longer works; G:23927.
Capabilities: no brightness, no hardware blink documented on DSi. Values 0-3 only per GBATEK.
Shared: nds-bootstrap ROM-read LED option 2 writes 63h = 0xFF (purple) then 0x00 (revert) (CE/cardengine.c:709,724,741,753); reads 63h first, if ==0xFF treats as purple (:696-698). Note it uses 0xFF/0x00, not GBATEK's 0-3. Gated by consoleModel<2 and !i2bricked (:683). (consoleModel field: hb/common/include/cardengine_header_arm7.h:75; meaning of <2 not read.) inGameMenu.c:91-92 has SPI "sleep LED (Does not work)" writePowerManagement(PM_CONTROL_REG, PM_LED_CONTROL(1)).
Volatility: not stated for 63h. Register 41h comment (below) is the only "nonvolatile" mark.
3DS: BPTWL emulation list (G:31643-31656) does NOT include 63h ("Other - Unused (FFh)"). 3DS has own Power LED via MCU[29h] (3DS-mode bus 1 only? G:68209 lists bus 1:4Ah as 3DS-mode MCU; bus 0:4Ah = BPTWL emulation).

## 2. Wi-Fi LED (DSi, DSi XL)
Physical: not on GBATEK DSi diagram. G:23927 "Power LED blink feature (for wifi) is replaced by a separate wifi LED". Location/colour: not retrieved.
Control: BPTWL[30h] R/W "Wifi LED (0/2=Off, 1=On, 3=BlinkOnTraffic) and bit4=SDIO enable" (G:31613). G:23878 "Wifi LED seems to blink automatically on Wifi access"; G:41901, 43180-43186: signals WL_RXPE/WL_TXPE blink the LED if enabled; 30h also may gate Wifi reset / NDS vs DSi wifi mode; G:23900 "also needed to enable Atheros Wifi SDIO". Value 0x12 = LED off with bit4 kept; 0x13 = on (CE/cardengine.c:706,721,744).
Capabilities: on/off, blink-on-traffic (automatic by hardware). No brightness/colour. Blink-on-traffic rate: not documented.
Shared: Wi-Fi hardware/firmware; nds-bootstrap ROM-read LED option 1 (CE/cardengine.c:706,721); this fork's own cues "Wi-Fi LED patterns" (CE/cardengine.c ~1885-1976, comment says cues use opposite of normal state, saves BPTWL[30h] and restores, off while the ROM-read option drives the LED). Games in DS mode cannot use I2C (SCFG-locked); DS-mode wifi LED is automatic. Settings "Wifi Enable" flag only disables the LED (G:41901).
Risks: bit4 = SDIO enable for Atheros; writing 30h without bit4 may cut Wi-Fi (G:31613, 23900). GPIO_WIFI 4004C04h bit8 separate (G:63110-63120).
3DS: not in emulated list (G:31646-31656). 3DS-mode MCU[2Ah] Wifi LED state (G:68710-68713) is bus 1.

## 3. Camera LED (DSi, DSi XL)
Physical: GBATEK DSi diagram G:350-358: "CAM (O) and LED (o) (on backside)" - on the outside of the top lid next to the outer camera (also G:31594 comment 02h "30h=AlternateCamLedHardware?"). Not visible to the player with lid open facing screens (WORKLIST.md:44 owner note says it faces outward). Colour: not retrieved.
Control: BPTWL[31h] R/W "Camera LED (00h=Off, 01h=On, 02h=Blink)" (G:31614). Blink rate not documented. Hardware blink: one write per state. CE: 0x01/0x00 (cardengine.c:712,727). G:32693: "don't forget to activate the Camera LED via BPTWL" when using the external camera (so it is also meant as camera-activity indicator).
Shared: nds-bootstrap ROM-read LED option 3; DSi firmware/camera apps. DS-mode games: no.
3DS TWL mode: emulated "31h R/W Camera LED (bit0,1)" (G:31653). 3DS-mode MCU[2Bh] camera LED: 00 off, 01 slow blink, 02 on, 03 set via BPTWL[31h] (DSi mode), 04 flash once, 05 off once; "Camera LED exists only in older 3DS, not in New3DS" (G:68716-68725 region) - so on New 3DS XL, 31h may have no LED (hardware test H9 pending: research-hw.md:361-368).

## 4. Charge/power LED(s)
G:392-401 3DS diagram "LEDs (power/charge)". DSi charge indicator: no register found beyond automatic MCU behaviour; BPTWL[20h] bit7 = charging, bits0-3 battery level (G:31604, 31692-31701).

## 5. Backlight (not LED but nonvolatile note)
BPTWL[40h] volume, [41h] backlight 00h-04h R/W, marked ";\nonvolatile!" (G:31616-31617). Also G:18786.

## 6. 3DS
- BPTWL emulated regs in DSi mode: 00,10,11,12,20,31,40,70; others unused FFh (G:31643-31656). Device list: bus 0:4Ah = BPTWL emulation, bus 1:4Ah = 3DS-mode MCU (G:68209-68212).
- Notification RGB LED: MCU[2Dh] array (G:68737-68750), power LED MCU[29h] (G:68693-68708), wifi MCU[2Ah], camera MCU[2Bh], 3D LED MCU[2Ch] are all 3DS-mode (bus 1) registers. GBATEK shows no DSi-mode path to them; whether DSi-mode ARM7 can reach bus 1 is not stated in what I read.
- Chip names: DSi BPTWL reg00=33h; DSi XL "BP UTL-1" reg00=BBh/B7h; New3DS XL 35h (G:31703-31709). DSi XL extra regs 50h/51h output to I2C device 30h (G:31384, 31619-31620).

## Not found
iFixit/dsibrew/3dbrew pages could not be retrieved (403/off-topic); physical colours/locations for Wi-Fi LED, DSi XL differences, I2C write duration in microseconds are undocumented in the sources read.
