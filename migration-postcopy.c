/*
 * migration-postcopy.c: postcopy livemigration
 *
 * Copyright (c) 2011, 2012, 2013
 * National Institute of Advanced Industrial Science and Technology
 *
 * https://sites.google.com/site/grivonhome/quick-kvm-migration
 * Author: Isaku Yamahata <isaku.yamahata at gmail com>
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config-host.h"

#if defined(CONFIG_MADVISE) || defined(CONFIG_POSIX_MADVISE)
#include <sys/mman.h>
#endif

#include "exec/memory.h"
#include "exec/memory-internal.h"
#include "exec/cpu-common.h"
#include "sysemu/sysemu.h"

#include "qemu/bitmap.h"
#include "sysemu/kvm.h"
#include "hw/hw.h"
#include "sysemu/arch_init.h"
#include "migration/migration.h"
#include "migration/rdma.h"
#include "migration/postcopy.h"
#include "qemu/sockets.h"
#include "qemu/thread.h"
#include "migration/umem.h"

/* #define DEBUG_POSTCOPY */
#ifdef DEBUG_POSTCOPY
#ifdef CONFIG_LINUX
#include <sys/syscall.h>
#define DPRINTF(fmt, ...)                                               \
    do {                                                                \
        printf("%d:%ld %s:%d: " fmt, getpid(), syscall(SYS_gettid),     \
               __func__, __LINE__, ## __VA_ARGS__);                     \
    } while (0)
#else
#define DPRINTF(fmt, ...)                                               \
    do {                                                                \
        printf("%s:%d: " fmt, __func__, __LINE__, ## __VA_ARGS__);      \
    } while (0)
#endif
#else
#define DPRINTF(fmt, ...)       do { } while (0)
#endif

static void fd_close(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void set_fd(int fd, fd_set *fds, int *nfds)
{
    FD_SET(fd, fds);
    if (fd > *nfds) {
        *nfds = fd;
    }
}

/***************************************************************************
 * umem daemon on destination <-> qemu on source protocol
 */

static void postcopy_incoming_send_req_idstr(QEMUFile *f, const char* idstr)
{
    qemu_put_byte(f, strlen(idstr));
    qemu_put_buffer(f, (uint8_t *)idstr, strlen(idstr));
}

static void postcopy_incoming_send_req_pgoffs(QEMUFile *f, uint32_t nr,
                                              const uint64_t *pgoffs)
{
    uint32_t i;

    qemu_put_be32(f, nr);
    for (i = 0; i < nr; i++) {
        qemu_put_be64(f, pgoffs[i]);
    }
}

static void postcopy_incoming_send_req_one(QEMUFile *f, const QEMUUMemReq *req)
{
    DPRINTF("cmd %d nr %d\n", req->cmd, req->nr);
    qemu_put_byte(f, req->cmd);
    switch (req->cmd) {
    case QEMU_UMEM_REQ_EOC:
        /* nothing */
        break;
    case QEMU_UMEM_REQ_PAGE:
        postcopy_incoming_send_req_idstr(f, req->idstr);
        postcopy_incoming_send_req_pgoffs(f, req->nr, req->pgoffs);
        break;
    case QEMU_UMEM_REQ_PAGE_CONT:
        postcopy_incoming_send_req_pgoffs(f, req->nr, req->pgoffs);
        break;
    default:
        abort();
        break;
    }
}

/* QEMUFile can buffer up to IO_BUF_SIZE = 32 * 1024.
 * So one message size must be <= IO_BUF_SIZE
 * cmd: 1
 * id len: 1
 * id: 256
 * nr: 2
 */
#define MAX_PAGE_NR     ((32 * 1024 - 1 - 1 - 256 - 2) / sizeof(uint64_t))
static void postcopy_file_incoming_send_req(QEMUFile *f,
                                            const QEMUUMemReq *req)
{
    uint32_t nr = req->nr;
    QEMUUMemReq tmp = *req;

    switch (req->cmd) {
    case QEMU_UMEM_REQ_EOC:
        postcopy_incoming_send_req_one(f, &tmp);
        break;
    case QEMU_UMEM_REQ_PAGE:
        tmp.nr = MIN(nr, MAX_PAGE_NR);
        postcopy_incoming_send_req_one(f, &tmp);

        nr -= tmp.nr;
        tmp.pgoffs += tmp.nr;
        tmp.cmd = QEMU_UMEM_REQ_PAGE_CONT;
        /* fall through */
    case QEMU_UMEM_REQ_PAGE_CONT:
        while (nr > 0) {
            tmp.nr = MIN(nr, MAX_PAGE_NR);
            postcopy_incoming_send_req_one(f, &tmp);

            nr -= tmp.nr;
            tmp.pgoffs += tmp.nr;
        }
        break;
    default:
        abort();
        break;
    }
}

/* TODO: use function pointer */
static void postcopy_incoming_send_req(QEMUFile *f,
                                       RDMAPostcopyIncoming *rdma,
                                       const QEMUUMemReq *req,
                                       const UMemBlock *block)
{
    assert(!!f ^ !!rdma);
    if (f) {
        postcopy_file_incoming_send_req(f, req);
    } else if (rdma) {
        postcopy_rdma_incoming_send_req(rdma, req, block);
    }
}

static int postcopy_outgoing_recv_req_idstr(QEMUFile *f,
                                            QEMUUMemReq *req, size_t *offset)
{
    int ret;

    req->len = qemu_peek_byte(f, *offset);
    *offset += 1;
    if (req->len == 0) {
        return -EAGAIN;
    }
    ret = qemu_peek_buffer(f, (uint8_t*)req->idstr, req->len, *offset);
    *offset += ret;
    if (ret != req->len) {
        return -EAGAIN;
    }
    req->idstr[req->len] = 0;
    return 0;
}

static int postcopy_outgoing_recv_req_pgoffs(QEMUFile *f,
                                             QEMUUMemReq *req, size_t *offset)
{
    int ret;
    uint32_t be32;
    uint32_t i;

    ret = qemu_peek_buffer(f, (uint8_t*)&be32, sizeof(be32), *offset);
    *offset += sizeof(be32);
    if (ret != sizeof(be32)) {
        return -EAGAIN;
    }

    req->nr = be32_to_cpu(be32);
    req->pgoffs = g_new(uint64_t, req->nr);
    for (i = 0; i < req->nr; i++) {
        uint64_t be64;
        ret = qemu_peek_buffer(f, (uint8_t*)&be64, sizeof(be64), *offset);
        *offset += sizeof(be64);
        if (ret != sizeof(be64)) {
            g_free(req->pgoffs);
            req->pgoffs = NULL;
            return -EAGAIN;
        }
        req->pgoffs[i] = be64_to_cpu(be64);
    }
    return 0;
}

static int postcopy_outgoing_recv_req(QEMUFile *f, QEMUUMemReq *req)
{
    int size;
    int ret;
    size_t offset = 0;

    size = qemu_peek_buffer(f, (uint8_t*)&req->cmd, 1, offset);
    if (size <= 0) {
        return -EAGAIN;
    }
    offset += 1;

    switch (req->cmd) {
    case QEMU_UMEM_REQ_EOC:
        /* nothing */
        break;
    case QEMU_UMEM_REQ_PAGE:
        ret = postcopy_outgoing_recv_req_idstr(f, req, &offset);
        if (ret < 0) {
            return ret;
        }
        ret = postcopy_outgoing_recv_req_pgoffs(f, req, &offset);
        if (ret < 0) {
            return ret;
        }
        break;
    case QEMU_UMEM_REQ_PAGE_CONT:
        ret = postcopy_outgoing_recv_req_pgoffs(f, req, &offset);
        if (ret < 0) {
            return ret;
        }
        break;
    default:
        abort();
        break;
    }
    qemu_file_skip(f, offset);
    DPRINTF("cmd %d\n", req->cmd);
    return 0;
}

static void postcopy_outgoing_free_req(QEMUUMemReq *req)
{
    g_free(req->pgoffs);
}

/***************************************************************************
 * QEMU_VM_POSTCOPY section subtype
 */
#define QEMU_VM_POSTCOPY_INIT           0
#define QEMU_VM_POSTCOPY_SECTION_FULL   1

/* options in QEMU_VM_POSTCOPY_INIT section */
#define POSTCOPY_OPTION_PRECOPY         1ULL

/***************************************************************************
 * outgoing part
 */

void qmp_migrate_force_postcopy_phase(Error **errp)
{
    MigrationState *ms = migrate_get_current();
    ms->force_postcopy_phase = true;
}

void qmp_migrate_postcopy_set_bg(bool enable, Error **errp)
{
    MigrationState *ms = migrate_get_current();
    ms->enabled_capabilities[MIGRATION_CAPABILITY_POSTCOPY_NO_BACKGROUND] =
        !enable;
}

/* Should not call this when RDMA case. It is handled specifically */
int postcopy_outgoing_create_read_socket(MigrationState *s, int fd)
{
    int flags;
    int fd_read;

    if (!migrate_postcopy_outgoing()) {
        return 0;
    }

    flags = fcntl(fd, F_GETFL);
    if ((flags & O_ACCMODE) != O_RDWR) {
        return -ENOSYS;
    }

    fd_read = dup(fd);
    if (fd_read == -1) {
        int ret = -errno;
        perror("dup");
        return ret;
    }
    s->file_read = qemu_fopen_socket(fd_read, "rb");
    if (s->file_read == NULL) {
        close(fd_read);
        return -EINVAL;
    }
    qemu_file_set_thread(s->file_read, true);
    return 0;
}

void postcopy_outgoing_state_begin(QEMUFile *f, const MigrationParams *params)
{
    uint64_t options = 0;
    if (params->precopy_count > 0) {
        options |= POSTCOPY_OPTION_PRECOPY;
    }
    migrate_get_current()->force_postcopy_phase = false;

    qemu_put_ubyte(f, QEMU_VM_POSTCOPY_INIT);
    qemu_put_be32(f, sizeof(options));
    qemu_put_be64(f, options);
}

void postcopy_outgoing_state_complete(
    QEMUFile *f, const uint8_t *buffer, size_t buffer_size)
{
    qemu_put_ubyte(f, QEMU_VM_POSTCOPY_SECTION_FULL);
    qemu_put_be32(f, buffer_size);
    qemu_put_buffer(f, buffer, buffer_size);
}

int postcopy_outgoing_ram_save_iterate(QEMUFile *f, void *opaque)
{
    int ret;
    MigrationState *ms = migrate_get_current();
    if (ms->params.precopy_count == 0 || ms->force_postcopy_phase) {
        qemu_put_be64(f, RAM_SAVE_FLAG_EOS);
        return 1;
    }

    ret = ram_save_iterate(f);
    if (ret < 0) {
        return ret;
    }
    if (ret == 1) {
        DPRINTF("precopy worked\n");
        return ret;
    }
    if (ram_bytes_remaining() == 0) {
        DPRINTF("no more precopy\n");
        return 1;
    }
    return ms->precopy_count >= ms->params.precopy_count? 1: 0;
}

int postcopy_outgoing_ram_save_complete(QEMUFile *f, void *opaque)
{
    MigrationState *ms = migrate_get_current();
    if (ms->params.precopy_count > 0) {
        /* Make sure all dirty bits are set */
        qemu_mutex_lock_ramlist();
        migration_bitmap_sync();
        ram_control_before_iterate(f, RAM_CONTROL_FINISH);
        ram_control_after_iterate(f, RAM_CONTROL_FINISH);
        memory_global_dirty_log_stop();
        qemu_mutex_unlock_ramlist();
    } else {
        migration_bitmap_init();
    }
    ram_save_page_reset();
    ram_save_bulk_stage_done();
    qemu_put_be64(f, RAM_SAVE_FLAG_EOS);
    return 0;
}

uint64_t postcopy_outgoing_ram_save_pending(QEMUFile *f, void *opaque,
                                            uint64_t max_size)
{
    MigrationState *ms = migrate_get_current();
    if (ms->params.precopy_count > 0 &&
        ms->precopy_count < ms->params.precopy_count &&
        !ms->force_postcopy_phase) {
        return ram_save_pending(f, opaque, max_size);
    }
    return 0;
}

static void postcopy_outgoing_ram_save_page(QEMUFile *f,
                                            PostcopyOutgoingState *s,
                                            uint64_t pgoffset, bool forward,
                                            int prefault_pgoffset)
{
    ram_addr_t offset;

    if (forward) {
        pgoffset += prefault_pgoffset;
    } else {
        if (pgoffset < prefault_pgoffset) {
            return;
        }
        pgoffset -= prefault_pgoffset;
    }

    offset = pgoffset << TARGET_PAGE_BITS;
    if (offset >= s->last_block_read->length) {
        assert(forward);
        assert(prefault_pgoffset > 0);
        return;
    }

    ram_save_page(f, s->last_block_read, offset);
}

/*
 * return value
 *   0: continue postcopy mode
 * > 0: completed postcopy mode.
 * < 0: error
 */
static int postcopy_outgoing_handle_req(MigrationState *ms,
                                        const QEMUUMemReq *req)
{
    PostcopyOutgoingState *s = ms->postcopy;
    QEMUFile *f = ms->file;
    int i;
    uint64_t j;
    RAMBlock *block;

    DPRINTF("cmd %d state %d\n", req->cmd, s->state);
    switch(req->cmd) {
    case QEMU_UMEM_REQ_EOC:
        /* tell to finish migration. */
        if (s->state == PO_STATE_ALL_PAGES_SENT) {
            s->state = PO_STATE_COMPLETED;
            DPRINTF("-> PO_STATE_COMPLETED\n");
        } else {
            s->state = PO_STATE_EOC_RECEIVED;
            DPRINTF("-> PO_STATE_EOC_RECEIVED\n");
        }
        return 1;
    case QEMU_UMEM_REQ_PAGE:
        DPRINTF("idstr: %s\n", req->idstr);
        block = ram_find_block(req->idstr, strlen(req->idstr));
        if (block == NULL) {
            return -EINVAL;
        }
        s->last_block_read = block;
        /* fall through */
    case QEMU_UMEM_REQ_PAGE_CONT:
        DPRINTF("nr %d\n", req->nr);
        if (s->state == PO_STATE_ALL_PAGES_SENT) {
            break;
        }
        for (i = 0; i < req->nr; i++) {
            DPRINTF("pgoffs[%d] 0x%"PRIx64"\n", i, req->pgoffs[i]);
            postcopy_outgoing_ram_save_page(f, s, req->pgoffs[i], true, 0);
        }
        /* forward prefault */
        for (j = 1; j <= ms->params.prefault_forward; j++) {
            for (i = 0; i < req->nr; i++) {
                DPRINTF("pgoffs[%d] + 0x%"PRIx64" 0x%"PRIx64"\n",
                        i, j, req->pgoffs[i] + j);
                postcopy_outgoing_ram_save_page(f, s, req->pgoffs[i],
                                                true, j);
            }
        }
        if (migrate_postcopy_outgoing_move_background()) {
            ram_addr_t last_offset =
                (req->pgoffs[req->nr - 1] + ms->params.prefault_forward) <<
                TARGET_PAGE_BITS;
            last_offset = MIN(last_offset,
                              s->last_block_read->length - TARGET_PAGE_SIZE);
            ram_save_set_last_seen_block(s->last_block_read, last_offset);
        }
        /* backward prefault */
        for (j = 1; j <= ms->params.prefault_backward; j++) {
            for (i = 0; i < req->nr; i++) {
                DPRINTF("pgoffs[%d] - 0x%"PRIx64" 0x%"PRIx64"\n",
                        i, j, req->pgoffs[i] - j);
                postcopy_outgoing_ram_save_page(f, s, req->pgoffs[i],
                                                false, j);
            }
        }
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static void postcopy_outgoing_recv_handler(MigrationState *ms)
{
    PostcopyOutgoingState *s = ms->postcopy;
    QEMUFile *file_write = ms->file;
    QEMUFile *file_read = ms->file_read;
    int readfd = qemu_get_fd(file_read);
    int ret = 0;

    DPRINTF("called\n");
    assert(s->state == PO_STATE_ACTIVE ||
           s->state == PO_STATE_ALL_PAGES_SENT);

    do {
        QEMUUMemReq req = {.pgoffs = NULL};

        qemu_set_nonblock(readfd);
        ret = postcopy_outgoing_recv_req(file_read, &req);
        qemu_set_block(readfd);
        if (ret < 0) {
            if (ret == -EAGAIN) {
                ret = 0;
            }
            break;
        }

        /* Even when s->state == PO_STATE_ALL_PAGES_SENT,
           some request can be received like QEMU_UMEM_REQ_EOC */
        qemu_mutex_lock_ramlist();
        ret = postcopy_outgoing_handle_req(ms, &req);
        qemu_mutex_unlock_ramlist();
        postcopy_outgoing_free_req(&req);
    } while (ret == 0);
    qemu_fflush(file_write);

    if (ret < 0) {
        switch (s->state) {
        case PO_STATE_ACTIVE:
            s->state = PO_STATE_ERROR_RECEIVE;
            DPRINTF("-> PO_STATE_ERROR_RECEIVE\n");
            break;
        case PO_STATE_ALL_PAGES_SENT:
            s->state = PO_STATE_COMPLETED;
            DPRINTF("-> PO_STATE_ALL_PAGES_SENT\n");
            break;
        default:
            abort();
        }
    }
    if (s->state == PO_STATE_COMPLETED) {
        DPRINTF("PO_STATE_COMPLETED\n");
    }
    DPRINTF("done\n");
}

static void postcopy_outgoing_send_clean_bitmap(QEMUFile *f)
{
    const unsigned long *bitmap;
    const RAMBlock *block;

    /* migration bitmap is dirty bitmap.
       needs to convert it from dirty to clean. */
    qemu_mutex_lock_ramlist();
    bitmap = migration_bitmap_get();
    QTAILQ_FOREACH(block, &ram_list.blocks, next) {
        uint64_t length;
        uint64_t start;
        uint64_t end;
        uint64_t end_uint64;
        uint64_t i;
        uint64_t val;
        unsigned long tmp[sizeof(uint64_t) / sizeof(unsigned long)];

        qemu_put_byte(f, strlen(block->idstr));
        qemu_put_buffer(f, (uint8_t *)block->idstr, strlen(block->idstr));
        qemu_put_be64(f, block->offset);
        qemu_put_be64(f, block->length);

        start = (block->offset >> TARGET_PAGE_BITS);
        end = (block->offset + block->length) >> TARGET_PAGE_BITS;

        length = postcopy_bitmap_length(block->length);
        qemu_put_be64(f, length);
        DPRINTF("dirty bitmap %s 0x%"PRIx64" 0x%"PRIx64" 0x%"PRIx64"\n",
                block->idstr, block->offset, block->length, length);

        end_uint64 = start + ((end - start) & ~63);
        /* depends on the implementation of bitmap library */
        if ((start % 64) == 0) {
            for (i = start; i < end_uint64; i += 64) {
                val = postcopy_bitmap_to_uint64(&bitmap[BIT_WORD(i)]);
                val = ~val;     /* dirty bitmap -> clean bitmap */
                qemu_put_be64(f, val);
            }
        } else {
            for (i = start; i < end_uint64; i += 64) {
                int j;
                bitmap_zero(tmp, 64);
                for (j = 0; j < 63; j++) {
                    if (!test_bit(i + j, bitmap)) {
                        set_bit(j, tmp);
                    }
                }
                val = postcopy_bitmap_to_uint64(tmp);
                qemu_put_be64(f, val);
            }
        }
        if (end_uint64 < end) {
            bitmap_zero(tmp, 64);
            for (i = end_uint64; i < end; i++) {
                if (!test_bit(i, bitmap)) {
                    set_bit(i - end_uint64, tmp);
                }
            }
            val = postcopy_bitmap_to_uint64(tmp);
            qemu_put_be64(f, val);
        }
    }
    qemu_mutex_unlock_ramlist();

    /* terminator */
    qemu_put_byte(f, 0);    /* idstr len */
    qemu_put_be64(f, 0);    /* block offset */
    qemu_put_be64(f, 0);    /* block length */
    qemu_put_be64(f, 0);    /* bitmap len */
    DPRINTF("sent dirty bitmap\n");
}

PostcopyOutgoingState *postcopy_outgoing_begin(MigrationState *ms)
{
    PostcopyOutgoingState *s = g_new(PostcopyOutgoingState, 1);
    DPRINTF("outgoing begin\n");
    s->state = PO_STATE_ACTIVE;
    s->last_block_read = NULL;

    if (ms->params.precopy_count > 0 && !qemu_file_is_rdma(ms->file)) {
        postcopy_outgoing_send_clean_bitmap(ms->file);
    }
    qemu_fflush(ms->file);
    qemu_file_reset_rate_limit(ms->file);
    return s;
}

void postcopy_outgoing_cleanup(MigrationState *ms)
{
    migration_bitmap_free();
    if (!migrate_postcopy_outgoing()) {
        return;
    }
    if (ms->rdma_outgoing) {
        postcopy_rdma_outgoing_cleanup(ms->rdma_outgoing);
        ms->rdma_outgoing = NULL;
    }
    if (ms->file_read) {
        qemu_fclose(ms->file_read);
        ms->file_read = NULL;
    }
    g_free(ms->postcopy);
    ms->postcopy = NULL;
}

static void postcopy_outgoing_ram_all_sent(QEMUFile *f,
                                           PostcopyOutgoingState *s)
{
    assert(s->state == PO_STATE_ACTIVE);

    s->state = PO_STATE_ALL_PAGES_SENT;
    /* tell incoming side that all pages are sent */
    qemu_put_be64(f, RAM_SAVE_FLAG_EOS);
    qemu_fflush(f);
    DPRINTF("sent RAM_SAVE_FLAG_EOS\n");
}

static int postcopy_outgoing_ram_save_background(
    MigrationState *ms, MigrationRateLimitStat *rlstat)
{
    PostcopyOutgoingState *s = ms->postcopy;
    QEMUFile *f = ms->file;
    int i;
    int64_t t0;

    DPRINTF("called\n");
    assert(s->state == PO_STATE_ACTIVE ||
           s->state == PO_STATE_EOC_RECEIVED ||
           s->state == PO_STATE_ERROR_RECEIVE);

    switch (s->state) {
    case PO_STATE_ACTIVE:
        /* nothing. processed below */
        break;
    case PO_STATE_EOC_RECEIVED:
        qemu_put_be64(f, RAM_SAVE_FLAG_EOS);
        s->state = PO_STATE_COMPLETED;
        DPRINTF("PO_STATE_COMPLETED\n");
        return 0;
    case PO_STATE_ERROR_RECEIVE:
        DPRINTF("PO_STATE_ERROR_RECEIVE\n");
        return -1;
    default:
        abort();
    }

    if (migrate_postcopy_outgoing_no_background()) {
        if (ram_bytes_remaining() == 0) {
            postcopy_outgoing_ram_all_sent(f, s);
        }
        return 0;
    }

    i = 0;
    t0 = qemu_get_clock_ms(rt_clock);
    migration_update_rate_limit_stat(ms, rlstat, t0);
    qemu_mutex_lock_ramlist();
    while (qemu_file_rate_limit(f) == 0) {
        int nfds = -1;
        int readfd = qemu_get_fd(ms->file_read);
        int writefd = qemu_get_fd(f);
        fd_set readfds;
        fd_set writefds;
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 0};
        int ret;

        if (!ram_save_block(f, true, true)) { /* no more blocks */
            DPRINTF("outgoing background all sent\n");
            assert(s->state == PO_STATE_ACTIVE);
            postcopy_outgoing_ram_all_sent(f, s);
            break;
        }

        migration_update_rate_limit_stat(ms, rlstat,
                                         qemu_get_clock_ms(rt_clock));

        /* If page request is pending, try to process it early. */
        FD_ZERO(&readfds);
        set_fd(readfd, &readfds, &nfds);
        /* We don't want to block on writing so that we can accept
         * page requests as early as possible. */
        FD_ZERO(&writefds);
        set_fd(writefd, &writefds, &nfds);
        ret = select(nfds + 1, &readfds, &writefds, NULL, &timeout);
        if (ret >= 0 && (FD_ISSET(readfd, &readfds) ||
                         !FD_ISSET(writefd, &writefds))) {
            DPRINTF("pending request\n");
            break;
        }

        /* stolen from ram_save_iterate(): not to hold ram lock too long
         * Since this is postcopy phase and VM is already quiescent,
         * bitmap doesn't need to be synced.
         */
        i++;
#define MAX_WAIT 50
        if ((i & 63) == 0) {
            uint64_t t1 = (qemu_get_clock_ms(rt_clock) - t0);
            if (t1 > MAX_WAIT) {
                DPRINTF("big wait: %" PRIu64 " milliseconds, %d iterations\n",
                        t1, i);
                break;
            }
        }
    }
    qemu_mutex_unlock_ramlist();

    DPRINTF("done\n");
    return 0;
}

static int postcopy_outgoing_loop(MigrationState *ms,
                                  MigrationRateLimitStat *rlstat)
{
    /* XXX: threading? */
    PostcopyOutgoingState *s = ms->postcopy;
    int ret = 0;
    int nfds = -1;
    int readfd = qemu_get_fd(ms->file_read);
    int writefd = qemu_get_fd(ms->file);
    fd_set readfds;
    fd_set writefds;
    struct timeval *timeoutp = &(struct timeval) {
        .tv_sec = 0,
        .tv_usec = 0,
    };

    FD_ZERO(&readfds);
    if (s->state == PO_STATE_ACTIVE || s->state == PO_STATE_ALL_PAGES_SENT) {
        set_fd(readfd, &readfds, &nfds);
    }
    FD_ZERO(&writefds);
    if (s->state == PO_STATE_ACTIVE || s->state == PO_STATE_EOC_RECEIVED) {
        int64_t current_time = qemu_get_clock_ms(rt_clock);
        migration_update_rate_limit_stat(ms, rlstat, current_time);
        if (qemu_file_rate_limit(ms->file)) {
            int64_t sleep_ms = migration_sleep_time_ms(rlstat, current_time);
            timeoutp->tv_sec = sleep_ms / 1000;
            timeoutp->tv_usec = (sleep_ms % 1000) * 1000;
        } else {
            set_fd(writefd, &writefds, &nfds);
            timeoutp = NULL;
        }
    } else {
        timeoutp = NULL;
    }
    ret = select(nfds + 1, &readfds, &writefds, NULL, timeoutp);
    if (ret == -1) {
        if (errno == EINTR) {
            return 0;
        }
        return ret;
    }
    if (FD_ISSET(readfd, &readfds)) {
        postcopy_outgoing_recv_handler(ms);
        return 0;
    }
    if (FD_ISSET(writefd, &writefds)) {
        return postcopy_outgoing_ram_save_background(ms, rlstat);
    }
    return 0;
}

static int postcopy_outgoing_file(MigrationState *ms,
                                  MigrationRateLimitStat *rlstat)
{
    int ret = postcopy_outgoing_loop(ms, rlstat);
    if (qemu_file_get_error(ms->file_read)) {
        qemu_file_set_error(ms->file, qemu_file_get_error(ms->file_read));
    }
    if (qemu_file_get_error(ms->file)) {
        return -1;
    }
    return ret;
}

int postcopy_outgoing(MigrationState *ms, MigrationRateLimitStat *rlstat)
{
    int ret = 0;
    PostcopyOutgoingState *s = ms->postcopy;
    int (*loop)(MigrationState *ms, MigrationRateLimitStat *rlstat);

    DPRINTF("postcopy outgoing prefault"
            " forward %"PRId64" backward %"PRId64"\n",
            ms->params.prefault_forward, ms->params.prefault_backward);
    if (qemu_file_is_rdma(ms->file)) {
        ret = postcopy_rdma_outgoing(ms, rlstat);
        if (ret) {
            return ret;
        }
        loop = postcopy_rdma_outgoing_loop;
    } else {
        loop = postcopy_outgoing_file;
    }

    while (s->state != PO_STATE_ERROR_RECEIVE &&
           s->state != PO_STATE_COMPLETED) {
        ret = (*loop)(ms, rlstat);
        if (ret < 0) {
            break;
        }
    }
    return ret;
}

/***************************************************************************
 * incoming part
 */

#define PIS_STATE_QUIT_RECEIVED         0x01
#define PIS_STATE_QUIT_QUEUED           0x02
#define PIS_STATE_QUIT_SENT             0x04

#define PIS_STATE_QUIT_MASK             (PIS_STATE_QUIT_RECEIVED | \
                                         PIS_STATE_QUIT_QUEUED | \
                                         PIS_STATE_QUIT_SENT)

struct PostcopyIncomingState {
    /* dest qemu state */
    uint32_t    state;

    int host_page_size;
    int host_page_shift;

    /* qemu side */
    int to_umemd_fd;
    QEMUFile *to_umemd;

    int from_umemd_fd;
    QEMUFile *from_umemd;
    int version_id;     /* save/load format version id */
};
typedef struct PostcopyIncomingState PostcopyIncomingState;


#define UMEM_STATE_EOS_RECEIVED         0x01    /* umem daemon <-> src qemu */
#define UMEM_STATE_EOC_SEND_REQ         0x02    /* umem daemon <-> src qemu */
#define UMEM_STATE_EOC_SENDING          0x04    /* umem daemon <-> src qemu */
#define UMEM_STATE_EOC_SENT             0x08    /* umem daemon <-> src qemu */

#define UMEM_STATE_QUIT_RECEIVED        0x10    /* umem daemon <-> dst qemu */
#define UMEM_STATE_QUIT_HANDLED         0x20    /* umem daemon <-> dst qemu */
#define UMEM_STATE_QUIT_QUEUED          0x40    /* umem daemon <-> dst qemu */
#define UMEM_STATE_QUIT_SENDING         0x80    /* umem daemon <-> dst qemu */
#define UMEM_STATE_QUIT_SENT            0x100   /* umem daemon <-> dst qemu */

#define UMEM_STATE_ERROR_REQ            0x1000  /* umem daemon error */
#define UMEM_STATE_ERROR_SENDING        0x2000  /* umem daemon error */
#define UMEM_STATE_ERROR_SENT           0x3000  /* umem daemon error */

#define UMEM_STATE_QUIT_MASK            (UMEM_STATE_QUIT_QUEUED |   \
                                         UMEM_STATE_QUIT_SENDING |  \
                                         UMEM_STATE_QUIT_SENT |     \
                                         UMEM_STATE_QUIT_RECEIVED | \
                                         UMEM_STATE_QUIT_HANDLED)
#define UMEM_STATE_END_MASK             (UMEM_STATE_EOS_RECEIVED | \
                                         UMEM_STATE_EOC_SEND_REQ | \
                                         UMEM_STATE_EOC_SENDING |  \
                                         UMEM_STATE_EOC_SENT |     \
                                         UMEM_STATE_QUIT_MASK)

struct PostcopyIncomingUMemDaemon {
    /* umem daemon side */
    QemuMutex mutex;
    uint32_t state;     /* shared state. protected by mutex */

    /* read only */
    int host_page_size;
    int host_page_shift;
    int nr_host_pages_per_target_page;
    int host_to_target_page_shift;
    int nr_target_pages_per_host_page;
    int target_to_host_page_shift;
    int version_id;     /* save/load format version id */
    bool precopy_enabled;

    QemuThread thread;
    UMemBlockHead blocks;

    /* thread to communicate with qemu main loop via pipe */
    QemuThread pipe_thread;
    int to_qemu_fd;
    QEMUFile *to_qemu;
    int from_qemu_fd;
    QEMUFile *from_qemu;

    /* = KVM_MAX_VCPUS * (ASYNC_PF_PER_VCPUS + 1) */
#define MAX_REQUESTS    (512 * (64 + 1))

    /* thread to read from outgoing qemu */
    QemuThread mig_read_thread;
    QEMUFile *mig_read;                 /* qemu on source -> umem daemon */
    UMemBlock *last_block_read;         /* qemu on source -> umem daemon */
    /* bitmap indexed by target page offset */
    UMemPages *page_cached;
    int fault_write_fd;         /* umem daemon -> qemu on destination */
    QemuThread bitmap_thread;

    /* thread to write to outgoing qemu */
    QemuThread mig_write_thread;
    QEMUFile *mig_write;                /* umem daemon -> qemu on source */
    UMemBlock *last_block_write;        /* umem daemon -> qemu on source */
    /* bitmap indexed by target page offset */
    UMemPages *page_request;
    UMemPages *page_clean;
    uint64_t *target_pgoffs;

    /* thread to write to fault pipe write
     * Usually postcopy_incoming_umem_ram_load() writes to fault pipe write
     * by postcopy_incoming_umem_mark_cached(). But it can't be blocked
     * to avoid deadlock. Such pages are marked in
     * UMemBlock::pending_clean_bitmap
     * In that case, this thread handles them.
     */
    QemuThread pending_clean_thread;
    QemuMutex pending_clean_mutex;
    QemuCond pending_clean_cond;
    unsigned long nr_pending_clean;     /* protected by pending_clean_mutex */
    bool pending_clean_exit;

    /* thread to fault pipe read */
    QemuThread fault_thread;
    int fault_read_fd;          /* qemu on destination -> umem daemon */
    ssize_t offset;
    uint64_t buf[PIPE_BUF / sizeof(uint64_t)];

    /* rdma */
    RDMAPostcopyIncoming *rdma;
};
typedef struct PostcopyIncomingUMemDaemon PostcopyIncomingUMemDaemon;

static PostcopyIncomingState state = {
    .state = 0,
    .to_umemd_fd = -1,
    .to_umemd = NULL,
    .from_umemd_fd = -1,
    .from_umemd = NULL,
};

static PostcopyIncomingUMemDaemon umemd = {
    .state = 0,
    .precopy_enabled = false,
    .to_qemu_fd = -1,
    .to_qemu = NULL,
    .from_qemu_fd = -1,
    .from_qemu = NULL,
    .blocks = QLIST_HEAD_INITIALIZER(&umemd.blocks),
    .mig_read = NULL,
    .mig_write = NULL,
    .rdma = NULL,
};

static void postcopy_incoming_umemd(void);
static void postcopy_incoming_qemu_handle_req(void *opaque);
static UMemBlock *postcopy_incoming_umem_block_from_stream(
    QEMUFile *f, int flags);
static void postcopy_incoming_create_fault_thread(int read_fd, int write_fd);

/* protected by qemu_mutex_lock_ramlist() */
void postcopy_incoming_ram_free(RAMBlock *ram_block)
{
    UMemBlock *block;
    QLIST_FOREACH(block, &umemd.blocks, next) {
        if (!strncmp(ram_block->idstr, block->idstr, strlen(block->idstr))) {
            break;
        }
    }
    if (block != NULL) {
        umem_unmap(block->umem);
    } else {
        munmap(ram_block->host, ram_block->length);
    }
}

static int postcopy_incoming_ram_load_get64(QEMUFile *f,
                                            ram_addr_t *addr, uint64_t *flags)
{
    *addr = qemu_get_be64(f);
    *flags = *addr & ~TARGET_PAGE_MASK;
    *addr &= TARGET_PAGE_MASK;
    return qemu_file_get_error(f);
}

static int postcopy_incoming_ram_load(QEMUFile *f, void *opaque, int version_id)
{
    ram_addr_t addr;
    uint64_t flags;
    int error;

    DPRINTF("incoming ram load\n");
    /*
     * RAM_SAVE_FLAGS_EOS or
     * RAM_SAVE_FLAGS_MEM_SIZE + mem size + RAM_SAVE_FLAGS_EOS
     * see postcopy_outgoing_ram_save_live()
     */

    if (version_id != RAM_SAVE_VERSION_ID) {
        DPRINTF("RAM_SAVE_VERSION_ID %d != %d\n",
                version_id, RAM_SAVE_VERSION_ID);
        return -EINVAL;
    }
    while (true) {
        error = postcopy_incoming_ram_load_get64(f, &addr, &flags);
        DPRINTF("addr 0x%lx flags 0x%"PRIx64"\n", addr, flags);
        if (error) {
            DPRINTF("error %d\n", error);
            return error;
        }
        if (flags == RAM_SAVE_FLAG_EOS && addr == 0) {
            DPRINTF("EOS\n");
            return 0;
        }

        if (flags != RAM_SAVE_FLAG_MEM_SIZE) {
            DPRINTF("-EINVAL flags 0x%"PRIx64"\n", flags);
            return -EINVAL;
        }
        error = ram_load_mem_size(f, addr);
        if (error) {
            DPRINTF("addr 0x%lx error %d\n", addr, error);
            return error;
        }

        error = postcopy_incoming_ram_load_get64(f, &addr, &flags);
        if (error) {
            DPRINTF("addr 0x%lx flags 0x%"PRIx64" error %d\n",
                    addr, flags, error);
            return error;
        }
        if (flags == RAM_SAVE_FLAG_EOS && addr == 0) {
            DPRINTF("done\n");
            return 0;
        }
        if (flags == RAM_SAVE_FLAG_HOOK) {
            DPRINTF("RAM_SAVE_FLAG_HOOK\n");
            assert(addr == 0);
            ram_control_load_hook(f, flags);
        }
    }

    DPRINTF("-EINVAL addr 0x"RAM_ADDR_FMT" flags 0x%"PRIx64"\n", addr, flags);
    return -EINVAL;
}

static void*
postcopy_incoming_shmem_from_stream_offset(QEMUFile *f, ram_addr_t offset,
                                           int flags)
{
    UMemBlock *block = postcopy_incoming_umem_block_from_stream(f, flags);
    if (block == NULL) {
        DPRINTF("error block = NULL\n");
        return NULL;
    }
    return block->umem->shmem + offset;
}

static int postcopy_incoming_ram_load_precopy(QEMUFile *f, void *opaque,
                                              int version_id)
{
    return ram_load(f, opaque, version_id,
                    &postcopy_incoming_shmem_from_stream_offset);
}

static void postcopy_incoming_umem_block_free(void)
{
    UMemBlock *block;
    UMemBlock *tmp;

    /* to protect againt postcopy_incoming_ram_free() */
    qemu_mutex_lock_ramlist();
    QLIST_FOREACH_SAFE(block, &umemd.blocks, next, tmp) {
        UMem *umem = block->umem;
        umem_unmap_shmem(umem);
        umem_destroy(umem);
        QLIST_REMOVE(block, next);
        g_free(block->phys_requested);
        g_free(block->phys_received);
        g_free(block->clean_bitmap);
        g_free(block->pending_clean_bitmap);
        g_free(block);
    }
    qemu_mutex_unlock_ramlist();
}

int postcopy_incoming_prepare(UMemBlockHead **umem_blocks)
{
    int error = 0;
    RAMBlock *block;
    int block_index = 0;

    if (!QLIST_EMPTY(&umemd.blocks)) {
        goto out;
    }

    state.state = 0;
    state.host_page_size = getpagesize();
    state.host_page_shift = ffs(state.host_page_size) - 1;
    state.version_id = RAM_SAVE_VERSION_ID; /* = save version of
                                               ram_save_live() */

    qemu_mutex_init(&umemd.mutex);
    umemd.host_page_size = state.host_page_size;
    umemd.host_page_shift = state.host_page_shift;

    umemd.nr_host_pages_per_target_page =
        TARGET_PAGE_SIZE / umemd.host_page_size;
    umemd.nr_target_pages_per_host_page =
        umemd.host_page_size / TARGET_PAGE_SIZE;
    umemd.target_to_host_page_shift =
        ffs(umemd.nr_host_pages_per_target_page) - 1;
    umemd.host_to_target_page_shift =
        ffs(umemd.nr_target_pages_per_host_page) - 1;

    qemu_mutex_lock_ramlist();
    QTAILQ_FOREACH(block, &ram_list.blocks, next) {
        UMem *umem;
        UMemBlock *umem_block;

        if (block->flags & RAM_PREALLOC_MASK) {
            continue;
        }
        error = umem_new(block->host, block->length, &umem);
        if (error < 0) {
            qemu_mutex_unlock_ramlist();
            goto error;
        }
        umem_block = g_malloc0(sizeof(*umem_block));
        umem_block->block_index = block_index;
        block_index++;

        umem_block->umem = umem;
        umem_block->offset = block->offset;
        umem_block->length = block->length;
        pstrcpy(umem_block->idstr, sizeof(umem_block->idstr), block->idstr);

        error = umem_map_shmem(umem_block->umem);
        if (error) {
            qemu_mutex_unlock_ramlist();
            goto error;
        }
        umem_close_shmem(umem_block->umem);

        block->flags |= RAM_POSTCOPY_UMEM_MASK;
        QLIST_INSERT_HEAD(&umemd.blocks, umem_block, next);
    }
    qemu_mutex_unlock_ramlist();

out:
    if (umem_blocks != NULL) {
        *umem_blocks = &umemd.blocks;
    }
    return 0;

error:
    postcopy_incoming_umem_block_free();
    return error;
}

static int postcopy_incoming_loadvm_init(QEMUFile *f, uint32_t size)
{
    uint64_t options;
    int flags;
    int error;

    DPRINTF("postcopy_incoming_loadvm_init\n");
    if (size != sizeof(options)) {
        fprintf(stderr, "unknown size %d\n", size);
        return -EINVAL;
    }
    options = qemu_get_be64(f);
    if (options & POSTCOPY_OPTION_PRECOPY) {
        options &= ~POSTCOPY_OPTION_PRECOPY;
        umemd.precopy_enabled = true;
    } else {
        umemd.precopy_enabled = false;
    }
    if (options) {
        fprintf(stderr, "unknown options 0x%"PRIx64, options);
        return -ENOSYS;
    }
    flags = fcntl(qemu_get_fd(f), F_GETFL);
    if (!qemu_file_is_rdma(f) && (flags & O_ACCMODE) != O_RDWR) {
        /* postcopy requires read/write file descriptor */
        fprintf(stderr, "non-writable connection. "
                "postcopy requires read/write connection \n");
        return -EINVAL;
    }
    if (mem_path) {
        fprintf(stderr, "mem_path is specified to %s. "
                "postcopy doesn't work with it\n", mem_path);
        return -ENOSYS;
    }

    DPRINTF("detected POSTCOPY precpoy %d\n", umemd.precopy_enabled);
    error = postcopy_incoming_prepare(NULL);
    if (error) {
        return error;
    }
    if (umemd.precopy_enabled) {
        savevm_ram_handlers.load_state = postcopy_incoming_ram_load_precopy;
    } else {
        savevm_ram_handlers.load_state = postcopy_incoming_ram_load;
    }
    return 0;
}

static int postcopy_incoming_create_umemd(QEMUFile *mig_read)
{
    int error;
    int fds[2];
    int mig_write_fd;
    int qemu_fault_read_fd;
    int qemu_fault_write_fd;
    pid_t child;
    bool is_rdma = qemu_file_is_rdma(mig_read);
    RDMAPostcopyIncomingInit arg;
    assert(is_rdma ||
           (fcntl(qemu_get_fd(mig_read), F_GETFL) & O_ACCMODE) == O_RDWR);

    if (qemu_pipe(fds) == -1) {
        perror("qemu_pipe");
        return -errno;
    }
    state.from_umemd_fd = fds[0];
    umemd.to_qemu_fd = fds[1];

    if (qemu_pipe(fds) == -1) {
        perror("qemu_pipe");
        return -errno;
    }
    umemd.from_qemu_fd = fds[0];
    state.to_umemd_fd = fds[1];

    if (qemu_pipe(fds) == -1) {
        perror("qemu_pipe");
        return -errno;
    }
    qemu_fault_read_fd = fds[0];
    umemd.fault_write_fd = fds[1];

    if (qemu_pipe(fds) == -1) {
        perror("qemu_pipe");
        return -errno;
    }
    umemd.fault_read_fd = fds[0];
    qemu_fault_write_fd = fds[1];

    if (is_rdma) {
        postcopy_rdma_incoming_prefork(mig_read, &arg);
        qemu_fclose_null(mig_read, NULL, NULL);
        /* ibverb isn't compatible with fork.
         * Child process will again establish connection again.
         * Or swap the role of child and parent. (This way would confuses
         * management program like libvirt)
         */
    }

    child = fork();
    if (child < 0) {
        perror("fork failed");
        return -errno;
    }
    if (child == 0) {
        UMemBlock *block;

        DPRINTF("fork child daemon\n");
        /* needs to fork before rdma setup */
        qemu_daemon(1, 1);

        QLIST_FOREACH(block, &umemd.blocks, next) {
            umem_unmap(block->umem);
        }
        fd_close(&state.to_umemd_fd);
        fd_close(&state.from_umemd_fd);
        fd_close(&qemu_fault_write_fd);
        fd_close(&qemu_fault_read_fd);

        umemd.state = 0;
        umemd.version_id = state.version_id;

        /* postcopy_rdma_incoming_init() accesses those bitmaps
         * for RDMA pre+post as rdma postcopy handles bitmap specifically */
        QLIST_FOREACH(block, &umemd.blocks, next) {
            /* bitmap is sent in the array of uint64_t for pre+post,
             * so round it up to 64 */
            int nbits = ROUND_UP(block->length >> TARGET_PAGE_BITS, 64);
            block->phys_requested = bitmap_new(nbits);
            block->phys_received = bitmap_new(nbits);
            if (umemd.precopy_enabled) {
                block->clean_bitmap = bitmap_new(nbits);
            }
            block->nr_pending_clean = 0;
            block->pending_clean_bitmap =
                bitmap_new(block->length >> umemd.host_page_shift);
        }
        qemu_file_set_thread(mig_read, true);
        if (is_rdma) {
            /* setup rdma connection again */
            arg.umem_blocks = &umemd.blocks;
            arg.precopy_enabled = umemd.precopy_enabled;
            umemd.rdma = postcopy_rdma_incoming_init(&arg);
        } else {
            /* process_incoming_migration set mig_read to non-blocking
             * mode with corouting for qmp working.
             * Here we switches to its dedicated thread and it expects
             * blocking mode. Otherwise it results in assert by
             * yield_until_fd_readable()
             */
            qemu_set_block(qemu_get_fd(mig_read));
            umemd.mig_read = mig_read;

            mig_write_fd = dup(qemu_get_fd(mig_read));
            if (mig_write_fd < 0) {
                perror("could not dup for writable socket \n");
                return -errno;
            }
            umemd.mig_write = qemu_fdopen(mig_write_fd, "wb");
        }
        qemu_set_nonblock(umemd.fault_write_fd);

        postcopy_incoming_umemd();      /* noreturn */
        return -EINVAL;
    }

    if (is_rdma) {
        postcopy_rdma_incoming_postfork_parent(&arg);
    }
    qemu_add_child_watch(child);
    fd_close(&umemd.to_qemu_fd);
    fd_close(&umemd.from_qemu_fd);
    fd_close(&umemd.fault_write_fd);
    fd_close(&umemd.fault_read_fd);
    postcopy_incoming_umem_block_free();
    postcopy_incoming_create_fault_thread(qemu_fault_read_fd,
                                          qemu_fault_write_fd);

    error = umem_qemu_wait_for_daemon(state.from_umemd_fd);
    if (error) {
        return error;
    }
    /* now socket is disowned. So tell umem thread that it's safe to use it */
    error = umem_qemu_ready(state.to_umemd_fd);
    if (error) {
        return error;
    }

    state.from_umemd = qemu_fdopen(state.from_umemd_fd, "rb");
    state.to_umemd = qemu_fdopen(state.to_umemd_fd, "wb");
    qemu_set_fd_handler(state.from_umemd_fd,
                        postcopy_incoming_qemu_handle_req, NULL, NULL);
    return 0;
}

static int postcopy_incoming_loadvm_section_full(QEMUFile *f, uint32_t size,
                                                 QEMUFile **buf_file)
{
    int error;
    uint8_t *buf;
    int read_size;

    /* as size comes from network, check if it's not unreasonably big
     * At the moment, it is guessed as 16MB.
     */
    DPRINTF("size 0x%"PRIx32"\n", size);
#define SAVE_VM_FULL_SIZE_MAX   (16 * 1024 * 1024)
    if (size > SAVE_VM_FULL_SIZE_MAX) {
        fprintf(stderr,
                "QEMU_VM_POSTCOPY QEMU_VM_POSTCOPY_SECTION_FULL section seems "
                "to have unreasonably big size 0x%x"PRIx32". aborting.\n"
                "If its size is really correct, "
                "please increase it in the code\n",
                size);
        return -EINVAL;
    }

    buf = g_malloc(size);
    read_size = qemu_get_buffer(f, buf, size);
    if (size != read_size) {
        fprintf(stderr, "qemu: warning: error while postcopy size %d %d\n",
                size, read_size);
        g_free(buf);
        return -EINVAL;
    }
    error = postcopy_incoming_create_umemd(f);
    if (error) {
        return error;
    }

    /* VMStateDescription:pre/post_load and
     * cpu_sychronize_all_post_init() may fault on guest RAM.
     * (MSR_KVM_WALL_CLOCK, MSR_KVM_SYSTEM_TIME)
     * postcopy daemon needs to be forked before the fault.
     */
    *buf_file = qemu_fopen_buf_read(buf, size);
    return 0;
}

int postcopy_incoming_loadvm_state(QEMUFile *f, QEMUFile **buf_file)
{
    int ret = 0;
    uint8_t subtype;
    uint32_t size;

    subtype = qemu_get_ubyte(f);
    size = qemu_get_be32(f);
    switch (subtype) {
    case QEMU_VM_POSTCOPY_INIT:
        ret = postcopy_incoming_loadvm_init(f, size);
        break;
    case QEMU_VM_POSTCOPY_SECTION_FULL:
        ret = postcopy_incoming_loadvm_section_full(f, size, buf_file);
        break;
    default:
        ret = -EINVAL;
        break;
    }
    return ret;
}

static void postcopy_incoming_qemu_recv_quit(void)
{
    if (state.state & PIS_STATE_QUIT_RECEIVED) {
        return;
    }

    DPRINTF("|= PIS_STATE_QUIT_RECEIVED\n");
    state.state |= PIS_STATE_QUIT_RECEIVED;
    qemu_set_fd_handler(state.from_umemd_fd, NULL, NULL, NULL);
    qemu_fclose(state.from_umemd);
    state.from_umemd = NULL;
    fd_close(&state.from_umemd_fd);
}

static void postcopy_incoming_qemu_check_quite_queued(void)
{
    if (state.state & PIS_STATE_QUIT_QUEUED &&
        !(state.state & PIS_STATE_QUIT_SENT)) {
        DPRINTF("|= PIS_STATE_QUIT_SENT\n");
        state.state |= PIS_STATE_QUIT_SENT;

        qemu_fclose(state.to_umemd);
        state.to_umemd = NULL;
        fd_close(&state.to_umemd_fd);
    }
}

static void postcopy_incoming_qemu_queue_quit(void)
{
    if (state.state & PIS_STATE_QUIT_QUEUED) {
        return;
    }

    DPRINTF("|= PIS_STATE_QUIT_QUEUED\n");
    umem_qemu_quit(state.to_umemd);
    state.state |= PIS_STATE_QUIT_QUEUED;
}

static void postcopy_incoming_qemu_handle_req(void *opaque)
{
    uint8_t cmd;

    cmd = qemu_get_ubyte(state.from_umemd);
    DPRINTF("cmd %c\n", cmd);

    switch (cmd) {
    case UMEM_DAEMON_QUIT:
        postcopy_incoming_qemu_recv_quit();
        postcopy_incoming_qemu_queue_quit();
        postcopy_incoming_qemu_cleanup();
        break;
    case UMEM_DAEMON_ERROR:
        /* umem daemon hit troubles, so it warned us to stop vm execution */
        vm_stop(RUN_STATE_IO_ERROR); /* or RUN_STATE_INTERNAL_ERROR */
        break;
    default:
        DPRINTF("unknown command %d\n", cmd);
        abort();
        break;
    }

    postcopy_incoming_qemu_check_quite_queued();
}

void postcopy_incoming_qemu_cleanup(void)
{
    /* when qemu will quit before completing postcopy, tell umem daemon
       to tear down umem device and exit. */
    if (state.to_umemd_fd >= 0) {
        postcopy_incoming_qemu_queue_quit();
        postcopy_incoming_qemu_check_quite_queued();
    }
}

struct IncomingFaultArgs {
    int read_fd;
    int write_fd;
};
typedef struct IncomingFaultArgs IncomingFaultArgs;

static void postcopy_incoming_fault_loop(int read_fd, int write_fd)
{
    const int host_page_shift = ffs(getpagesize()) - 1;
    uint64_t buf[PIPE_BUF / sizeof(uint64_t)];
    ssize_t offset = 0;

    for (;;) {
        ssize_t ret;
        int nreq;
        int i;

        ret = read(read_fd, (uint8_t*)buf + offset, sizeof(buf) - offset);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("qemu pipe read\n");
            break;
        }
        if (ret == 0) {
            break;
        }

        offset += ret;
        nreq = offset / sizeof(buf[0]);
        if (nreq == 0) {
            continue;
        }
        /* make pages present by forcibly triggering page fault. */
        qemu_mutex_lock_ramlist();
        for (i = 0; i < nreq; i++) {
            ram_addr_t addr = buf[i] << host_page_shift;
            volatile uint8_t *ram = qemu_safe_ram_ptr(addr);
            if (ram) {
                uint8_t dummy_read = ram[0];
                (void)dummy_read;   /* suppress unused variable warning */
            }
        }
        qemu_mutex_unlock_ramlist();
        ret = qemu_write_full(write_fd, buf, nreq * sizeof(buf[0]));
        if (ret != nreq * sizeof(buf[0])) {
            perror("qemu pipe write\n");
            break;
        }
        memmove(buf, (uint8_t*)buf + ret, offset - ret);
        offset -= ret;
    }

    close(read_fd);
    close(write_fd);
}

static void *postcopy_incoming_fault_thread(void *args)
{
    IncomingFaultArgs *ofa = args;
    int read_fd = ofa->read_fd;
    int write_fd = ofa->write_fd;
    sigset_t set;

    g_free(args);
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    postcopy_incoming_fault_loop(read_fd, write_fd);
    close(read_fd);
    close(write_fd);
    return NULL;
}

static void postcopy_incoming_create_fault_thread(int read_fd, int write_fd)
{
    IncomingFaultArgs *args = g_malloc(sizeof(*args));
    QemuThread thread;

    args->read_fd = read_fd;
    args->write_fd = write_fd;
    qemu_thread_create(&thread, &postcopy_incoming_fault_thread, args,
                       QEMU_THREAD_DETACHED);
}


/**************************************************************************
 * incoming umem daemon
 */

static void postcopy_incoming_umem_error_req(void)
{
    qemu_mutex_lock(&umemd.mutex);
    umemd.state |= UMEM_STATE_ERROR_REQ;
    qemu_mutex_unlock(&umemd.mutex);
}

static void postcopy_incoming_umem_recv_quit(void)
{
    qemu_mutex_lock(&umemd.mutex);
    if (umemd.state & UMEM_STATE_QUIT_RECEIVED) {
        qemu_mutex_unlock(&umemd.mutex);
        return;
    }
    DPRINTF("|= UMEM_STATE_QUIT_RECEIVED\n");
    umemd.state |= UMEM_STATE_QUIT_RECEIVED;
    qemu_mutex_unlock(&umemd.mutex);

    qemu_fclose(umemd.from_qemu);
    umemd.from_qemu = NULL;
    fd_close(&umemd.from_qemu_fd);

    qemu_mutex_lock(&umemd.mutex);
    DPRINTF("|= UMEM_STATE_QUIT_HANDLED\n");
    umemd.state |= UMEM_STATE_QUIT_HANDLED;
    qemu_mutex_unlock(&umemd.mutex);
}

/* call with umemd.mutex held */
static void postcopy_incoming_umem_queue_quit_locked(void)
{
    if (umemd.state & UMEM_STATE_QUIT_QUEUED) {
        return;
    }
    DPRINTF("|= UMEM_STATE_QUIT_QUEUED\n");
    umemd.state |= UMEM_STATE_QUIT_QUEUED;
}

static void postcopy_incoming_umem_check_eoc_req(void)
{
    QEMUUMemReq req;

    qemu_mutex_lock(&umemd.mutex);
    if (!(umemd.state & UMEM_STATE_EOC_SEND_REQ) ||
        umemd.state & (UMEM_STATE_EOC_SENDING | UMEM_STATE_EOC_SENT)) {
        qemu_mutex_unlock(&umemd.mutex);
        return;
    }

    DPRINTF("|= UMEM_STATE_EOC_SENDING\n");
    umemd.state |= UMEM_STATE_EOC_SENDING;
    qemu_mutex_unlock(&umemd.mutex);

    req.cmd = QEMU_UMEM_REQ_EOC;
    postcopy_incoming_send_req(umemd.mig_write, umemd.rdma, &req, NULL);
    if (umemd.mig_write != NULL) {
        qemu_fclose(umemd.mig_write);
        umemd.mig_write = NULL;
    }

    qemu_mutex_lock(&umemd.mutex);
    DPRINTF("|= UMEM_STATE_EOC_SENT\n");
    umemd.state |= UMEM_STATE_EOC_SENT;
    qemu_mutex_unlock(&umemd.mutex);
}

void postcopy_incoming_umem_req_eoc(void)
{
    qemu_mutex_lock(&umemd.mutex);
    DPRINTF("|= UMEM_STATE_EOC_SEND_REQ\n");
    umemd.state |= UMEM_STATE_EOC_SEND_REQ;
    qemu_mutex_unlock(&umemd.mutex);
}

static int postcopy_incoming_umem_send_page_req(UMemBlock *block)
{
    int error;
    QEMUUMemReq req;
    uint64_t target_pgoff;
    int i;

    umemd.page_request->nr = MAX_REQUESTS;
    error = umem_get_page_request(block->umem, umemd.page_request);
    if (error) {
        return error;
    }
    DPRINTF("id %s nr %"PRId64" offs 0x%"PRIx64" 0x%"PRIx64"\n",
            block->idstr, (uint64_t)umemd.page_request->nr,
            (uint64_t)umemd.page_request->pgoffs[0],
            (uint64_t)umemd.page_request->pgoffs[1]);

    if (umemd.last_block_write != block) {
        req.cmd = QEMU_UMEM_REQ_PAGE;
        pstrcpy(req.idstr, sizeof(req.idstr), block->idstr);
    } else {
        req.cmd = QEMU_UMEM_REQ_PAGE_CONT;
    }

    req.nr = 0;
    req.pgoffs = umemd.target_pgoffs;
    umemd.page_clean->nr = 0;
    if (TARGET_PAGE_SIZE >= umemd.host_page_size) {
        for (i = 0; i < umemd.page_request->nr; i++) {
            target_pgoff = umemd.page_request->pgoffs[i] >>
                umemd.host_to_target_page_shift;
            if ((umemd.precopy_enabled &&
                 /* race with postcopy_incoming_umemd_read_clean_bitmap
                    but it results in sending redundant page req */
                 test_bit(target_pgoff, block->clean_bitmap)) ||
                /* race with postcopy_incoming_umem_ram_loaded
                   bit it results in avoiding duplicated mark_cached */
                test_bit(target_pgoff, block->phys_received)) {
                int j;
                for (j = 0; j < umemd.nr_host_pages_per_target_page; j++) {
                    umemd.page_clean->pgoffs[umemd.page_clean->nr] =
                        umemd.page_request->pgoffs[i] + j;
                    umemd.page_clean->nr++;
                }
            } else if (!test_and_set_bit(target_pgoff,
                                         block->phys_requested)) {
                req.pgoffs[req.nr] = target_pgoff;
                req.nr++;
            }
        }
    } else {
        for (i = 0; i < umemd.page_request->nr; i++) {
            int j;
            bool marked_clean = true;
            target_pgoff = umemd.page_request->pgoffs[i] <<
                umemd.host_to_target_page_shift;
            for (j = 0; j < umemd.nr_target_pages_per_host_page; j++) {
                if (umemd.precopy_enabled &&
                    /* race with postcopy_incoming_umemd_read_clean_bitmap
                       but it results in sending redundant page req */
                    test_bit(target_pgoff + j, block->clean_bitmap)) {
                    continue;
                }
                /* race with postcopy_incoming_umem_ram_loaded
                   bit it results in avoiding duplicate mark cached */
                if (test_bit(target_pgoff + j, block->phys_received)) {
                    continue;
                }

                marked_clean = false;
                break;
            }
            if (marked_clean) {
                umemd.page_clean->pgoffs[umemd.page_clean->nr] =
                    umemd.page_request->pgoffs[i];
                umemd.page_clean->nr++;
            } else {
                for (j = 0; j < umemd.nr_target_pages_per_host_page; j++) {
                    if (!test_and_set_bit(target_pgoff + j,
                                          block->phys_requested)) {
                        req.pgoffs[req.nr] = target_pgoff + j;
                        req.nr++;
                    }
                }
            }
        }
    }

    DPRINTF("id %s nr %d offs 0x%"PRIx64" 0x%"PRIx64"\n",
            block->idstr, req.nr, req.pgoffs[0], req.pgoffs[1]);
    if (umemd.page_clean->nr > 0) {
        int error = umem_mark_page_cached(block->umem, umemd.page_clean);
        if (error) {
            return error;
        }
    }
    if (req.nr > 0) {
        if (umemd.mig_write != NULL || umemd.rdma != NULL) {
            postcopy_incoming_send_req(umemd.mig_write, umemd.rdma, &req,
                                       block);
            umemd.last_block_write = block;
        }
    }
    return 0;
}

static void postcopy_incoming_umem_done(void)
{
    postcopy_incoming_umem_req_eoc();
    qemu_mutex_lock(&umemd.mutex);
    postcopy_incoming_umem_queue_quit_locked();
    qemu_mutex_unlock(&umemd.mutex);
}

static bool postcopy_incoming_umem_check_umem_done(void)
{
    bool all_done = true;
    UMemBlock *block;

    QLIST_FOREACH(block, &umemd.blocks, next) {
        if (umem_shmem_finished(block->umem)) {
            umem_unmap_shmem(block->umem);
        } else {
            all_done = false;
            break;
        }
    }

    if (all_done) {
        postcopy_incoming_umem_done();
    }
    return all_done;
}

static UMemBlock *postcopy_incoming_umem_block_from_stream(
    QEMUFile *f, int flags)
{
    uint8_t len;
    char id[256];
    UMemBlock *block;

    if (flags & RAM_SAVE_FLAG_CONTINUE) {
        return umemd.last_block_read;
    }

    len = qemu_get_byte(f);
    qemu_get_buffer(f, (uint8_t*)id, len);
    id[len] = 0;

    DPRINTF("idstr: %s len %d\n", id, len);
    QLIST_FOREACH(block, &umemd.blocks, next) {
        if (!strncmp(id, block->idstr, len)) {
            umemd.last_block_read = block;
            return block;
        }
    }
    DPRINTF("error\n");
    return NULL;
}

static void postcopy_incoming_umem_wait_fault_write_fd(void)
{
    /* wait for umemd.fault_write_fd to be writable */
    int nfds = -1;
    fd_set writefds;

    FD_ZERO(&writefds);
    set_fd(umemd.fault_write_fd, &writefds, &nfds);
    select(nfds + 1, NULL, &writefds, NULL, NULL);
}

static void postcopy_incoming_umem_mark_pending_clean(
    const UMemPages *page_cached)
{
    /* record it for postcopy_incoming_umem_pending_clean_loop() */
    bool wakeup = false;
    int i;

    DPRINTF("EAGAIN\n");
    qemu_mutex_lock(&umemd.pending_clean_mutex);
    for (i = 0; i < umemd.page_cached->nr; ++i) {
        /* Although this calculation is inefficient,
         * this code path is rare case.
         */
        uint64_t pgoff = page_cached->pgoffs[i];
        uint64_t addr = pgoff << umemd.host_page_shift;
        UMemBlock *block;

        QLIST_FOREACH(block, &umemd.blocks, next) {
            if (block->offset <= addr &&
                addr < block->offset + block->length) {
                addr -= block->offset;
                pgoff = addr >> umemd.host_page_shift;
                if (!test_and_set_bit(pgoff, block->pending_clean_bitmap)) {
                    block->nr_pending_clean++;
                    umemd.nr_pending_clean++;
                    wakeup = true;
                }
                break;
            }
        }
    }
    qemu_mutex_unlock(&umemd.pending_clean_mutex);
    if (wakeup) {
        qemu_cond_broadcast(&umemd.pending_clean_cond);
    }
}

static int postcopy_incoming_umem_fault_request(const UMemPages *page_cached,
                                                bool nonblock)
{
    size_t length = page_cached->nr * sizeof(page_cached->pgoffs[0]);
    const uint8_t *buf = (const uint8_t*)page_cached->pgoffs;

    while (length > 0) {
        /* atomic write to pipe */
        ssize_t size = MIN(PIPE_BUF, length) & ~(sizeof(uint64_t) - 1);
        ssize_t ret = qemu_write_full(umemd.fault_write_fd, buf, size);
        if (ret != size) {
            int error = -errno;
            if (error == -EAGAIN || error == -EWOULDBLOCK) {
                if (nonblock) {
                    postcopy_incoming_umem_mark_pending_clean(page_cached);
                    break;
                }
                postcopy_incoming_umem_wait_fault_write_fd();
                continue;
            }
            DPRINTF("error ret %zd size %zd errno %d\n", ret, size, errno);
            return error;
        }
        length -= size;
        buf += size;
    }
    return 0;
}


static int postcopy_incoming_umem_mark_cached(
    UMem *umem, const UMemPages *page_cached)
{
    int error = umem_mark_page_cached(umem, page_cached);
    if (error) {
        DPRINTF("mark_cahced %d\n", error);
        return error;
    }

    return postcopy_incoming_umem_fault_request(page_cached, true);
}

int postcopy_incoming_umem_ram_loaded(UMemBlock *block, ram_addr_t offset)
{
    int i;
    int bit = offset >> TARGET_PAGE_BITS;

    umemd.page_cached->nr = 0;
    if (!test_and_set_bit(bit, block->phys_received)) {
        if (TARGET_PAGE_SIZE >= umemd.host_page_size) {
            uint64_t pgoff = offset >> umemd.host_page_shift;
            for (i = 0; i < umemd.nr_host_pages_per_target_page; i++) {
                umemd.page_cached->pgoffs[umemd.page_cached->nr] = pgoff + i;
                umemd.page_cached->nr++;
            }
        } else {
            bool mark_cache = true;
            bit &= ~(umemd.nr_host_pages_per_target_page - 1);
            for (i = 0; i < umemd.nr_target_pages_per_host_page; i++) {
                if (!test_bit(bit + i, block->phys_received)) {
                    mark_cache = false;
                    break;
                }
            }
            if (mark_cache) {
                umemd.page_cached->pgoffs[0] = offset >> umemd.host_page_shift;
                umemd.page_cached->nr = 1;
            }
        }
    }

    if (umemd.page_cached->nr > 0) {
        int error = postcopy_incoming_umem_mark_cached(block->umem,
                                                       umemd.page_cached);
        if (error) {
            perror("postcopy_incoming_umem_ram_load() write pipe\n");
            return error;
        }
    }
    return 0;
}

void postcopy_incoming_umem_eos_received(void)
{
    qemu_mutex_lock(&umemd.mutex);
    postcopy_incoming_umem_queue_quit_locked();
    umemd.state |= UMEM_STATE_EOS_RECEIVED;
    qemu_mutex_unlock(&umemd.mutex);
    DPRINTF("|= UMEM_STATE_EOS_RECEIVED\n");
}

static int postcopy_incoming_umem_ram_load(void)
{
    ram_addr_t offset;
    uint64_t flags;
    UMemBlock *block;

    void *shmem;
    int error;

    if (umemd.version_id != RAM_SAVE_VERSION_ID) {
        return -EINVAL;
    }

    error = postcopy_incoming_ram_load_get64(umemd.mig_read, &offset, &flags);
    //DPRINTF("offset 0x%lx flags 0x%"PRIx64"\n", offset, flags);
    if (error) {
        DPRINTF("error %d\n", error);
        return error;
    }
    assert(!(flags & RAM_SAVE_FLAG_MEM_SIZE));

    if (flags & RAM_SAVE_FLAG_EOS) {
        DPRINTF("RAM_SAVE_FLAG_EOS\n");
        postcopy_incoming_umem_req_eoc();

        qemu_fclose(umemd.mig_read);
        umemd.mig_read = NULL;

        postcopy_incoming_umem_eos_received();
        return 0;
    }

    if (!(flags & (RAM_SAVE_FLAG_COMPRESS | RAM_SAVE_FLAG_PAGE |
                   RAM_SAVE_FLAG_XBZRLE))) {
        DPRINTF("unknown flags 0x%"PRIx64"\n", flags);
        return 0;
    }

    block = postcopy_incoming_umem_block_from_stream(umemd.mig_read,
                                                     flags);
    if (block == NULL) {
        return -EINVAL;
    }
    assert(!umem_shmem_finished(block->umem));
    shmem = block->umem->shmem + offset;
    error = ram_load_page(umemd.mig_read, shmem, flags);
    if (error) {
        DPRINTF("error %d\n", error);
        return error;
    }

    error = qemu_file_get_error(umemd.mig_read);
    if (error) {
        DPRINTF("error %d\n", error);
        return error;
    }

    return postcopy_incoming_umem_ram_loaded(block, offset);
}

static int postcopy_incoming_umemd_pending_clean_loop(void)
{
    uint64_t buffer[(sizeof(UMemPages) + PIPE_BUF + 7) / sizeof(uint64_t)];
    UMemPages * const page_cached = (UMemPages*)buffer;
    const int max_nr = PIPE_BUF / sizeof(uint64_t) - 1;
    UMemBlock *block;
    int error;

    DPRINTF("pending clean bitmap\n");
    QLIST_FOREACH(block, &umemd.blocks, next) {
        const unsigned long nbits = (block->length >> umemd.host_page_shift);
        unsigned long bit;

        if (block->nr_pending_clean == 0) {
            continue;
        }

        DPRINTF("idstr %s\n", block->idstr);
        page_cached->nr = 0;
        for (bit = find_first_bit(block->pending_clean_bitmap, nbits);
             bit < nbits;
             bit = find_next_bit(block->pending_clean_bitmap, nbits, ++bit)) {
            clear_bit(bit, block->pending_clean_bitmap);
            block->nr_pending_clean--;
            umemd.nr_pending_clean--;
            page_cached->pgoffs[page_cached->nr] = bit;
            page_cached->nr++;

            if (page_cached->nr == max_nr) {
                qemu_mutex_unlock(&umemd.pending_clean_mutex);
                error = postcopy_incoming_umem_fault_request(page_cached,
                                                             false);
                qemu_mutex_lock(&umemd.pending_clean_mutex);
                if (error) {
                    goto error_out;
                }
                page_cached->nr = 0;
            }
        }
        if (page_cached->nr > 0) {
            qemu_mutex_unlock(&umemd.pending_clean_mutex);
            error = postcopy_incoming_umem_fault_request(page_cached, false);
            qemu_mutex_lock(&umemd.pending_clean_mutex);
            if (error) {
                goto error_out;
            }
        }
    }

    DPRINTF("pending clean bitmap done\n");
    return 0;

error_out:
    perror("umemd clean bitmap pipe write\n");
    fd_close(&umemd.fault_write_fd);
    return error;
}

static void *postcopy_incoming_umemd_pending_clean_thread(void* arg)
{
    DPRINTF("postcopy_incoming_umemd_pending_clean_thread starts\n");
    qemu_mutex_lock(&umemd.pending_clean_mutex);
    for (;;) {
        bool do_sleep;
        int error;

        if (umemd.nr_pending_clean == 0) {
            if (umemd.pending_clean_exit) {
                break;
            }
            qemu_cond_wait(&umemd.pending_clean_cond,
                           &umemd.pending_clean_mutex);
            continue;
        }

        /*
         * the pipe of umemd.fault_write_fd is full.
         * give postcopy_incoming_fault_thread() a chance to process.
         * postcopy_incoming_umem_ram_load() is likely to set more
         * bits in pending_clean_bitmap. Increase the possibility of batching.
         */
        do_sleep = !umemd.pending_clean_exit;
        qemu_mutex_unlock(&umemd.pending_clean_mutex);
        postcopy_incoming_umem_wait_fault_write_fd();
        if (do_sleep) {
            struct timespec timespec = {.tv_sec = 1, .tv_nsec = 0};
            nanosleep(&timespec, NULL);
        }
        qemu_mutex_lock(&umemd.pending_clean_mutex);

        error = postcopy_incoming_umemd_pending_clean_loop();
        if (error < 0) {
            DPRINTF("postcopy_incoming_umemd_pending_clean_loop "
                    "error = %d\n", error);
            break;
        }
    }
    qemu_mutex_unlock(&umemd.pending_clean_mutex);
    DPRINTF("postcopy_incoming_umemd_pending_clean_thread exits\n");
    return NULL;
}

static void postcopy_incoming_umemd_pending_clean_create(void)
{
    qemu_thread_create(&umemd.pending_clean_thread,
                       &postcopy_incoming_umemd_pending_clean_thread, NULL,
                       QEMU_THREAD_JOINABLE);
}

static void *postcopy_incoming_umemd_fault_clean_bitmap(void *args)
{
    int i;
    UMemBlock *block;
    uint64_t buffer[(sizeof(UMemPages) + PIPE_BUF + 7) / sizeof(uint64_t)];
    UMemPages * const page_cached = (UMemPages*)buffer;
    int max_nr = PIPE_BUF / sizeof(uint64_t);
    int needed;

    if (TARGET_PAGE_SIZE >= umemd.host_page_size) {
        needed = umemd.nr_host_pages_per_target_page;
    } else {
        needed = 1;
    }
    assert(needed <= max_nr);

    DPRINTF("faulting clean bitmap\n");
    QLIST_FOREACH(block, &umemd.blocks, next) {
        const unsigned long nbits = (block->length >> TARGET_PAGE_BITS);
        unsigned long bit;
        int error;

        DPRINTF("idstr %s\n", block->idstr);
        page_cached->nr = 0;
        for (bit = find_first_bit(block->clean_bitmap, nbits);
             bit < nbits;
             bit = find_next_bit(block->clean_bitmap, nbits, ++bit)) {
            if (TARGET_PAGE_SIZE >= umemd.host_page_size) {
                uint64_t pgoff = bit << umemd.target_to_host_page_shift;
                for (i = 0; i < umemd.nr_host_pages_per_target_page; i++) {
                    page_cached->pgoffs[page_cached->nr] = pgoff + i;
                    page_cached->nr++;
                }
            } else {
                bool mark_cache = true;
                if ((bit % umemd.nr_target_pages_per_host_page) != 0) {
                    /* skip to next host page */
                    bit |= umemd.nr_target_pages_per_host_page - 1;
                    continue;
                }
                for (i = 0; i < umemd.nr_target_pages_per_host_page; i++) {
                    if (!test_bit(bit + i, block->clean_bitmap)) {
                        mark_cache = false;
                        break;
                    }
                }
                if (mark_cache) {
                    page_cached->pgoffs[page_cached->nr] =
                        bit >> (umemd.host_page_shift - TARGET_PAGE_BITS);
                    page_cached->nr++;
                }
            }
            if (max_nr - page_cached->nr < needed) {
                error = postcopy_incoming_umem_mark_cached(block->umem,
                                                           page_cached);
                if (error) {
                    goto error_out;
                }
                page_cached->nr = 0;
            }
        }
        if (page_cached->nr > 0) {
            error = postcopy_incoming_umem_mark_cached(block->umem,
                                                       page_cached);
            if (error) {
                goto error_out;
            }
        }
    }

out:
    DPRINTF("faulting clean bitmap done\n");
    postcopy_incoming_umemd_pending_clean_create();
    return NULL;

error_out:
    perror("umemd bitmap pipe write\n");
    fd_close(&umemd.fault_write_fd);
    goto out;
}

uint64_t postcopy_bitmap_length(uint64_t length)
{
    return DIV_ROUND_UP(length >> TARGET_PAGE_BITS, 64) * sizeof(uint64_t);
}

void postcopy_be64_to_bitmap(uint8_t *buffer, uint64_t length)
{
    unsigned long *bitmap = (unsigned long*)buffer;
    uint64_t i;
    assert(length % sizeof(uint64_t) == 0);

    for (i = 0; i < length; i += sizeof(uint64_t)) {
        uint64_t val = be64_to_cpup((uint64_t*)(buffer + i));

#if HOST_LONG_BITS == 64
        bitmap[i / sizeof(*bitmap)] = val;
#elif HOST_LONG_BITS == 32
        bitmap[i / sizeof(*bitmap)] = val;
        bitmap[i / sizeof(*bitmap) + 1] = (val >> 32);
#else
# error "unsupported"
#endif
    }
}

void postcopy_incoming_umemd_read_clean_bitmap_done(UMemBlock *block)
{
    bitmap_copy(block->phys_requested, block->phys_received,
                block->length >> TARGET_PAGE_BITS);
    /* race with postcopy_incoming_umem_send_page_req,
       but it only sends redundant page requests which will be discarded */
    bitmap_copy(block->clean_bitmap, block->phys_received,
                block->length >> TARGET_PAGE_BITS);
}

static int postcopy_incoming_umemd_read_clean_bitmap(
    QEMUFile *f, const char *idstr, uint8_t idlen,
    uint64_t block_offset, uint64_t block_length, uint64_t bitmap_length)
{
    UMemBlock *block;
    uint8_t* buffer;

    if ((bitmap_length % sizeof(uint64_t)) != 0) {
        DPRINTF("block %s invalid length 0x%"PRIx64"\n", idstr, bitmap_length);
        return -EINVAL;
    }
    QLIST_FOREACH(block, &umemd.blocks, next) {
        if (!strncmp(block->idstr, idstr, idlen)) {
            break;
        }
    }
    if (block == NULL) {
        DPRINTF("Unknown block %s\n", idstr);
        return -EINVAL;
    }

    DPRINTF("bitmap %s 0x%"PRIx64" 0x%"PRIx64" 0x%"PRIx64"\n",
            block->idstr, block_offset, block_length, bitmap_length);

    /* setting phys_requested is racey,
       but write side just sends redundant request */
    buffer = (uint8_t*)block->phys_received;
    qemu_get_buffer(f, buffer, bitmap_length);
    postcopy_be64_to_bitmap(buffer, bitmap_length);

    postcopy_incoming_umemd_read_clean_bitmap_done(block);
    return 0;
}

static int postcopy_file_incoming_umemd_read_clean_bitmap(void)
{
    QEMUFile *f = umemd.mig_read;

    for (;;) {
        uint8_t idlen;
        char idstr[256];
        uint64_t block_offset;
        uint64_t block_length;
        uint64_t bitmap_length;
        int ret;

        idlen = qemu_get_byte(f);
        qemu_get_buffer(f, (uint8_t*)idstr, idlen);
        idstr[idlen] = 0;
        block_offset = qemu_get_be64(f);
        block_length = qemu_get_be64(f);
        bitmap_length = qemu_get_be64(f);

        if (idlen == 0 && block_offset == 0 && block_length == 0 &&
            bitmap_length == 0) {
            DPRINTF("bitmap done\n");
            break;
        }
        ret = postcopy_incoming_umemd_read_clean_bitmap(
            f, idstr, idlen, block_offset, block_length, bitmap_length);
        if (ret < 0) {
            DPRINTF("bitmap error %d\n", ret);
            return ret;
        }
    }

    return 0;
}

static int postcopy_incoming_umemd_mig_read_init(void)
{
#ifdef DEBUG_POSTCOPY
    uint64_t start = qemu_get_clock_ns(rt_clock);
    uint64_t end;
#endif
    int ret;

    if (!umemd.precopy_enabled) {
        postcopy_incoming_umemd_pending_clean_create();
        return 0;
    }

    if (umemd.rdma != NULL) {
        ret = postcopy_rdma_incoming_umemd_read_clean_bitmap(umemd.rdma,
                                                             &umemd.blocks);
    } else {
        ret = postcopy_file_incoming_umemd_read_clean_bitmap();
    }
    if (ret) {
        return ret;
    }

    qemu_thread_create(&umemd.bitmap_thread,
                       &postcopy_incoming_umemd_fault_clean_bitmap,
                       NULL, QEMU_THREAD_JOINABLE);
    postcopy_incoming_umem_check_umem_done();
#ifdef DEBUG_POSTCOPY
    end = qemu_get_clock_ns(rt_clock);
    DPRINTF("bitmap %"PRIu64" nsec\n", end - start);
#endif
    return 0;
}

static int postcopy_incoming_umemd_mig_read_loop(void)
{
    int error;
    /* read thread doesn't need to check periodically UMEM_STATE_EOC_SEND_REQ
     * because RAM_SAVE_FLAG_EOS is always sent by the outgoing part. */

    if (umemd.rdma != NULL) {
        /* TODO: use function pointer */
        error = postcopy_rdma_incoming_recv(umemd.rdma);
    } else {
        if (umemd.mig_read == NULL) {
            return -EINVAL;
        }
        error = postcopy_incoming_umem_ram_load();
    }

    if (error) {
        postcopy_incoming_umem_error_req();
    }
    return error;
}

static int postcopy_incoming_umemd_mig_write_loop(void)
{
    int ret;
    UMemBlock *block;
    /* to check UMEM_STATE_EOC_SEND_REQ periodically */
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    int nfds = -1;
    fd_set readfds;
    FD_ZERO(&readfds);

    QLIST_FOREACH(block, &umemd.blocks, next) {
        set_fd(block->umem->fd, &readfds, &nfds);
    }
    ret = select(nfds + 1, &readfds, NULL, NULL, &timeout);
    if (ret == -1) {
        if (errno == EINTR) {
            return 0;
        }
        return ret;
    }
    QLIST_FOREACH(block, &umemd.blocks, next) {
        if (FD_ISSET(block->umem->fd, &readfds)) {
            ret = postcopy_incoming_umem_send_page_req(block);
            if (ret) {
                postcopy_incoming_umem_error_req();
                return ret;
            }
        }
    }
    if (umemd.mig_write != NULL) {
        qemu_fflush(umemd.mig_write);
    }
    postcopy_incoming_umem_check_eoc_req();

    return 0;
}

static int postcopy_incoming_umemd_pipe_init(void)
{
    int error;
    error = umem_daemon_ready(umemd.to_qemu_fd);
    if (error) {
        goto out;
    }
    umemd.to_qemu = qemu_fdopen(umemd.to_qemu_fd, "wb");

    /* wait for qemu to disown migration_fd */
    error = umem_daemon_wait_for_qemu(umemd.from_qemu_fd);
    if (error) {
        goto out;
    }
    umemd.from_qemu = qemu_fdopen(umemd.from_qemu_fd, "rb");
    return 0;

out:
    /* Here there is no way to tell error to main thread
       in order to teardown. */
    perror("initialization error");
    abort();
    return error;
}

static int postcopy_incoming_umemd_pipe_loop(void)
{
    int ret;
    /* to check UMEM_STATE_QUIT_QUEUED periodically */
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    fd_set readfds;
    int nfds = -1;

    FD_ZERO(&readfds);
    if (umemd.from_qemu_fd >= 0) {
        set_fd(umemd.from_qemu_fd, &readfds, &nfds);
    }
    ret = select(nfds + 1, &readfds, NULL, NULL, &timeout);
    if (ret == -1) {
        if (errno == EINTR) {
            return 0;
        }
        return ret;
    }
    if (umemd.from_qemu_fd >= 0 && FD_ISSET(umemd.from_qemu_fd, &readfds)) {
        uint8_t cmd;
        cmd = qemu_get_ubyte(umemd.from_qemu);
        DPRINTF("cmd %c 0x%x\n", cmd, cmd);
        switch (cmd) {
        case UMEM_QEMU_QUIT:
            postcopy_incoming_umem_recv_quit();
            postcopy_incoming_umem_done();
            break;
        case 0:
            /* qemu_get_ubyte() return 0 when pipe is closed */
            break;
        default:
            abort();
            break;
        }
        if (umemd.to_qemu != NULL) {
            qemu_fflush(umemd.to_qemu);
        }
    }

    if (umemd.to_qemu != NULL) {
        qemu_mutex_lock(&umemd.mutex);
        if (umemd.state & UMEM_STATE_ERROR_REQ &&
            !(umemd.state & UMEM_STATE_ERROR_SENDING)) {
            umemd.state |= UMEM_STATE_ERROR_SENDING;
            qemu_mutex_unlock(&umemd.mutex);
            umem_daemon_error(umemd.to_qemu);
            qemu_mutex_lock(&umemd.mutex);
            umemd.state |= UMEM_STATE_ERROR_SENT;
        }
        if (umemd.state & UMEM_STATE_QUIT_QUEUED &&
            !(umemd.state & (UMEM_STATE_QUIT_SENDING |
                             UMEM_STATE_QUIT_SENT))) {
            DPRINTF("|= UMEM_STATE_QUIT_SENDING\n");
            umemd.state |= UMEM_STATE_QUIT_SENDING;
            qemu_mutex_unlock(&umemd.mutex);

            umem_daemon_quit(umemd.to_qemu);
            qemu_fclose(umemd.to_qemu);
            umemd.to_qemu = NULL;
            fd_close(&umemd.to_qemu_fd);

            qemu_mutex_lock(&umemd.mutex);
            DPRINTF("|= UMEM_STATE_QUIT_SENT\n");
            umemd.state |= UMEM_STATE_QUIT_SENT;
        }
        qemu_mutex_unlock(&umemd.mutex);
    }

    return 0;
}

/*
 * return value
 * 0: success. loop continues
 * 1: success. loop exits
 * <0: error
 */
static int postcopy_incoming_umemd_fault_loop(void)
{
    ssize_t ret;
    int i;
    int nreq;

    ret = read(umemd.fault_read_fd, (uint8_t*)umemd.buf + umemd.offset,
               sizeof(umemd.buf) - umemd.offset);
    if (ret < 0) {
        if (errno == EINTR) {
            return 0;
        }
        perror("umemd pipe read\n");
        return ret;
    }
    if (ret == 0) {
        /* EOF: pipe is closed */
        return 1;
    }

    umemd.offset += ret;
    nreq = umemd.offset / sizeof(umemd.buf[0]);
    for (i = 0; i < nreq; i++) {
        uint64_t addr = umemd.buf[i] << umemd.host_page_shift;
        UMemBlock *block;
        QLIST_FOREACH(block, &umemd.blocks, next) {
            if (block->offset <= addr &&
                addr < block->offset + block->length) {
                umem_remove_shmem(block->umem, addr - block->offset,
                                  umemd.host_page_size);
                break;
            }
        }
        if (block == NULL) {
            DPRINTF("unknown offset 0x%"PRIx64"\n", addr);
            abort();
        }
    }
    umemd.offset &= sizeof(umemd.buf[0]) - 1;
    memmove(umemd.buf, (uint8_t*)umemd.buf + nreq * sizeof(umemd.buf[0]),
            umemd.offset);

    return postcopy_incoming_umem_check_umem_done()? 1: 0;
}

static void *postcopy_incoming_umemd_fault_thread(void* arg)
{
    for (;;) {
        int error = postcopy_incoming_umemd_fault_loop();
        if (error < 0) {
            DPRINTF("postcopy_incoming_umemd_fault_loop error = %d\n", error);
        }
        if (error) {
            break;
        }
    }
    DPRINTF("postcopy_incoming_umemd_fault_thread exits\n");
    fd_close(&umemd.fault_read_fd);
    return NULL;
}


struct IncomingThread {
    int (*init_func)(void);
    int (*loop_func)(void);
};
typedef struct IncomingThread IncomingThread;

static void *postcopy_incoming_umemd_thread(void* arg)
{
    IncomingThread *im  = arg;
    int error;

    DPRINTF("loop %d %p %p\n", getpid(), im->init_func, im->loop_func);
    if (im->init_func) {
        error = im->init_func();
        if (error) {
            postcopy_incoming_umem_error_req();
            return NULL;
        }
    }
    for (;;) {
        qemu_mutex_lock(&umemd.mutex);
        if ((umemd.state & UMEM_STATE_END_MASK) == UMEM_STATE_END_MASK) {
            qemu_mutex_unlock(&umemd.mutex);
            DPRINTF("loop out %p\n", im->loop_func);
            break;
        }
        qemu_mutex_unlock(&umemd.mutex);

        error = im->loop_func();
        if (error) {
            DPRINTF("func %p error = %d\n", im->loop_func, error);
            break;
        }
    }
    return NULL;
}

static void postcopy_incoming_umemd(void)
{
    QemuThread umemd_fault_thread;

    signal(SIGPIPE, SIG_IGN);
    DPRINTF("daemon pid: %d\n", getpid());

    umemd.page_request = g_malloc(umem_pages_size(MAX_REQUESTS));
    umemd.page_clean = g_malloc(
        umem_pages_size(MAX_REQUESTS *
                        MAX(1, umemd.nr_host_pages_per_target_page)));
    umemd.page_cached = g_malloc(
        umem_pages_size(MAX_REQUESTS *
                        (TARGET_PAGE_SIZE >= umemd.host_page_size ?
                         1: umemd.nr_host_pages_per_target_page)));
    umemd.target_pgoffs =
        g_new(uint64_t, MAX_REQUESTS *
              MAX(umemd.nr_host_pages_per_target_page,
                  umemd.nr_target_pages_per_host_page));

    umemd.pending_clean_exit = false;
    umemd.nr_pending_clean = 0;
    qemu_mutex_init(&umemd.pending_clean_mutex);
    qemu_cond_init(&umemd.pending_clean_cond);
    umemd.last_block_read = NULL;
    umemd.last_block_write = NULL;

    qemu_thread_create(&umemd_fault_thread,
                       &postcopy_incoming_umemd_fault_thread, NULL,
                       QEMU_THREAD_JOINABLE);
    qemu_thread_create(&umemd.mig_read_thread,
                       &postcopy_incoming_umemd_thread,
                       &(IncomingThread) {
                           &postcopy_incoming_umemd_mig_read_init,
                           &postcopy_incoming_umemd_mig_read_loop,},
                       QEMU_THREAD_JOINABLE);
    qemu_thread_create(&umemd.mig_write_thread,
                       &postcopy_incoming_umemd_thread,
                       &(IncomingThread) {
                           NULL, &postcopy_incoming_umemd_mig_write_loop,},
                       QEMU_THREAD_JOINABLE);
    qemu_thread_create(&umemd.pipe_thread, &postcopy_incoming_umemd_thread,
                       &(IncomingThread) {
                           &postcopy_incoming_umemd_pipe_init,
                           &postcopy_incoming_umemd_pipe_loop,},
                       QEMU_THREAD_JOINABLE);

    qemu_thread_join(&umemd.mig_read_thread);
    if (umemd.precopy_enabled) {
        qemu_thread_join(&umemd.bitmap_thread);
    }
    qemu_thread_join(&umemd.mig_write_thread);
    qemu_thread_join(&umemd.pipe_thread);

    qemu_mutex_lock(&umemd.pending_clean_mutex);
    umemd.pending_clean_exit = true;
    qemu_cond_broadcast(&umemd.pending_clean_cond);
    qemu_mutex_unlock(&umemd.pending_clean_mutex);
    qemu_thread_join(&umemd.pending_clean_thread);

    /* To tell postcopy_incmoing_fault_loop that umemd finished.
     * Then, postcopy_incoming_fault_loop() tells
     * postcopy_incoming_umemd_fault_loop() by closing fd.
     * Then postcopy_incoming_umemd_fault_loop() exits.
     */
    fd_close(&umemd.fault_write_fd);
    qemu_thread_join(&umemd_fault_thread);

    if (umemd.rdma) {
        postcopy_rdma_incoming_cleanup(umemd.rdma);
    }

    g_free(umemd.page_request);
    g_free(umemd.page_clean);
    g_free(umemd.page_cached);
    g_free(umemd.target_pgoffs);

    postcopy_incoming_umem_block_free();
    qemu_mutex_destroy(&umemd.pending_clean_mutex);
    qemu_cond_destroy(&umemd.pending_clean_cond);
    assert(umemd.nr_pending_clean == 0);

    DPRINTF("umemd done\n");
    /* This daemon forked from qemu and the parent qemu is still running.
     * Cleanups of linked libraries like SDL should not be triggered,
     * otherwise the parent qemu may use resources which was already freed.
     */
    fflush(stdout);
    fflush(stderr);
    _exit(0);
}
