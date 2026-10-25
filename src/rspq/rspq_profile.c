/**
 * @file rspq_profile.c
 * @author Dennis Heinze <dennisjp.heinze@gmail.com>
 * @author Giovanni Bajo <giovannibajo@gmail.com>
 * @brief RSP Command queue: profiler
 * @ingroup rsp
 */
#include "rspq_profile.h"
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "n64sys.h"
#include "debug.h"
#include "rsp.h"
#include "rspq.h"
#include "rspq_internal.h"

#if RSPQ_PROFILE

DEFINE_RSP_UCODE(rsp_profile);

#define CMD_PROFILE_FRAME 0x0

/**
 * @brief Profile data as written to RDRAM by the RSP on each frame marker.
 *
 * This must match PROFILE_STATE in rsp_profile.S. All values are running
 * totals since the RSP started, so the CPU works with differences between
 * two samples. The two counters bracket the data: the RSP increments both
 * before DMAing the buffer, and the DMA writes the words in address order,
 * so the CPU can detect a torn read by comparing the counter read before
 * the copy with the one read after.
 */
typedef struct {
    uint32_t cntr1;                                     ///< Sample counter (first word written)
    uint32_t padding1;                                  ///< Padding
    rspq_profile_slot_dmem_t ovl[RSPQ_MAX_OVERLAYS];    ///< Per-overlay slots (0 = builtins)
    rspq_profile_slot_dmem_t cmn[RSPQ_PROFILE_CSLOT_COUNT]; ///< Common slots
    uint32_t frame_time;                                ///< Accumulated frame time (ticks)
    uint32_t busy_time;                                 ///< Accumulated RDP busy time (ticks)
    uint32_t frame_last;                                ///< DP_CLOCK at the last marker
    uint32_t busy_last;                                 ///< DP_BUSY at the last marker
    uint32_t padding2;                                  ///< Padding
    uint32_t cntr2;                                     ///< Sample counter (last word written)
    uint32_t padding3[2];                               ///< Padding (not written by the RSP)
} profile_buffer_t;

_Static_assert(sizeof(profile_buffer_t) % 16 == 0, "profile_buffer_t size mismatch");

static rspq_profile_data_t profile_data;
static uint32_t ovl_id;
static bool profile_started;

/** @brief Buffer written by the RSP on each frame marker. Only accessed via its uncached address. */
static profile_buffer_t cur_profile_buffer __attribute__((aligned(16)));
/** @brief Buffer written by the RSP on the marker enqueued by #rspq_profile_start. */
static profile_buffer_t base_profile_buffer __attribute__((aligned(16)));
/** @brief Last sample folded into #profile_data: the baseline for the next differences */
static profile_buffer_t last_profile_buffer;
/** @brief True once the sample of the start marker has been read */
static bool have_baseline;

/** @brief Copy a consistent snapshot of a buffer written by the RSP */
static void profile_buffer_read(const profile_buffer_t *buf, profile_buffer_t *out)
{
    volatile uint32_t *src = UncachedAddr(buf);
    uint32_t *dst = (uint32_t*)out;
    const int nwords = sizeof(profile_buffer_t) / sizeof(uint32_t);
    uint32_t before, after;

    do {
        before = src[offsetof(profile_buffer_t, cntr2) / 4];
        for (int i = 0; i < nwords; i++)
            dst[i] = src[i];
        after = src[offsetof(profile_buffer_t, cntr1) / 4];
    } while (before != after);
}

/** @brief Clear a buffer written by the RSP */
static void profile_buffer_clear(profile_buffer_t *buf)
{
    // Make sure no stale line (eg: from the BSS clearing) is left in the
    // data cache, as it could be written back over the RSP data later.
    data_cache_hit_writeback_invalidate(buf, sizeof(*buf));
    volatile uint32_t *dst = UncachedAddr(buf);
    for (int i = 0; i < sizeof(profile_buffer_t) / sizeof(uint32_t); i++)
        dst[i] = 0;
}

/** @brief Fold the samples recorded since the last call into the profile data. */
static void rspq_profile_poll(void)
{
    profile_buffer_t buf;

    // The start marker is the baseline: nothing can be accumulated before
    // it has been processed. Since it writes its own buffer, it does not
    // matter how many frame markers were processed in the meantime.
    if (!have_baseline) {
        profile_buffer_read(&base_profile_buffer, &buf);
        if (buf.cntr1 == 0)
            return;
        last_profile_buffer = buf;
        have_baseline = true;
    }

    profile_buffer_read(&cur_profile_buffer, &buf);
    if (buf.cntr1 == 0 || buf.cntr1 == last_profile_buffer.cntr1)
        return;

    for (int i = 0; i < RSPQ_MAX_OVERLAYS; i++) {
        if (profile_data.slots[i].name == NULL) continue;
        profile_data.slots[i].total_ticks += buf.ovl[i].total_ticks - last_profile_buffer.ovl[i].total_ticks;
        profile_data.slots[i].sample_count += buf.ovl[i].sample_count - last_profile_buffer.ovl[i].sample_count;
    }

    int cbase = RSPQ_MAX_OVERLAYS;
    for (int i = 0; i < RSPQ_PROFILE_CSLOT_COUNT; i++) {
        profile_data.slots[cbase+i].total_ticks += buf.cmn[i].total_ticks - last_profile_buffer.cmn[i].total_ticks;
        profile_data.slots[cbase+i].sample_count += buf.cmn[i].sample_count - last_profile_buffer.cmn[i].sample_count;
    }

    profile_data.total_ticks += buf.frame_time - last_profile_buffer.frame_time;
    profile_data.rdp_busy_ticks += buf.busy_time - last_profile_buffer.busy_time;
    // Several markers might have been processed since the last poll: the
    // totals above cover all of them, so count them all.
    profile_data.frame_count += buf.cntr1 - last_profile_buffer.cntr1;

    last_profile_buffer = buf;
}

void rspq_profile_reset(void)
{
    memset(&profile_data, 0, sizeof(profile_data));

    profile_data.slots[0].name = "Builtin cmds";
    for (int i = 1; i < RSPQ_MAX_OVERLAYS; i++)
    {
        if (!rspq_overlay_ucodes[i]) continue;
        if (i == ovl_id >> RSPQ_OVERLAY_ID_SHIFT) continue;
        profile_data.slots[i].name = rspq_overlay_ucodes[i]->name;
    }

    profile_data.slots[RSPQ_PROFILE_CSLOT_WAIT_CPU].name = "Wait CPU";
    profile_data.slots[RSPQ_PROFILE_CSLOT_WAIT_RDP].name = "Wait RDP";
    profile_data.slots[RSPQ_PROFILE_CSLOT_WAIT_RDP_SYNCFULL].name = "Wait SYNC_FULL";
    profile_data.slots[RSPQ_PROFILE_CSLOT_WAIT_RDP_SYNCFULL_MULTI].name = "Wait SYNC_FULLx2";
    profile_data.slots[RSPQ_PROFILE_CSLOT_OVL_SWITCH].name = "Ovl Switch";

    // Discard the samples recorded so far: the latest one becomes the baseline
    if (have_baseline) {
        profile_buffer_t buf;
        profile_buffer_read(&cur_profile_buffer, &buf);
        if (buf.cntr1 != 0)
            last_profile_buffer = buf;
    }
}

void rspq_profile_start(void)
{
    if (profile_started)
        return;

    profile_buffer_clear(&cur_profile_buffer);
    profile_buffer_clear(&base_profile_buffer);
    memset(&last_profile_buffer, 0, sizeof(last_profile_buffer));
    have_baseline = false;

    ovl_id = rspq_overlay_register(&rsp_profile);
    profile_started = true;
    rspq_profile_reset();

    // Enqueue the marker that records the baseline
    rspq_write(ovl_id, CMD_PROFILE_FRAME, PhysicalAddr(&base_profile_buffer));
}

void rspq_profile_stop(void)
{
    if (!profile_started)
        return;

    rspq_overlay_unregister(ovl_id);
    profile_started = false;
}

void rspq_profile_next_frame(void)
{
    if (!profile_started)
        return;

    rspq_profile_poll();
    rspq_write(ovl_id, CMD_PROFILE_FRAME, PhysicalAddr(&cur_profile_buffer));
}

#define RCP_TICKS_TO_USECS(ticks) (((ticks) * 1000000ULL) / RCP_FREQUENCY)
#define PERCENT(fraction, total) ((total) > 0 ? (float)(fraction) * 100.0f / (float)(total) : 0.0f)

static void rspq_profile_dump_slot(rspq_profile_slot_t *slot, uint64_t frame_avg)
{
    uint64_t mean = slot->total_ticks / profile_data.frame_count;
    uint64_t mean_us = RCP_TICKS_TO_USECS(mean);
    float relative = PERCENT(mean, frame_avg);

    char buf[64];
    sprintf(buf, "%3.2f%%", relative);

    debugf("%-25s %10llu %10lluus %10s\n",
        slot->name,
        slot->sample_count / profile_data.frame_count,
        mean_us,
        buf);
}

void rspq_profile_dump(void)
{
    if (profile_started)
        rspq_profile_poll();

    if (profile_data.frame_count == 0) {
        debugf("rspq_profile: no frames recorded\n");
        return;
    }

    uint64_t frame_avg = profile_data.total_ticks / profile_data.frame_count;
    uint64_t frame_avg_us = RCP_TICKS_TO_USECS(frame_avg);

    uint64_t counted_time = 0;
    for (size_t i = 0; i < RSPQ_PROFILE_SLOT_COUNT; i++) counted_time += profile_data.slots[i].total_ticks;

    // The counted time could be slightly larger than the total time due to various measurement errors
    uint64_t overhead_time = profile_data.total_ticks > counted_time ? profile_data.total_ticks - counted_time : 0;
    uint64_t overhead_avg = overhead_time / profile_data.frame_count;
    uint64_t overhead_us = RCP_TICKS_TO_USECS(overhead_avg);

    float overhead_relative = PERCENT(overhead_avg, frame_avg);

    uint64_t rdp_busy_avg = profile_data.rdp_busy_ticks / profile_data.frame_count;
    uint64_t rdp_busy_us = RCP_TICKS_TO_USECS(rdp_busy_avg);
    float rdp_utilisation = PERCENT(rdp_busy_avg, frame_avg);

    debugf("%-25s %10s %12s %10s\n", "Slot", "Cnt/Frame", "Avg/Frame", "Rel/Frame");
    debugf("------------------------------------------------------------\n");

    for (int i = 0; i < RSPQ_PROFILE_SLOT_COUNT; i++) {
        if (!profile_data.slots[i].name) continue;
        rspq_profile_dump_slot(&profile_data.slots[i], frame_avg);
    }

    debugf("------------------------------------------------------------\n");
    debugf("Profiled frames:    %12llu\n", profile_data.frame_count);
    debugf("Frames per second:  %12.1f\n", (float)RCP_FREQUENCY/(float)frame_avg);
    debugf("Average frame time: %10lluus\n", frame_avg_us);
    debugf("RDP busy time:      %10lluus (%2.2f%%)\n", rdp_busy_us, rdp_utilisation);
    debugf("Unrecorded time:    %10lluus (%2.2f%%)\n", overhead_us, overhead_relative);
    debugf("\n");
}

void rspq_profile_get_data(rspq_profile_data_t *data)
{
    if (profile_started)
        rspq_profile_poll();
    memcpy(data, &profile_data, sizeof(profile_data));
}

#else

void rspq_profile_start(void) { }
void rspq_profile_stop(void) { }
void rspq_profile_reset(void) { }
void rspq_profile_next_frame(void) { }
void rspq_profile_dump(void) { }
void rspq_profile_get_data(rspq_profile_data_t *data) { memset(data, 0, sizeof(*data)); }

#endif
