/* qemu TCG plugin: counts guest instructions between two addresses.
 * plugin args mb=<hex>,me=<hex>: counting starts when the instruction at mb runs and
 * "PHASE n count" goes to stderr when the one at me runs (tests/perf/realsets.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "qemu-plugin.h"
QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;
static uint64_t count, start; static uint64_t mb, me; static int phase;
static void on_b(unsigned int v, void* u){ (void)v;(void)u; start = count; }
static void on_e(unsigned int v, void* u){ (void)v;(void)u; fprintf(stderr, "PHASE %d %llu\n", phase++, (unsigned long long)(count - start)); }
static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb* tb){
  size_t n = qemu_plugin_tb_n_insns(tb), i;
  for (i = 0; i < n; i++) {
    struct qemu_plugin_insn* in = qemu_plugin_tb_get_insn(tb, i);
    uint64_t a = qemu_plugin_insn_vaddr(in) & ~1ull;
    qemu_plugin_register_vcpu_insn_exec_inline(in, QEMU_PLUGIN_INLINE_ADD_U64, &count, 1);
    if (a == mb) qemu_plugin_register_vcpu_insn_exec_cb(in, on_b, QEMU_PLUGIN_CB_NO_REGS, NULL);
    if (a == me) qemu_plugin_register_vcpu_insn_exec_cb(in, on_e, QEMU_PLUGIN_CB_NO_REGS, NULL);
  }
}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t* info, int argc, char** argv){
  int i; for (i = 0; i < argc; i++) { if (!strncmp(argv[i],"mb=",3)) mb=strtoull(argv[i]+3,0,16); if (!strncmp(argv[i],"me=",3)) me=strtoull(argv[i]+3,0,16); }
  qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans); return 0; }
