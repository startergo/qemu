/*
 * QEMU AEHD support
 *
 * Copyright IBM, Corp. 2008
 *           Red Hat, Inc. 2008
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *  Glauber Costa     <gcosta@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"

#include "qemu/atomic.h"
#include "qemu/option.h"
#include "qemu/config-file.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "exec/gdbstub.h"
#include "sysemu/runstate.h"
#include "sysemu/cpus.h"
#include "qemu/bswap.h"
#include "exec/memory.h"
#include "exec/ram_addr.h"
#include "exec/address-spaces.h"
#include "qemu/event_notifier.h"
#include "qemu/main-loop.h"
#include "trace.h"
#include "hw/irq.h"
#include "qapi/visitor.h"
#include "qapi/qapi-types-common.h"
#include "qapi/qapi-visit-common.h"
#include "sysemu/hw_accel.h"
#include "sysemu/aehd-interface.h"
#include "aehd-accel-ops.h"
#include "aehd_int.h"

#include "hw/boards.h"

#ifdef DEBUG_AEHD
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

struct AEHDParkedVcpu {
    unsigned long vcpu_id;
    HANDLE aehd_fd;
    QLIST_ENTRY(AEHDParkedVcpu) node;
};

AEHDState *aehd_state;
bool aehd_allowed;

static AEHDSlot *aehd_get_free_slot(AEHDMemoryListener *gml)
{
    AEHDState *s = aehd_state;
    int i;

    for (i = 0; i < s->nr_slots; i++) {
        if (gml->slots[i].memory_size == 0) {
            return &gml->slots[i];
        }
    }

    return NULL;
}

bool aehd_has_free_slot(MachineState *ms)
{
    AEHDState *s = AEHD_STATE(ms->accelerator);

    return aehd_get_free_slot(&s->memory_listener);
}

static AEHDSlot *aehd_alloc_slot(AEHDMemoryListener *gml)
{
    AEHDSlot *slot = aehd_get_free_slot(gml);

    if (slot) {
        return slot;
    }

    fprintf(stderr, "%s: no free slot available\n", __func__);
    abort();
}

static AEHDSlot *aehd_lookup_matching_slot(AEHDMemoryListener *gml,
                                         hwaddr start_addr,
                                         hwaddr size)
{
    AEHDState *s = aehd_state;
    int i;

    for (i = 0; i < s->nr_slots; i++) {
        AEHDSlot *mem = &gml->slots[i];

        if (start_addr == mem->start_addr && size == mem->memory_size) {
            return mem;
        }
    }

    return NULL;
}

/*
 * Calculate and align the start address and the size of the section.
 * Return the size. If the size is 0, the aligned section is empty.
 */
static hwaddr aehd_align_section(MemoryRegionSection *section,
                                hwaddr *start)
{
    hwaddr size = int128_get64(section->size);
    hwaddr delta, aligned;

    /*
     * kvm works in page size chunks, but the function may be called
     * with sub-page size and unaligned start address. Pad the start
     * address to next and truncate size to previous page boundary.
     */
    aligned = ROUND_UP(section->offset_within_address_space,
                       qemu_real_host_page_size());
    delta = aligned - section->offset_within_address_space;
    *start = aligned;
    if (delta > size) {
        return 0;
    }

    return (size - delta) & qemu_real_host_page_mask();
}

static int aehd_set_user_memory_region(AEHDMemoryListener *gml, AEHDSlot *slot)
{
    AEHDState *s = aehd_state;
    struct aehd_userspace_memory_region mem;
    int r;

    mem.slot = slot->slot | (gml->as_id << 16);
    mem.guest_phys_addr = slot->start_addr;
    mem.userspace_addr = (uint64_t)slot->ram;
    mem.flags = slot->flags;

    if (slot->memory_size && mem.flags & AEHD_MEM_READONLY) {
        /*
         * Set the slot size to 0 before setting the slot to the desired
         * value. This is needed based on KVM commit 75d61fbc.
         */
        mem.memory_size = 0;
        r = aehd_vm_ioctl(s, AEHD_SET_USER_MEMORY_REGION,
                          &mem, sizeof(mem), NULL, 0);
    }
    mem.memory_size = slot->memory_size;
    r = aehd_vm_ioctl(s, AEHD_SET_USER_MEMORY_REGION,
                      &mem, sizeof(mem), NULL, 0);
    return r;
}

void aehd_destroy_vcpu(CPUState *cpu)
{
    struct AEHDParkedVcpu *vcpu = NULL;
    int ret = 0;

    DPRINTF("aehd_destroy_vcpu\n");

    ret = aehd_vcpu_ioctl(cpu, AEHD_VCPU_MUNMAP, NULL, 0, NULL, 0);
    fprintf(stderr, "aehd munmap %d\n", ret);

    vcpu = g_malloc0(sizeof(*vcpu));
    vcpu->vcpu_id = aehd_arch_vcpu_id(cpu);
    vcpu->aehd_fd = cpu->aehd_fd;
    QLIST_INSERT_HEAD(&aehd_state->aehd_parked_vcpus, vcpu, node);
}

static HANDLE aehd_get_vcpu(AEHDState *s, unsigned long vcpu_id)
{
    struct AEHDParkedVcpu *cpu;
    HANDLE vcpu_fd = INVALID_HANDLE_VALUE;
    int ret;

    QLIST_FOREACH(cpu, &s->aehd_parked_vcpus, node) {
        if (cpu->vcpu_id == vcpu_id) {
            HANDLE aehd_fd;

            QLIST_REMOVE(cpu, node);
            aehd_fd = cpu->aehd_fd;
            g_free(cpu);
            return aehd_fd;
        }
    }

    ret = aehd_vm_ioctl(s, AEHD_CREATE_VCPU, &vcpu_id, sizeof(vcpu_id),
                        &vcpu_fd, sizeof(vcpu_fd));
    if (ret) {
        return INVALID_HANDLE_VALUE;
    }

    return vcpu_fd;
}

int aehd_init_vcpu(CPUState *cpu)
{
    AEHDState *s = aehd_state;
    long mmap_size;
    int ret;
    HANDLE vcpu_fd;

    DPRINTF("aehd_init_vcpu\n");

    vcpu_fd = aehd_get_vcpu(s, aehd_arch_vcpu_id(cpu));
    if (vcpu_fd == INVALID_HANDLE_VALUE) {
        DPRINTF("aehd_create_vcpu failed\n");
        ret = -EFAULT;
        goto err;
    }

    cpu->aehd_fd = vcpu_fd;
    cpu->aehd_state = s;
    cpu->vcpu_dirty = true;

    ret = aehd_ioctl(s, AEHD_GET_VCPU_MMAP_SIZE, NULL, 0,
                     &mmap_size, sizeof(mmap_size));
    if (ret) {
        DPRINTF("AEHD_GET_VCPU_MMAP_SIZE failed\n");
        goto err;
    }

    ret = aehd_vcpu_ioctl(cpu, AEHD_VCPU_MMAP, NULL, 0,
                          &cpu->aehd_run, sizeof(cpu->aehd_run));
    if (ret) {
        DPRINTF("mmap'ing vcpu state failed\n");
        goto err;
    }

    ret = aehd_arch_init_vcpu(cpu);
err:
    return ret;
}

/*
 * dirty pages logging control
 */

static int aehd_mem_flags(MemoryRegion *mr)
{
    bool readonly = mr->readonly || memory_region_is_romd(mr);
    int flags = 0;

    if (memory_region_get_dirty_log_mask(mr) != 0) {
        flags |= AEHD_MEM_LOG_DIRTY_PAGES;
    }
    if (readonly) {
        flags |= AEHD_MEM_READONLY;
    }
    return flags;
}

static int aehd_slot_update_flags(AEHDMemoryListener *gml, AEHDSlot *mem,
                                 MemoryRegion *mr)
{
    int old_flags;

    old_flags = mem->flags;
    mem->flags = aehd_mem_flags(mr);

    /* If nothing changed effectively, no need to issue ioctl */
    if (mem->flags == old_flags) {
        return 0;
    }

    return aehd_set_user_memory_region(gml, mem);
}

static int aehd_section_update_flags(AEHDMemoryListener *gml,
                                    MemoryRegionSection *section)
{
    hwaddr start_addr, size;
    AEHDSlot *mem;

    size = aehd_align_section(section, &start_addr);
    if (!size) {
        return 0;
    }

    mem = aehd_lookup_matching_slot(gml, start_addr, size);
    if (!mem) {
        /* We don't have a slot if we want to trap every access. */
        return 0;
    }

    return aehd_slot_update_flags(gml, mem, section->mr);
}

static void aehd_log_start(MemoryListener *listener,
                          MemoryRegionSection *section,
                          int old, int new)
{
    AEHDMemoryListener *gml = container_of(listener, AEHDMemoryListener,
                                           listener);
    int r;

    if (old != 0) {
        return;
    }

    r = aehd_section_update_flags(gml, section);
    if (r < 0) {
        fprintf(stderr, "%s: dirty pages log change\n", __func__);
        abort();
    }
}

static void aehd_log_stop(MemoryListener *listener,
                          MemoryRegionSection *section,
                          int old, int new)
{
    AEHDMemoryListener *gml = container_of(listener, AEHDMemoryListener,
                                           listener);
    int r;

    if (new != 0) {
        return;
    }

    r = aehd_section_update_flags(gml, section);
    if (r < 0) {
        fprintf(stderr, "%s: dirty pages log change\n", __func__);
        abort();
    }
}

/* get aehd's dirty pages bitmap and update qemu's */
static int aehd_get_dirty_pages_log_range(MemoryRegionSection *section,
                                          unsigned long *bitmap)
{
    ram_addr_t start = section->offset_within_region +
                       memory_region_get_ram_addr(section->mr);
    ram_addr_t pages = int128_get64(section->size) /
                       qemu_real_host_page_size();

    cpu_physical_memory_set_dirty_lebitmap(bitmap, start, pages);
    return 0;
}

#define ALIGN(x, y)  (((x) + (y) - 1) & ~((y) - 1))

/**
 * aehd_physical_sync_dirty_bitmap - Grab dirty bitmap from kernel space
 * This function updates qemu's dirty bitmap using
 * memory_region_set_dirty().  This means all bits are set
 * to dirty.
 *
 * @start_add: start of logged region.
 * @end_addr: end of logged region.
 */
static int aehd_physical_sync_dirty_bitmap(AEHDMemoryListener *gml,
                                           MemoryRegionSection *section)
{
    AEHDState *s = aehd_state;
    struct aehd_dirty_log d = {};
    AEHDSlot *mem;
    hwaddr start_addr, size;

    size = aehd_align_section(section, &start_addr);
    if (size) {
        mem = aehd_lookup_matching_slot(gml, start_addr, size);
        if (!mem) {
            /* We don't have a slot if we want to trap every access. */
            return 0;
        }

        size = ALIGN(((mem->memory_size) >> TARGET_PAGE_BITS),
                     HOST_LONG_BITS) / 8;
        d.dirty_bitmap = g_malloc0(size);

        d.slot = mem->slot | (gml->as_id << 16);
        if (aehd_vm_ioctl(s, AEHD_GET_DIRTY_LOG, &d, sizeof(d),
                          &d, sizeof(d))) {
            DPRINTF("ioctl failed %d\n", errno);
            g_free(d.dirty_bitmap);
            return -1;
        }

        aehd_get_dirty_pages_log_range(section, d.dirty_bitmap);
        g_free(d.dirty_bitmap);
    }

    return 0;
}

int aehd_check_extension(AEHDState *s, unsigned int extension)
{
    int ret;
    int result;
    HANDLE hDevice = s->fd;

    if (hDevice == INVALID_HANDLE_VALUE) {
        DPRINTF("Invalid HANDLE for aehd device!\n");
        return 0;
    }

    ret = aehd_ioctl(s, AEHD_CHECK_EXTENSION, &extension, sizeof(extension),
                     &result, sizeof(result));

    if (ret) {
        DPRINTF("Failed to get aehd capabilities: %lx\n", GetLastError());
        return 0;
    }

    return result;
}

int aehd_vm_check_extension(AEHDState *s, unsigned int extension)
{
    int ret;
    int result;

    ret = aehd_vm_ioctl(s, AEHD_CHECK_EXTENSION, &extension, sizeof(extension),
                        &result, sizeof(result));
    if (ret < 0) {
        /* VM wide version not implemented, use global one instead */
        ret = aehd_check_extension(s, extension);
    }

    return result;
}

static void aehd_set_phys_mem(AEHDMemoryListener *gml,
                             MemoryRegionSection *section, bool add)
{
    AEHDSlot *mem;
    int err;
    MemoryRegion *mr = section->mr;
    bool writeable = !mr->readonly && !mr->rom_device;
    hwaddr start_addr, size;
    void *ram;

    if (!memory_region_is_ram(mr)) {
        if (writeable) {
            return;
        } else if (!mr->romd_mode) {
            /*
             * If the memory device is not in romd_mode, then we actually want
             * to remove the aehd memory slot so all accesses will trap.
             */
            add = false;
        }
    }

    size = aehd_align_section(section, &start_addr);
    if (!size) {
        return;
    }

    /* use aligned delta to align the ram address */
    ram = memory_region_get_ram_ptr(mr) + section->offset_within_region +
          (start_addr - section->offset_within_address_space);

    if (!add) {
        mem = aehd_lookup_matching_slot(gml, start_addr, size);
        if (!mem) {
            return;
        }
        if (mem->flags & AEHD_MEM_LOG_DIRTY_PAGES) {
            aehd_physical_sync_dirty_bitmap(gml, section);
        }

        /* unregister the slot */
        mem->memory_size = 0;
        err = aehd_set_user_memory_region(gml, mem);
        if (err) {
            fprintf(stderr, "%s: error unregistering overlapping slot: %s\n",
                    __func__, strerror(-err));
            abort();
        }
        return;
    }

    /* register the new slot */
    mem = aehd_alloc_slot(gml);
    mem->memory_size = size;
    mem->start_addr = start_addr;
    mem->ram = ram;
    mem->flags = aehd_mem_flags(mr);

    err = aehd_set_user_memory_region(gml, mem);
    if (err) {
        fprintf(stderr, "%s: error registering slot: %s\n", __func__,
                strerror(-err));
        abort();
    }
}

static void aehd_region_add(MemoryListener *listener,
                           MemoryRegionSection *section)
{
    AEHDMemoryListener *gml = container_of(listener, AEHDMemoryListener,
                                           listener);

    memory_region_ref(section->mr);
    aehd_set_phys_mem(gml, section, true);
}

static void aehd_region_del(MemoryListener *listener,
                           MemoryRegionSection *section)
{
    AEHDMemoryListener *gml = container_of(listener, AEHDMemoryListener,
                                           listener);

    aehd_set_phys_mem(gml, section, false);
    memory_region_unref(section->mr);
}

static void aehd_log_sync(MemoryListener *listener,
                         MemoryRegionSection *section)
{
    AEHDMemoryListener *gml = container_of(listener, AEHDMemoryListener,
                                           listener);
    int r;

    r = aehd_physical_sync_dirty_bitmap(gml, section);
    if (r < 0) {
        fprintf(stderr, "%s: sync dirty bitmap\n", __func__);
        abort();
    }
}

void aehd_memory_listener_register(AEHDState *s, AEHDMemoryListener *gml,
                                  AddressSpace *as, int as_id)
{
    int i;

    gml->slots = g_malloc0(s->nr_slots * sizeof(AEHDSlot));
    gml->as_id = as_id;

    for (i = 0; i < s->nr_slots; i++) {
        gml->slots[i].slot = i;
    }

    gml->listener.region_add = aehd_region_add;
    gml->listener.region_del = aehd_region_del;
    gml->listener.log_start = aehd_log_start;
    gml->listener.log_stop = aehd_log_stop;
    gml->listener.log_sync = aehd_log_sync;
    gml->listener.priority = 10;

    memory_listener_register(&gml->listener, as);
}

int aehd_set_irq(AEHDState *s, int irq, int level)
{
    struct aehd_irq_level event;
    int ret;

    event.level = level;
    event.irq = irq;
    ret = aehd_vm_ioctl(s, AEHD_IRQ_LINE_STATUS, &event, sizeof(event),
                        &event, sizeof(event));

    if (ret < 0) {
        perror("aehd_set_irq");
        abort();
    }

    return event.status;
}

typedef struct AEHDMSIRoute {
    struct aehd_irq_routing_entry kroute;
    QTAILQ_ENTRY(AEHDMSIRoute) entry;
} AEHDMSIRoute;

static void set_gsi(AEHDState *s, unsigned int gsi)
{
    set_bit(gsi, s->used_gsi_bitmap);
}

static void clear_gsi(AEHDState *s, unsigned int gsi)
{
    clear_bit(gsi, s->used_gsi_bitmap);
}

void aehd_init_irq_routing(AEHDState *s)
{
    int gsi_count, i;

    gsi_count = aehd_check_extension(s, AEHD_CAP_IRQ_ROUTING) - 1;
    if (gsi_count > 0) {
        /* Round up so we can search ints using ffs */
        s->used_gsi_bitmap = bitmap_new(gsi_count);
        s->gsi_count = gsi_count;
    }

    s->irq_routes = g_malloc0(sizeof(*s->irq_routes));
    s->nr_allocated_irq_routes = 0;

    for (i = 0; i < AEHD_MSI_HASHTAB_SIZE; i++) {
        QTAILQ_INIT(&s->msi_hashtab[i]);
    }
}

void aehd_irqchip_commit_routes(AEHDState *s)
{
    int ret;
    size_t irq_routing_size;

    s->irq_routes->flags = 0;
    irq_routing_size = sizeof(struct aehd_irq_routing) +
                       s->irq_routes->nr *
                       sizeof(struct aehd_irq_routing_entry);
    ret = aehd_vm_ioctl(s, AEHD_SET_GSI_ROUTING, s->irq_routes,
                        irq_routing_size, NULL, 0);
    assert(ret == 0);
}

static void aehd_add_routing_entry(AEHDState *s,
                                   struct aehd_irq_routing_entry *entry)
{
    struct aehd_irq_routing_entry *new;
    int n, size;

    if (s->irq_routes->nr == s->nr_allocated_irq_routes) {
        n = s->nr_allocated_irq_routes * 2;
        if (n < 64) {
            n = 64;
        }
        size = sizeof(struct aehd_irq_routing);
        size += n * sizeof(*new);
        s->irq_routes = g_realloc(s->irq_routes, size);
        s->nr_allocated_irq_routes = n;
    }
    n = s->irq_routes->nr++;
    new = &s->irq_routes->entries[n];

    *new = *entry;

    set_gsi(s, entry->gsi);
}

static int aehd_update_routing_entry(AEHDState *s,
                                    struct aehd_irq_routing_entry *new_entry)
{
    struct aehd_irq_routing_entry *entry;
    int n;

    for (n = 0; n < s->irq_routes->nr; n++) {
        entry = &s->irq_routes->entries[n];
        if (entry->gsi != new_entry->gsi) {
            continue;
        }

        if (!memcmp(entry, new_entry, sizeof *entry)) {
            return 0;
        }

        *entry = *new_entry;

        return 0;
    }

    return -ESRCH;
}

void aehd_irqchip_add_irq_route(AEHDState *s, int irq, int irqchip, int pin)
{
    struct aehd_irq_routing_entry e = {};

    assert(pin < s->gsi_count);

    e.gsi = irq;
    e.type = AEHD_IRQ_ROUTING_IRQCHIP;
    e.flags = 0;
    e.u.irqchip.irqchip = irqchip;
    e.u.irqchip.pin = pin;
    aehd_add_routing_entry(s, &e);
}

void aehd_irqchip_release_virq(AEHDState *s, int virq)
{
    struct aehd_irq_routing_entry *e;
    int i;

    for (i = 0; i < s->irq_routes->nr; i++) {
        e = &s->irq_routes->entries[i];
        if (e->gsi == virq) {
            s->irq_routes->nr--;
            *e = s->irq_routes->entries[s->irq_routes->nr];
        }
    }
    clear_gsi(s, virq);
    aehd_arch_release_virq_post(virq);
}

static unsigned int aehd_hash_msi(uint32_t data)
{
    /*
     * According to Intel SDM, the lowest byte is an interrupt vector
     */
    return data & 0xff;
}

static void aehd_flush_dynamic_msi_routes(AEHDState *s)
{
    AEHDMSIRoute *route, *next;
    unsigned int hash;

    for (hash = 0; hash < AEHD_MSI_HASHTAB_SIZE; hash++) {
        QTAILQ_FOREACH_SAFE(route, &s->msi_hashtab[hash], entry, next) {
            aehd_irqchip_release_virq(s, route->kroute.gsi);
            QTAILQ_REMOVE(&s->msi_hashtab[hash], route, entry);
            g_free(route);
        }
    }
}

static int aehd_irqchip_get_virq(AEHDState *s)
{
    int next_virq;

    /*
     * PIC and IOAPIC share the first 16 GSI numbers, thus the available
     * GSI numbers are more than the number of IRQ route. Allocating a GSI
     * number can succeed even though a new route entry cannot be added.
     * When this happens, flush dynamic MSI entries to free IRQ route entries.
     */
    if (s->irq_routes->nr == s->gsi_count) {
        aehd_flush_dynamic_msi_routes(s);
    }

    /* Return the lowest unused GSI in the bitmap */
    next_virq = find_first_zero_bit(s->used_gsi_bitmap, s->gsi_count);
    if (next_virq >= s->gsi_count) {
        return -ENOSPC;
    } else {
        return next_virq;
    }
}

static AEHDMSIRoute *aehd_lookup_msi_route(AEHDState *s, MSIMessage msg)
{
    unsigned int hash = aehd_hash_msi(msg.data);
    AEHDMSIRoute *route;

    QTAILQ_FOREACH(route, &s->msi_hashtab[hash], entry) {
        if (route->kroute.u.msi.address_lo == (uint32_t)msg.address &&
            route->kroute.u.msi.address_hi == (msg.address >> 32) &&
            route->kroute.u.msi.data == le32_to_cpu(msg.data)) {
            return route;
        }
    }
    return NULL;
}

int aehd_irqchip_send_msi(AEHDState *s, MSIMessage msg)
{
    AEHDMSIRoute *route;

    route = aehd_lookup_msi_route(s, msg);
    if (!route) {
        int virq;

        virq = aehd_irqchip_get_virq(s);
        if (virq < 0) {
            return virq;
        }

        route = g_malloc0(sizeof(AEHDMSIRoute));
        route->kroute.gsi = virq;
        route->kroute.type = AEHD_IRQ_ROUTING_MSI;
        route->kroute.flags = 0;
        route->kroute.u.msi.address_lo = (uint32_t)msg.address;
        route->kroute.u.msi.address_hi = msg.address >> 32;
        route->kroute.u.msi.data = le32_to_cpu(msg.data);

        aehd_add_routing_entry(s, &route->kroute);
        aehd_irqchip_commit_routes(s);

        QTAILQ_INSERT_TAIL(&s->msi_hashtab[aehd_hash_msi(msg.data)], route,
                           entry);
    }

    assert(route->kroute.type == AEHD_IRQ_ROUTING_MSI);

    return aehd_set_irq(s, route->kroute.gsi, 1);
}

int aehd_irqchip_add_msi_route(AEHDState *s, int vector, PCIDevice *dev)
{
    struct aehd_irq_routing_entry kroute = {};
    int virq;
    MSIMessage msg = {0, 0};

    if (dev) {
        msg = pci_get_msi_message(dev, vector);
    }

    virq = aehd_irqchip_get_virq(s);
    if (virq < 0) {
        return virq;
    }

    kroute.gsi = virq;
    kroute.type = AEHD_IRQ_ROUTING_MSI;
    kroute.flags = 0;
    kroute.u.msi.address_lo = (uint32_t)msg.address;
    kroute.u.msi.address_hi = msg.address >> 32;
    kroute.u.msi.data = le32_to_cpu(msg.data);

    aehd_add_routing_entry(s, &kroute);
    aehd_arch_add_msi_route_post(&kroute, vector, dev);
    aehd_irqchip_commit_routes(s);

    return virq;
}

int aehd_irqchip_update_msi_route(AEHDState *s, int virq, MSIMessage msg,
                                 PCIDevice *dev)
{
    struct aehd_irq_routing_entry kroute = {};

    kroute.gsi = virq;
    kroute.type = AEHD_IRQ_ROUTING_MSI;
    kroute.flags = 0;
    kroute.u.msi.address_lo = (uint32_t)msg.address;
    kroute.u.msi.address_hi = msg.address >> 32;
    kroute.u.msi.data = le32_to_cpu(msg.data);

    return aehd_update_routing_entry(s, &kroute);
}

void aehd_irqchip_set_qemuirq_gsi(AEHDState *s, qemu_irq irq, int gsi)
{
    g_hash_table_insert(s->gsimap, irq, GINT_TO_POINTER(gsi));
}

static void aehd_irqchip_create(MachineState *machine, AEHDState *s)
{
    int ret;

    /*
     * First probe and see if there's a arch-specific hook to create the
     * in-kernel irqchip for us
     */
    ret = aehd_arch_irqchip_create(machine, s);
    if (ret == 0) {
        ret = aehd_vm_ioctl(s, AEHD_CREATE_IRQCHIP, NULL, 0, NULL, 0);
    }
    if (ret < 0) {
        fprintf(stderr, "Create kernel irqchip failed: %s\n", strerror(-ret));
        exit(1);
    }

    aehd_init_irq_routing(s);

    s->gsimap = g_hash_table_new(g_direct_hash, g_direct_equal);
}

/*
 * Find number of supported CPUs using the recommended
 * procedure from the kernel API documentation to cope with
 * older kernels that may be missing capabilities.
 */
static int aehd_recommended_vcpus(AEHDState *s)
{
    int ret = aehd_check_extension(s, AEHD_CAP_NR_VCPUS);
    return (ret) ? ret : 4;
}

static int aehd_max_vcpus(AEHDState *s)
{
    int ret = aehd_check_extension(s, AEHD_CAP_MAX_VCPUS);
    return (ret) ? ret : aehd_recommended_vcpus(s);
}

static int aehd_max_vcpu_id(AEHDState *s)
{
    int ret = aehd_check_extension(s, AEHD_CAP_MAX_VCPU_ID);
    return (ret) ? ret : aehd_max_vcpus(s);
}

bool aehd_vcpu_id_is_valid(int vcpu_id)
{
    AEHDState *s = AEHD_STATE(current_machine->accelerator);
    return vcpu_id >= 0 && vcpu_id < aehd_max_vcpu_id(s);
}

static HANDLE aehd_open_device(void)
{
    HANDLE hDevice;

    hDevice = CreateFile("\\\\.\\aehd", GENERIC_READ | GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hDevice == INVALID_HANDLE_VALUE)
        fprintf(stderr, "Failed to open the aehd device! Error code %lx\n",
                GetLastError());
    return hDevice;
}

static int aehd_init(MachineState *ms)
{
    struct {
        const char *name;
        int num;
    } num_cpus[] = {
        { "SMP",          ms->smp.cpus },
        { "hotpluggable", ms->smp.max_cpus },
        { NULL, }
    }, *nc = num_cpus;
    int soft_vcpus_limit, hard_vcpus_limit;
    AEHDState *s;
    int ret;
    int type = 0;
    HANDLE vmfd;

    s = AEHD_STATE(ms->accelerator);

    /*
     * On systems where the kernel can support different base page
     * sizes, host page size may be different from TARGET_PAGE_SIZE,
     * even with AEHD.  TARGET_PAGE_SIZE is assumed to be the minimum
     * page size for the system though.
     */
    assert(TARGET_PAGE_SIZE <= qemu_real_host_page_size());

    QLIST_INIT(&s->aehd_parked_vcpus);
    s->vmfd = INVALID_HANDLE_VALUE;
    s->fd = aehd_open_device();
    if (s->fd == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Could not access AEHD kernel module: %m\n");
        ret = -ENODEV;
        goto err;
    }

    s->nr_slots = aehd_check_extension(s, AEHD_CAP_NR_MEMSLOTS);

    /* If unspecified, use the default value */
    if (!s->nr_slots) {
        s->nr_slots = 32;
    }

    /* check the vcpu limits */
    soft_vcpus_limit = aehd_recommended_vcpus(s);
    hard_vcpus_limit = aehd_max_vcpus(s);

    while (nc->name) {
        if (nc->num > soft_vcpus_limit) {
            fprintf(stderr,
                    "Warning: Number of %s cpus requested (%d) exceeds "
                    "the recommended cpus supported by AEHD (%d)\n",
                    nc->name, nc->num, soft_vcpus_limit);

            if (nc->num > hard_vcpus_limit) {
                fprintf(stderr, "Number of %s cpus requested (%d) exceeds "
                        "the maximum cpus supported by AEHD (%d)\n",
                        nc->name, nc->num, hard_vcpus_limit);
                exit(1);
            }
        }
        nc++;
    }

    do {
        ret = aehd_ioctl(s, AEHD_CREATE_VM, &type, sizeof(type),
                         &vmfd, sizeof(vmfd));
    } while (ret == -EINTR);

    if (ret < 0) {
        fprintf(stderr, "ioctl(AEHD_CREATE_VM) failed: %d %s\n", -ret,
                strerror(-ret));
        goto err;
    }

    s->vmfd = vmfd;

    ret = aehd_arch_init(ms, s);
    if (ret < 0) {
        goto err;
    }

    aehd_irqchip_create(ms, s);

    aehd_state = s;

    aehd_memory_listener_register(s, &s->memory_listener,
                                 &address_space_memory, 0);

    printf("AEHD is operational\n");

    return 0;

err:
    assert(ret < 0);
    if (s->vmfd != INVALID_HANDLE_VALUE) {
        CloseHandle(s->vmfd);
    }
    if (s->fd != INVALID_HANDLE_VALUE) {
        CloseHandle(s->fd);
    }
    g_free(s->memory_listener.slots);

    return ret;
}

static void aehd_handle_io(uint16_t port, MemTxAttrs attrs, void *data,
                          int direction, int size, uint32_t count)
{
    int i;
    uint8_t *ptr = data;

    for (i = 0; i < count; i++) {
        address_space_rw(&address_space_io, port, attrs,
                         ptr, size,
                         direction == AEHD_EXIT_IO_OUT);
        ptr += size;
    }
}

static int aehd_handle_internal_error(CPUState *cpu, struct aehd_run *run)
{
    fprintf(stderr, "AEHD internal error. Suberror: %d\n",
            run->internal.suberror);

    int i;

    for (i = 0; i < run->internal.ndata; ++i) {
        fprintf(stderr, "extra data[%d]: %"PRIx64"\n",
                i, (uint64_t)run->internal.data[i]);
    }

    if (run->internal.suberror == AEHD_INTERNAL_ERROR_EMULATION) {
        fprintf(stderr, "emulation failure\n");
        if (!aehd_arch_stop_on_emulation_error(cpu)) {
            cpu_dump_state(cpu, stderr, CPU_DUMP_CODE);
            return EXCP_INTERRUPT;
        }
    }
    /*
     * FIXME: Should trigger a qmp message to let management know
     * something went wrong.
     */
    return -1;
}

void aehd_raise_event(CPUState *cpu)
{
    AEHDState *s = aehd_state;
    struct aehd_run *run = cpu->aehd_run;
    unsigned long vcpu_id = aehd_arch_vcpu_id(cpu);

    if (!run) {
        return;
    }
    run->user_event_pending = 1;
    aehd_vm_ioctl(s, AEHD_KICK_VCPU, &vcpu_id, sizeof(vcpu_id), NULL, 0);
}

static void do_aehd_cpu_synchronize_state(CPUState *cpu, run_on_cpu_data arg)
{
    if (!cpu->vcpu_dirty) {
        aehd_arch_get_registers(cpu);
        cpu->vcpu_dirty = true;
    }
}

void aehd_cpu_synchronize_state(CPUState *cpu)
{
    if (!cpu->vcpu_dirty) {
        run_on_cpu(cpu, do_aehd_cpu_synchronize_state, RUN_ON_CPU_NULL);
    }
}

static void do_aehd_cpu_synchronize_post_reset(CPUState *cpu,
                                               run_on_cpu_data arg)
{
    aehd_arch_put_registers(cpu, AEHD_PUT_RESET_STATE);
    cpu->vcpu_dirty = false;
}

void aehd_cpu_synchronize_post_reset(CPUState *cpu)
{
    run_on_cpu(cpu, do_aehd_cpu_synchronize_post_reset, RUN_ON_CPU_NULL);
}

static void do_aehd_cpu_synchronize_post_init(CPUState *cpu,
                                              run_on_cpu_data arg)
{
    aehd_arch_put_registers(cpu, AEHD_PUT_FULL_STATE);
    cpu->vcpu_dirty = false;
}

void aehd_cpu_synchronize_post_init(CPUState *cpu)
{
    run_on_cpu(cpu, do_aehd_cpu_synchronize_post_init, RUN_ON_CPU_NULL);
}

static void do_aehd_cpu_synchronize_pre_loadvm(CPUState *cpu,
                                               run_on_cpu_data arg)
{
    cpu->vcpu_dirty = true;
}

void aehd_cpu_synchronize_pre_loadvm(CPUState *cpu)
{
    run_on_cpu(cpu, do_aehd_cpu_synchronize_pre_loadvm, RUN_ON_CPU_NULL);
}

int aehd_cpu_exec(CPUState *cpu)
{
    struct aehd_run *run = cpu->aehd_run;
    int ret, run_ret;

    DPRINTF("aehd_cpu_exec()\n");

    if (aehd_arch_process_async_events(cpu)) {
        cpu->exit_request = 0;
        return EXCP_HLT;
    }

    qemu_mutex_unlock_iothread();

    do {
        MemTxAttrs attrs;

        if (cpu->vcpu_dirty) {
            aehd_arch_put_registers(cpu, AEHD_PUT_RUNTIME_STATE);
            cpu->vcpu_dirty = false;
        }

        aehd_arch_pre_run(cpu, run);
        if (cpu->exit_request) {
            DPRINTF("interrupt exit requested\n");
            /*
             * AEHD requires us to reenter the kernel after IO exits to complete
             * instruction emulation. This self-signal will ensure that we
             * leave ASAP again.
             */
            qemu_cpu_kick(cpu);
        }

        run_ret = aehd_vcpu_ioctl(cpu, AEHD_RUN, NULL, 0, NULL, 0);

        attrs = aehd_arch_post_run(cpu, run);

        if (run_ret < 0) {
            if (run_ret == -EINTR || run_ret == -EAGAIN) {
                DPRINTF("io window exit\n");
                ret = EXCP_INTERRUPT;
                break;
            }
            fprintf(stderr, "error: aehd run failed %s\n",
                    strerror(-run_ret));
            ret = -1;
            break;
        }

        switch (run->exit_reason) {
        case AEHD_EXIT_IO:
            DPRINTF("handle_io\n");
            /* Called outside BQL */
            aehd_handle_io(run->io.port, attrs,
                          (uint8_t *)run + run->io.data_offset,
                          run->io.direction,
                          run->io.size,
                          run->io.count);
            ret = 0;
            break;
        case AEHD_EXIT_MMIO:
            DPRINTF("handle_mmio\n");
            /* Called outside BQL */
            address_space_rw(&address_space_memory,
                             run->mmio.phys_addr, attrs,
                             run->mmio.data,
                             run->mmio.len,
                             run->mmio.is_write);
            ret = 0;
            break;
        case AEHD_EXIT_IRQ_WINDOW_OPEN:
            DPRINTF("irq_window_open\n");
            ret = EXCP_INTERRUPT;
            break;
        case AEHD_EXIT_INTR:
            DPRINTF("aehd raise event exiting\n");
            ret = EXCP_INTERRUPT;
            break;
        case AEHD_EXIT_SHUTDOWN:
            DPRINTF("shutdown\n");
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            ret = EXCP_INTERRUPT;
            break;
        case AEHD_EXIT_UNKNOWN:
            fprintf(stderr, "AEHD: unknown exit, hardware reason %" PRIx64 "\n",
                    (uint64_t)run->hw.hardware_exit_reason);
            ret = -1;
            break;
        case AEHD_EXIT_INTERNAL_ERROR:
            ret = aehd_handle_internal_error(cpu, run);
            break;
        case AEHD_EXIT_SYSTEM_EVENT:
            switch (run->system_event.type) {
            case AEHD_SYSTEM_EVENT_SHUTDOWN:
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
                ret = EXCP_INTERRUPT;
                break;
            case AEHD_SYSTEM_EVENT_RESET:
                qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
                ret = EXCP_INTERRUPT;
                break;
            case AEHD_SYSTEM_EVENT_CRASH:
                aehd_cpu_synchronize_state(cpu);
                qemu_mutex_lock_iothread();
                qemu_system_guest_panicked(cpu_get_crash_info(cpu));
                qemu_mutex_unlock_iothread();
                ret = 0;
                break;
            default:
                DPRINTF("aehd_arch_handle_exit\n");
                ret = aehd_arch_handle_exit(cpu, run);
                break;
            }
            break;
        default:
            DPRINTF("aehd_arch_handle_exit\n");
            ret = aehd_arch_handle_exit(cpu, run);
            break;
        }
    } while (ret == 0);

    qemu_mutex_lock_iothread();

    if (ret < 0) {
        cpu_dump_state(cpu, stderr, CPU_DUMP_CODE);
        vm_stop(RUN_STATE_INTERNAL_ERROR);
    }

    cpu->exit_request = 0;
    return ret;
}

int aehd_ioctl(AEHDState *s, int type, void *input, size_t input_size,
               void *output, size_t output_size)
{
    int ret;
    DWORD byteRet;

    ret = DeviceIoControl(s->fd, type, input, input_size,
                          output, output_size, &byteRet, NULL);
    if (!ret) {
        DPRINTF("aehd device IO control %x failed: %lx\n",
                type, GetLastError());
        switch (GetLastError()) {
        case ERROR_MORE_DATA:
            ret = -E2BIG;
            break;
        case ERROR_RETRY:
            ret = -EAGAIN;
            break;
        default:
            ret = -EFAULT;
        }
    } else {
        ret = 0;
    }
    return ret;
}

int aehd_vm_ioctl(AEHDState *s, int type, void *input, size_t input_size,
                  void *output, size_t output_size)
{
    int ret;
    DWORD byteRet;

    ret = DeviceIoControl(s->vmfd, type, input, input_size,
                          output, output_size, &byteRet, NULL);
    if (!ret) {
        DPRINTF("aehd VM IO control %x failed: %lx\n",
                type, GetLastError());
        switch (GetLastError()) {
        case ERROR_MORE_DATA:
            ret = -E2BIG;
            break;
        case ERROR_RETRY:
            ret = -EAGAIN;
            break;
        default:
            ret = -EFAULT;
        }
    } else {
        ret = 0;
    }
    return ret;
}

int aehd_vcpu_ioctl(CPUState *cpu, int type, void *input, size_t input_size,
                    void *output, size_t output_size)
{
    int ret;
    DWORD byteRet;

    ret = DeviceIoControl(cpu->aehd_fd, type, input, input_size,
                          output, output_size, &byteRet, NULL);
    if (!ret) {
        DPRINTF("aehd VCPU IO control %x failed: %lx\n",
                type, GetLastError());
        switch (GetLastError()) {
        case ERROR_MORE_DATA:
            ret = -E2BIG;
            break;
        case ERROR_RETRY:
            ret = -EAGAIN;
            break;
        default:
            ret = -EFAULT;
        }
    } else {
        ret = 0;
    }
    return ret;
}

static void aehd_accel_class_init(ObjectClass *oc, void *data)
{
    AccelClass *ac = ACCEL_CLASS(oc);
    ac->name = "AEHD";
    ac->init_machine = aehd_init;
    ac->allowed = &aehd_allowed;
}

static const TypeInfo aehd_accel_type = {
    .name = TYPE_AEHD_ACCEL,
    .parent = TYPE_ACCEL,
    .class_init = aehd_accel_class_init,
    .instance_size = sizeof(AEHDState),
};

static void aehd_type_init(void)
{
    type_register_static(&aehd_accel_type);
}

type_init(aehd_type_init);