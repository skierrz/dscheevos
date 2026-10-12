/* display-lab ARM7: the usual libnds services (input, SD, power button) plus
 * one test command: write the camera LED register (BPTWL 31h) and report how
 * long the I2C write took, in 33.5 MHz ticks. */
#include <nds.h>

static void cameraLed(u32 value, void* userdata) {
	(void)userdata;
	TIMER0_CR = 0;
	TIMER1_CR = 0;
	TIMER0_DATA = 0;
	TIMER1_DATA = 0;
	TIMER1_CR = TIMER_ENABLE | TIMER_CASCADE;
	TIMER0_CR = TIMER_ENABLE | TIMER_DIV_1;
	i2cWriteRegister(I2C_PM, I2CREGPM_CAMLED, (u8)value);
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
	fifoSetValue32Handler(FIFO_USER_01, cameraLed, 0);
	while (!exitflag) {
		swiWaitForVBlank();
	}
	if (isDSiMode()) i2cWriteRegister(I2C_PM, I2CREGPM_CAMLED, 0);
	return 0;
}
