/* Float-operand variant (for information only, not shipped): rcheevos'
 * rc_operand_t stores its constant as a float instead of a double. Force-included
 * into every compile by realsets.sh FLOAT=1; rcheevos' source is unchanged. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include <assert.h>
#include <limits.h>
#include <float.h>
#include <errno.h>
#define double float
#include "rc_runtime_types.h"
#undef double
