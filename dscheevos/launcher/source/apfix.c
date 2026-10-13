#include "apfix.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define APFIX_DIR "sd:/_nds/nds-bootstrap/apFix/"

static int read_at(FILE* f, long pos, void* buf, size_t n)
{
  return fseek(f, pos, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

/* nitro:/apfix.pck inside nds-bootstrap's .nds: offset and size, from the
 * root directory of its file name table. */
static int find_pck(FILE* f, uint32_t* start, uint32_t* size)
{
  uint32_t fnt, fat, sub, range[2];
  uint16_t id;
  uint8_t len;
  char name[128];
  long pos;
  if (!read_at(f, 0x40, &fnt, 4) || !read_at(f, 0x48, &fat, 4)
   || !read_at(f, fnt, &sub, 4) || !read_at(f, fnt + 4, &id, 2))
    return 0;
  pos = (long)fnt + sub;
  for (;;) {
    if (!read_at(f, pos, &len, 1) || len == 0)
      return 0;
    if (!read_at(f, pos + 1, name, len & 0x7F))
      return 0;
    pos += 1 + (len & 0x7F);
    if (len & 0x80) {            /* subdirectory: skip its id */
      pos += 2;
      continue;
    }
    if (len == 9 && !memcmp(name, "apfix.pck", 9)) {
      if (!read_at(f, fat + id * 8u, range, 8) || range[1] <= range[0])
        return 0;
      *start = range[0];
      *size = range[1] - range[0];
      return 1;
    }
    id++;
  }
}

/* Whether apfix.pck has an entry for this game: same match as nds-bootstrap
 * (title ID, and header CRC or FFFF for any). */
static int builtin_fix(const char* bootstrap_nds, const char tid[4], uint16_t crc)
{
  FILE* f = fopen(bootstrap_nds, "rb");
  uint32_t start, size, count, i;
  int found = 0;
  if (!f)
    return 0;
  if (find_pck(f, &start, &size) && size >= 16) {
    char magic[4];
    if (read_at(f, start, magic, 4) && !memcmp(magic, ".PCK", 4) && read_at(f, start + 4, &count, 4)
     && count <= (size - 16) / 16) {
      for (i = 0; i < count && !found; i++) {
        uint8_t e[14];
        uint32_t off, len;
        uint16_t ecrc;
        if (!read_at(f, start + 16 + i * 16, e, sizeof(e)))
          break;
        memcpy(&ecrc, e + 4, 2);
        memcpy(&off, e + 6, 4);
        memcpy(&len, e + 10, 4);
        found = !memcmp(e, tid, 4) && (ecrc == 0xFFFF || ecrc == crc) && off > 0 && len > 0;
      }
    }
  }
  fclose(f);
  return found;
}

int donor20_needed(const char* rom_path)
{
  char tid[3];
  uint8_t unit = 0xFF;
  uint32_t arm7_size = 0;
  FILE* f = fopen(rom_path, "rb");
  int ok;
  if (!f)
    return 0;
  ok = read_at(f, 0x0C, tid, 3) && read_at(f, 0x12, &unit, 1) && read_at(f, 0x3C, &arm7_size, 4);
  fclose(f);
  return ok && !memcmp(tid, "AYI", 3) && unit == 0 && arm7_size == 0x25F70;
}

int apfix_check(const char* rom_path, const char* bootstrap_nds, char* name, int name_size)
{
  char tid[5] = { 0 }, path[300];
  uint16_t crc = 0;
  const char* base = strrchr(rom_path, '/');
  FILE* f = fopen(rom_path, "rb");
  int i;
  if (!f)
    return APFIX_NONE;
  if (!read_at(f, 0x0C, tid, 4) || !read_at(f, 0x15E, &crc, 2))
    tid[0] = '\0';
  fclose(f);
  base = base ? base + 1 : rom_path;
  for (i = 0; i < 6; i++) {
    switch (i) {
    case 0: snprintf(name, name_size, "%s.ips", base); break;
    case 1: snprintf(name, name_size, "%s.bin", base); break;
    case 2: snprintf(name, name_size, "%s-%04X.ips", tid, crc); break;
    case 3: snprintf(name, name_size, "%s-FFFF.ips", tid); break;
    case 4: snprintf(name, name_size, "%s-%04X.bin", tid, crc); break;
    default: snprintf(name, name_size, "%s-FFFF.bin", tid); break;
    }
    if (i >= 2 && !tid[0])
      break;
    snprintf(path, sizeof(path), APFIX_DIR "%s", name);
    if (access(path, F_OK) == 0)
      return builtin_fix(bootstrap_nds, tid, crc) ? APFIX_BUILTIN : APFIX_USER_ONLY;
  }
  name[0] = '\0';
  return APFIX_NONE;
}
