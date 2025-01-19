/*
 * QEMU RISCV Hart Array
 *
 * Copyright (c) 2017 SiFive, Inc.
 *
 * Holds the state of a homogeneous array of RISC-V harts
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "system/reset.h"
#include "system/qtest.h"
#include "qemu/cutils.h"
#include "hw/sysbus.h"
#include "target/riscv/cpu.h"
#include "hw/qdev-properties.h"
#include "hw/riscv/riscv_hart.h"

static const Property riscv_harts_props[] = {
    DEFINE_PROP_UINT32("num-harts", RISCVHartArrayState, num_harts, 1),
    DEFINE_PROP_UINT32("hartid-base", RISCVHartArrayState, hartid_base, 0),
    DEFINE_PROP_STRING("cpu-type", RISCVHartArrayState, cpu_type),
    DEFINE_PROP_UINT64("resetvec", RISCVHartArrayState, resetvec,
                       DEFAULT_RSTVEC),
};

static void riscv_harts_cpu_reset(void *opaque)
{
    RISCVCPU *cpu = opaque;
    cpu_reset(CPU(cpu));
}

#ifndef CONFIG_USER_ONLY
static void csr_call(char *cmd, uint64_t cpu_num, int csrno, uint64_t *val)
{
    RISCVCPU *cpu = RISCV_CPU(cpu_by_arch_id(cpu_num));
    CPURISCVState *env = &cpu->env;

    int ret = RISCV_EXCP_NONE;
    if (strcmp(cmd, "get_csr") == 0) {
        ret = riscv_csrr(env, csrno, (target_ulong *)val);
    } else if (strcmp(cmd, "set_csr") == 0) {
        ret = riscv_csrrw(env, csrno, NULL, *(target_ulong *)val,
                MAKE_64BIT_MASK(0, TARGET_LONG_BITS));
    }

    g_assert(ret == RISCV_EXCP_NONE);
}

static bool csr_qtest_callback(CharBackend *chr, gchar **words)
{
    if (strcmp(words[0], "csr") == 0) {

        uint64_t cpu;
        uint64_t val;
        int rc, csr;

        rc = qemu_strtou64(words[2], NULL, 0, &cpu);
        g_assert(rc == 0);
        rc = qemu_strtoi(words[3], NULL, 0, &csr);
        g_assert(rc == 0);
        rc = qemu_strtou64(words[4], NULL, 0, &val);
        g_assert(rc == 0);
        csr_call(words[1], cpu, csr, &val);

        qtest_send_prefix(chr);
        qtest_sendf(chr, "OK 0 "TARGET_FMT_lx"\n", (target_ulong)val);

        return true;
    }

    return false;
}

static void riscv_cpu_register_csr_qtest_callback(void)
{
    static GOnce once;
    g_once(&once, (GThreadFunc)qtest_set_command_cb, csr_qtest_callback);
}
#endif

static bool riscv_hart_realize(RISCVHartArrayState *s, int idx,
                               char *cpu_type, Error **errp)
{
    object_initialize_child(OBJECT(s), "harts[*]", &s->harts[idx], cpu_type);
    qdev_prop_set_uint64(DEVICE(&s->harts[idx]), "resetvec", s->resetvec);
    s->harts[idx].env.mhartid = s->hartid_base + idx;
    qemu_register_reset(riscv_harts_cpu_reset, &s->harts[idx]);
    return qdev_realize(DEVICE(&s->harts[idx]), NULL, errp);
}

static void riscv_harts_realize(DeviceState *dev, Error **errp)
{
    RISCVHartArrayState *s = RISCV_HART_ARRAY(dev);
    int n;

    s->harts = g_new0(RISCVCPU, s->num_harts);

#ifndef CONFIG_USER_ONLY
    riscv_cpu_register_csr_qtest_callback();
#endif

    for (n = 0; n < s->num_harts; n++) {
        if (!riscv_hart_realize(s, n, s->cpu_type, errp)) {
            return;
        }
    }
}

static void riscv_harts_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, riscv_harts_props);
    dc->realize = riscv_harts_realize;
}

static const TypeInfo riscv_harts_info = {
    .name          = TYPE_RISCV_HART_ARRAY,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RISCVHartArrayState),
    .class_init    = riscv_harts_class_init,
};

static void riscv_harts_register_types(void)
{
    type_register_static(&riscv_harts_info);
}

type_init(riscv_harts_register_types)
