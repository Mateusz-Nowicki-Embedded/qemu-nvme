/*
 * QEMU NVMe Controller monitor commands
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-machine.h"
#include "qobject/qdict.h"
#include "qapi/type-helpers.h"
#include "hw/pci/pci.h"
#include "monitor/hmp.h"
#include "monitor/monitor.h"

#include "nvme.h"

typedef struct CollectCtx {
    GString    *buf;
    const char *path;       /* NULL = collect every NVMe controller */
} CollectCtx;

/*
 * Build a canonical QOM path from a user-supplied name.  A bare qdev
 * id like "nvme0" is rewritten as "/machine/peripheral/nvme0"; a value
 * that already starts with '/' is taken verbatim.  Caller owns the
 * returned string.
 */
static char *nvme_canonical_path(const char *name)
{
    if (!name) {
        return NULL;
    }
    if (name[0] == '/') {
        return g_strdup(name);
    }
    return g_strdup_printf("/machine/peripheral/%s", name);
}

static bool obj_matches_path(Object *obj, const char *path)
{
    g_autofree char *canonical = NULL;

    if (!path) {
        return true;
    }
    canonical = object_get_canonical_path(obj);
    return g_strcmp0(canonical, path) == 0;
}

static int collect_one(Object *obj, void *opaque)
{
    CollectCtx *ctx = opaque;
    NvmeCtrl *n;
    PCIDevice *pci;
    pcibus_t bar0;
    unsigned io_sq = 0, io_cq = 0;

    if (!object_dynamic_cast(obj, TYPE_NVME)) {
        return 0;
    }
    if (!obj_matches_path(obj, ctx->path)) {
        return 0;
    }
    n = NVME(obj);
    pci = PCI_DEVICE(n);
    bar0 = pci_get_bar_addr(pci, 0);

    for (unsigned i = 1; i <= n->params.max_ioqpairs; i++) {
        if (n->sq && n->sq[i]) {
            io_sq++;
        }
        if (n->cq && n->cq[i]) {
            io_cq++;
        }
    }

    g_string_append_printf(ctx->buf, "%s\n", object_get_canonical_path(obj));
    g_string_append_printf(ctx->buf,
        "  PCI:    BDF %02x:%02x.%x  VID=%04x DID=%04x  ",
        pci_dev_bus_num(pci), PCI_SLOT(pci->devfn), PCI_FUNC(pci->devfn),
        pci_get_word(pci->config + PCI_VENDOR_ID),
        pci_get_word(pci->config + PCI_DEVICE_ID));
    if (bar0 == PCI_BAR_UNMAPPED) {
        g_string_append(ctx->buf, "BAR0=unmapped\n");
    } else {
        g_string_append_printf(ctx->buf, "BAR0=0x%016" PRIx64 "\n",
                               (uint64_t)bar0);
    }
    g_string_append_printf(ctx->buf,
        "  ID:     SN=%.20s  MN=%.40s  FR=%.8s  CNTLID=0x%04x\n",
        n->id_ctrl.sn, n->id_ctrl.mn, n->id_ctrl.fr, n->cntlid);
    g_string_append_printf(ctx->buf, "  CC:     0x%08x\n",
                           ldl_le_p(&n->bar.cc));
    g_string_append_printf(ctx->buf, "  CSTS:   0x%08x\n",
                           ldl_le_p(&n->bar.csts));
    g_string_append_printf(ctx->buf, "  AQA:    0x%08x\n",
                           ldl_le_p(&n->bar.aqa));
    g_string_append_printf(ctx->buf,
        "  Queues: 1 admin + %u IO SQ / %u IO CQ\n", io_sq, io_cq);
    return 0;
}

HumanReadableText *qmp_x_query_nvme(const char *path, Error **errp)
{
    g_autoptr(GString) buf = g_string_new("");
    CollectCtx ctx = { .buf = buf, .path = path };

    object_child_foreach_recursive(object_get_root(), collect_one, &ctx);

    if (buf->len == 0) {
        if (path) {
            g_string_append_printf(buf, "no NVMe controller at %s\n", path);
        } else {
            g_string_append(buf, "no NVMe controllers\n");
        }
    }

    return human_readable_text_from_str(buf);
}

static void append_sq(GString *buf, NvmeSQueue *sq, unsigned stride)
{
    hwaddr db_off = 0x1000 + 2 * sq->sqid * stride;

    g_string_append_printf(buf,
        "  SQ %u  size=%-5u head=%-5u tail=%-5u cqid=%-3u  "
        "prp1=0x%016" PRIx64 "  SQTDBL=BAR0+0x%03" HWADDR_PRIx "\n",
        sq->sqid, sq->size, sq->head, sq->tail, sq->cqid,
        sq->dma_addr, db_off);
}

static void append_cq(GString *buf, NvmeCQueue *cq, unsigned stride)
{
    hwaddr db_off = 0x1000 + (2 * cq->cqid + 1) * stride;

    g_string_append_printf(buf,
        "  CQ %u  size=%-5u head=%-5u tail=%-5u iv=%-5u  "
        "prp1=0x%016" PRIx64 "  CQHDBL=BAR0+0x%03" HWADDR_PRIx
        "  phaseTag=%u\n",
        cq->cqid, cq->size, cq->head, cq->tail, cq->vector,
        cq->dma_addr, db_off, cq->phase);
}

static int collect_queues(Object *obj, void *opaque)
{
    CollectCtx *ctx = opaque;
    NvmeCtrl *n;
    unsigned stride;

    if (!object_dynamic_cast(obj, TYPE_NVME)) {
        return 0;
    }
    if (!obj_matches_path(obj, ctx->path)) {
        return 0;
    }
    n = NVME(obj);
    stride = 4u << NVME_CAP_DSTRD(ldq_le_p(&n->bar.cap));

    g_string_append_printf(ctx->buf, "%s\n", object_get_canonical_path(obj));
    append_sq(ctx->buf, &n->admin_sq, stride);
    append_cq(ctx->buf, &n->admin_cq, stride);

    for (unsigned i = 1; i <= n->params.max_ioqpairs; i++) {
        if (n->sq && n->sq[i]) {
            append_sq(ctx->buf, n->sq[i], stride);
        }
        if (n->cq && n->cq[i]) {
            append_cq(ctx->buf, n->cq[i], stride);
        }
    }
    return 0;
}

HumanReadableText *qmp_x_query_nvme_queues(const char *path, Error **errp)
{
    g_autoptr(GString) buf = g_string_new("");
    CollectCtx ctx = { .buf = buf, .path = path };

    object_child_foreach_recursive(object_get_root(), collect_queues, &ctx);

    if (buf->len == 0) {
        if (path) {
            g_string_append_printf(buf, "no NVMe controller at %s\n", path);
        } else {
            g_string_append(buf, "no NVMe controllers\n");
        }
    }

    return human_readable_text_from_str(buf);
}

typedef struct SetSqDelayCtx {
    uint32_t   sqid;
    int64_t    delay_ns;
    const char *path;       /* NULL = match all NVMe controllers */
    unsigned   matched;
} SetSqDelayCtx;

static int set_sq_delay_one(Object *obj, void *opaque)
{
    SetSqDelayCtx *ctx = opaque;
    NvmeCtrl *n;
    NvmeSQueue *sq;

    if (!object_dynamic_cast(obj, TYPE_NVME)) {
        return 0;
    }
    if (!obj_matches_path(obj, ctx->path)) {
        return 0;
    }
    n = NVME(obj);

    if (ctx->sqid > n->params.max_ioqpairs) {
        return 0;
    }
    sq = n->sq ? n->sq[ctx->sqid] : NULL;
    if (!sq) {
        return 0;
    }

    sq->delay_ns = ctx->delay_ns;
    ctx->matched++;
    return 0;
}

void hmp_nvme_completion_delay(Monitor *mon, const QDict *qdict)
{
    int64_t sqid = qdict_get_int(qdict, "sqid");
    int64_t delay_ms = qdict_get_int(qdict, "delay_ms");
    const char *name = qdict_get_try_str(qdict, "name");
    g_autofree char *path = nvme_canonical_path(name);
    SetSqDelayCtx ctx;

    if (sqid < 0 || sqid > UINT16_MAX) {
        monitor_printf(mon, "sqid %" PRId64 " out of range\n", sqid);
        return;
    }
    if (delay_ms < 0) {
        monitor_printf(mon, "delay_ms must be >= 0\n");
        return;
    }

    ctx.sqid = sqid;
    ctx.delay_ns = delay_ms * 1000000LL;
    ctx.path = path;
    ctx.matched = 0;

    object_child_foreach_recursive(object_get_root(),
                                   set_sq_delay_one, &ctx);

    if (ctx.matched == 0) {
        if (path) {
            monitor_printf(mon,
                           "no NVMe SQ with sqid=%" PRId64 " at %s\n",
                           sqid, path);
        } else {
            monitor_printf(mon,
                           "no NVMe SQ with sqid=%" PRId64 " found\n",
                           sqid);
        }
        return;
    }

    monitor_printf(mon,
                   "nvme_completion_delay: sqid=%" PRId64
                   " delay_ms=%" PRId64 "\n",
                   sqid, delay_ms);
}

void hmp_info_nvme(Monitor *mon, const QDict *qdict)
{
    const char *name = qdict_get_try_str(qdict, "name");
    g_autofree char *path = nvme_canonical_path(name);
    Error *err = NULL;
    g_autoptr(HumanReadableText) info = NULL;

    info = qmp_x_query_nvme(path, &err);
    if (hmp_handle_error(mon, err)) {
        return;
    }
    monitor_puts(mon, info->human_readable_text);
}

void hmp_info_nvme_queues(Monitor *mon, const QDict *qdict)
{
    const char *name = qdict_get_try_str(qdict, "name");
    g_autofree char *path = nvme_canonical_path(name);
    Error *err = NULL;
    g_autoptr(HumanReadableText) info = NULL;

    info = qmp_x_query_nvme_queues(path, &err);
    if (hmp_handle_error(mon, err)) {
        return;
    }
    monitor_puts(mon, info->human_readable_text);
}
