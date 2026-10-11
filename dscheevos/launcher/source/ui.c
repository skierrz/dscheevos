#include "ui.h"

#include <nds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static PrintConsole top, bottom;

void ui_init(void)
{
  videoSetMode(MODE_0_2D);
  videoSetModeSub(MODE_0_2D);
  vramSetBankA(VRAM_A_MAIN_BG);
  vramSetBankC(VRAM_C_SUB_BG);
  consoleInit(&top, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
  consoleInit(&bottom, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 3, false, true);
  consoleSelect(&top);
}

void ui_header(const char* game, const char* line2)
{
  consoleSelect(&top);
  consoleClear();
  printf("\x1b[0;0H\x1b[33;1m DScheevos\x1b[39;0m  RetroAchievements\n");
  printf("--------------------------------");
  printf("\n %.30s\n", game ? game : "");
  if (line2)
    printf(" %.30s\n", line2);
  printf("\n");
}

void ui_status(const char* fmt, ...)
{
  va_list ap;
  consoleSelect(&top);
  printf(" ");
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
}

void ui_clear_bottom(void)
{
  consoleSelect(&bottom);
  consoleClear();
}

void ui_bottom(const char* fmt, ...)
{
  va_list ap;
  consoleSelect(&bottom);
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}

int ui_choose(const char* message, const char* a_label, const char* b_label)
{
  ui_clear_bottom();
  ui_bottom("\n %s\n\n", message);
  if (a_label) ui_bottom(" \x1b[32;1mA\x1b[39;0m  %s\n", a_label);
  if (b_label) ui_bottom(" \x1b[31;1mB\x1b[39;0m  %s\n", b_label);
  for (;;) {
    swiWaitForVBlank();
    scanKeys();
    if (a_label && (keysDown() & KEY_A)) return 'A';
    if (b_label && (keysDown() & KEY_B)) return 'B';
  }
}

int ui_wait(const char* message, int frames)
{
  ui_clear_bottom();
  ui_bottom("\n %s\n", message);
  while (frames-- > 0) {
    int k;
    swiWaitForVBlank();
    scanKeys();
    k = keysDown() & (KEY_A | KEY_B | KEY_START | KEY_SELECT);
    if (k) return k;
  }
  return 0;
}

int ui_input(const char* prompt, char* buf, int size, int masked)
{
  Keyboard* kb;
  int len = (int)strlen(buf);
  ui_clear_bottom();
  kb = keyboardDemoInit();
  keyboardShow();
  for (;;) {
    int c, i;
    consoleSelect(&top);
    printf("\x1b[20;0H\x1b[2K %s\n\x1b[2K > ", prompt);
    for (i = 0; i < len; i++) putchar(masked ? '*' : buf[i]);
    printf("_");
    do {
      swiWaitForVBlank();
      scanKeys();
      if (keysDown() & KEY_B && len == 0) { keyboardHide(); return 0; }
      if (keysDown() & KEY_START) c = '\n';
      else c = keyboardUpdate();
    } while (c <= 0 && c != '\n');
    if (c == '\n' || c == DVK_ENTER) {
      if (len > 0) break;
    } else if (c == '\b' || c == DVK_BACKSPACE) {
      if (len > 0) buf[--len] = '\0';
    } else if (c >= 32 && c < 127 && len < size - 1) {
      buf[len++] = (char)c;
      buf[len] = '\0';
    }
  }
  (void)kb;
  keyboardHide();
  consoleSelect(&top);
  printf("\x1b[20;0H\x1b[2K\n\x1b[2K");
  return 1;
}
