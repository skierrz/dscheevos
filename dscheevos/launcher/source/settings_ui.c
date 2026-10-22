/* LAB (strip-lab): the notice bar's settings on the console (bottom screen):
 * from the launcher's Settings, or SELECT on the "Have fun!" screen. Changes are saved to
 * dscheevos.ini right away (the launcher's own file; before boot). */
#include <nds.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "settings_ui.h"
#include "ui.h"

enum { ROW_SIZE, ROW_EDGE, ROW_BADGES, ROWS };

static const char* value(const dsc_settings_t* s, int row)
{
  switch (row) {
  case ROW_SIZE: return s->notice_big ? "Big" : "Thin";
  case ROW_EDGE: return s->notice_edge == 1 ? "Top" : s->notice_edge == 2 ? "Bottom" : "Auto";
  default: return s->badges ? "On" : "Off";
  }
}

static void draw(const dsc_settings_t* s, int at)
{
  static const char* const names[ROWS] = { "Notice bar", "Position", "Badges" };
  int i;
  ui_clear_bottom();
  ui_bottom("\n \x1b[33;1mDScheevos settings\x1b[39;0m\n\n");
  for (i = 0; i < ROWS; i++)
    ui_bottom(" %s %-11s \x1b[32;1m%s\x1b[39;0m\n", i == at ? ">" : " ", names[i], value(s, i));
  ui_bottom("\n UP/DOWN pick, A/LEFT/RIGHT\n change, START or B: done\n\n");
  switch (at) {
  case ROW_SIZE:
    ui_bottom(" Thin: one line, small badge.\n Big: title, description and\n a bigger badge.\n\n"
              " A game that can't show your\n choice gets the one it can.");
    break;
  case ROW_EDGE:
    ui_bottom(" Which half of the screen the\n bar may use. Auto: wherever\n it shows best (bottom first).");
    break;
  default:
    ui_bottom(" Download each achievement's\n badge before the game starts\n (kept on the SD card).");
    break;
  }
}

int notice_settings_menu(dsc_settings_t* s)
{
  const dsc_settings_t before = *s;
  int at = 0;
  draw(s, at);
  for (;;) {
    int k, step = 0;
    swiWaitForVBlank();
    scanKeys();
    k = keysDown();
    if (k & (KEY_START | KEY_B))
      break;
    if (k & KEY_UP) { at = (at + ROWS - 1) % ROWS; draw(s, at); continue; }
    if (k & KEY_DOWN) { at = (at + 1) % ROWS; draw(s, at); continue; }
    if (k & (KEY_A | KEY_RIGHT)) step = 1;
    if (k & KEY_LEFT) step = -1;
    if (!step)
      continue;
    switch (at) {
    case ROW_SIZE: s->notice_big = !s->notice_big; break;
    case ROW_EDGE: s->notice_edge = (s->notice_edge + 3 + step) % 3; break;
    default: s->badges = !s->badges; break;
    }
    draw(s, at);
  }
  ui_clear_bottom();
  if (memcmp(&before, s, sizeof(*s)) == 0)
    return 0;
  settings_save(s);
  return 1;
}
