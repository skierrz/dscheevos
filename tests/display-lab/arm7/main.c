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
	irqEnable(IRQ_VBLANK | IRQ_VCOUNT);
	setPowerButtonCB(powerButton);
	fifoSetValue32Handler(FIFO_USER_01, ledWrite, 0);
	while (!exitflag) {
		swiWaitForVBlank();
	}
	if (isDSiMode()) {
		i2cWriteRegister(I2C_PM, I2CREGPM_CAMLED, 0);
		i2cWriteRegister(I2C_PM, 0x63, 0);              /* power LED back to automatic */
	}
	return 0;
}
