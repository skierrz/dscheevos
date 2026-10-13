/* display-lab ARM7: the usual libnds services (input, SD, power button) plus
 * one test command: write an LED register of the power-management chip
 * (BPTWL: 30h Wi-Fi LED, 31h camera LED, 63h power LED colour) and report how
 * long the I2C write took, in 33.5 MHz ticks. Only those three registers;
 * all are volatile. 30h keeps its other bits (bit 4 enables the Wi-Fi chip). */
#include <nds.h>

static void ledWrite(u32 value, void* userdata) {
	(void)userdata;
	u8 reg = (u8)(value >> 8);
	u8 val = (u8)value;
	if (reg != 0x30 && reg != 0x31 && reg != 0x63) {
		fifoSendValue32(FIFO_USER_01, 0xFFFFFFFF);
		return;
	}
	if (reg == 0x30) val = (i2cReadRegister(I2C_PM, 0x30) & ~3) | (val & 3);
	TIMER0_CR = 0;
	TIMER1_CR = 0;
	TIMER0_DATA = 0;
	TIMER1_DATA = 0;
	TIMER1_CR = TIMER_ENABLE | TIMER_CASCADE;
	TIMER0_CR = TIMER_ENABLE | TIMER_DIV_1;
	i2cWriteRegister(I2C_PM, reg, val);
	TIMER0_CR = 0;
	const u32 ticks = TIMER0_DATA | (u32)TIMER1_DATA << 16;
	TIMER1_CR = 0;
	fifoSendValue32(FIFO_USER_01, ticks);
}

/* A7: ARM7 interrupt latency while the ARM9 runs NDMA traffic. Timer 2
 * fires at 1 kHz; its IRQ reads how far the timer has run since it
 * overflowed, i.e. how late the IRQ was taken. A steady tone plays on PSG
 * channel 8 meanwhile, so a glitch is audible on the video. */
#define A7_RELOAD (65536 - 33514)           /* 1 kHz at 33.514 MHz */
static volatile u32 a7_max, a7_sum, a7_n;

static void a7Tick(void) {
	const u32 late = (u32)TIMER2_DATA - A7_RELOAD;
	if (late > a7_max) a7_max = late;
	a7_sum += late;
	a7_n++;
}

static void a7Command(u32 cmd, void* userdata) {
	(void)userdata;
	if (cmd == 1) {
		a7_max = a7_sum = a7_n = 0;
		SCHANNEL_TIMER(8) = SOUND_FREQ(440 * 8);
		SCHANNEL_CR(8) = SCHANNEL_ENABLE | SOUND_VOL(40) | SOUND_PAN(64) | SOUND_FORMAT_PSG | (3 << 24);
		TIMER_DATA(2) = A7_RELOAD;
		TIMER_CR(2) = TIMER_ENABLE | TIMER_IRQ_REQ | TIMER_DIV_1;
	} else {
		TIMER_CR(2) = 0;
		SCHANNEL_CR(8) = 0;
		fifoSendValue32(FIFO_USER_02, a7_max);
		fifoSendValue32(FIFO_USER_02, a7_n ? a7_sum / a7_n : 0);
		fifoSendValue32(FIFO_USER_02, a7_n);
	}
}

static void vcount(void) {
	inputGetAndSend();
}

static volatile bool exitflag = false;
static void powerButton(void) {
	exitflag = true;
}

int main(void) {
	readUserSettings();
	ledBlink(0);
	irqInit();
	initClockIRQ();
	fifoInit();
	touchInit();
	SetYtrigger(80);
	installSystemFIFO();
	irqSet(IRQ_VCOUNT, vcount);
	irqSet(IRQ_TIMER2, a7Tick);
	irqEnable(IRQ_VBLANK | IRQ_VCOUNT | IRQ_TIMER2);
	powerOn(POWER_SOUND);                      /* as libnds's default ARM7 does */
	writePowerManagement(PM_CONTROL_REG, (readPowerManagement(PM_CONTROL_REG) & ~PM_SOUND_MUTE) | PM_SOUND_AMP);
	REG_SOUNDCNT = SOUND_ENABLE | SOUND_VOL(0x7F);
	setPowerButtonCB(powerButton);
	fifoSetValue32Handler(FIFO_USER_01, ledWrite, 0);
	fifoSetValue32Handler(FIFO_USER_02, a7Command, 0);
	while (!exitflag) {
		swiWaitForVBlank();
	}
	if (isDSiMode()) {
		i2cWriteRegister(I2C_PM, I2CREGPM_CAMLED, 0);
		i2cWriteRegister(I2C_PM, 0x63, 0);              /* power LED back to automatic */
	}
	return 0;
}
