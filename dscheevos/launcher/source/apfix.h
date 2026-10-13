/* AP-fix patches in hardcore. nds-bootstrap patches some games at boot to
 * get past their anti-piracy checks: with its built-in patches (apfix.pck
 * inside its .nds) or with a user file in _nds/nds-bootstrap/apFix/. A user
 * file could patch anything after we hashed the clean ROM, so the DScheevos
 * nds-bootstrap ignores user files in a hardcore session. */
#ifndef DSC_APFIX_H
#define DSC_APFIX_H

#define APFIX_NONE      0   /* no user file for this game */
#define APFIX_BUILTIN   1   /* a user file, but nds-bootstrap has its own fix: hardcore uses that */
#define APFIX_USER_ONLY 2   /* only a user file: hardcore would boot the game unpatched */

/* Looks for the user files nds-bootstrap would use for this ROM (same names
 * and order as its loadApFix()); `name` gets the first one found. */
int apfix_check(const char* rom_path, const char* bootstrap_nds, char* name, int name_size);

#endif
