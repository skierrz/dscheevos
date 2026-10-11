#include "rc_runtime.h"
#include "rc_runtime_types.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
/* read MemAddr strings one per line, print parse result */
int main(void){ static char line[65536]; int bad=0,n=0;
  while (fgets(line,sizeof line,stdin)) { line[strcspn(line,"\n")]=0; n++;
    int r=rc_trigger_size(line); if (r<0){ bad++; if (bad<6) printf("%d: %s\n", r, line); } }
  printf("%d/%d bad\n",bad,n); return 0; }
