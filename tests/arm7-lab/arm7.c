/* arm7-lab ARM7: libnds' usual ARM7 services (the ARM9 needs them for the SD
 * card), plus: a FIFO_USER_01 value is a lab7_params_t*; run the payload the
 * ARM9 copied to LAB7_PAYLOAD_BASE with it, then answer on FIFO_USER_01. */
#include <nds.h>
#include "lab7.h"

typedef void (*payload_fn)(lab7_params_t*);

static volatile int exitflag;
static void power_button(void) { exitflag = 1; }
static void vcount_handler(void) { inputGetAndSend(); }

int main(void)
{
  readUserSettings();
  ledBlink(0);
  irqInit();
  initClockIRQ();
  fifoInit();
  touchInit();
  SetYtrigger(80);
  installSystemFIFO();
  irqSet(IRQ_VCOUNT, vcount_handler);
  irqEnable(IRQ_VBLANK | IRQ_VCOUNT);
  setPowerButtonCB(power_button);

  while (!exitflag) {
    if (fifoCheckValue32(FIFO_USER_01)) {
      lab7_params_t* p = (lab7_params_t*)fifoGetValue32(FIFO_USER_01);
      ((payload_fn)LAB7_PAYLOAD_BASE)(p);
      fifoSendValue32(FIFO_USER_01, 1);
    }
    swiWaitForVBlank();
  }
  return 0;
}
