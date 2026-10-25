/**
 * @file rspq_profile.h
 * @author Dennis Heinze <dennisjp.heinze@gmail.com>
 * @author Giovanni Bajo <giovannibajo@gmail.com>
 * @brief RSP Command queue: profiler
 * @ingroup rsp
 *
 * The rspq profiler measures how the RSP spends its time while it runs the
 * command queue: in each overlay, in the builtin commands, waiting for the
 * CPU (empty queue), waiting for the RDP, and switching overlays. It also
 * measures how long the RDP was busy (via DP_BUSY), so that the RDP
 * utilisation of a frame can be computed.
 *
 * The profiler is a compile-time option: build libdragon *and* the ROM with
 * RSPQ_PROFILE=1, for instance `make RSPQ_PROFILE=1 install` in libdragon
 * and then `make clean && make RSPQ_PROFILE=1` in the project (n64.mk
 * forwards the define to the C compiler and the RSP assembler). RSPQ_PROFILE
 * changes the DMEM layout shared by all RSP overlays, so a ROM built with a
 * different value than libdragon fails at overlay registration with "Common
 * data of overlay ... does not match!". When RSPQ_PROFILE is 0 (the default)
 * the functions below are no-ops and the RSP code is unchanged.
 *
 * Usage:
 *
 * @code{.c}
 *      rspq_profile_start();
 *      for (int frame = 0; ; frame++) {
 *          // ... build and run a frame ...
 *          rspq_profile_next_frame();
 *          if (frame % 60 == 59)
 *              rspq_profile_dump();
 *      }
 * @endcode
 *
 * #rspq_profile_next_frame enqueues a RSP command that marks the end of a
 * frame; #rspq_profile_start enqueues the marker that starts the first one.
 * The samples recorded by the RSP are folded into the profile data the next
 * time one of the rspq_profile functions is called, so N markers produce N
 * profiled frames once the RSP has processed them (for instance after
 * #rspq_wait).
 */
#ifndef __LIBDRAGON_RSPQ_PROFILE_H
#define __LIBDRAGON_RSPQ_PROFILE_H

#include <stdint.h>
#include "rspq_constants.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Profiling data of a single slot (for example an overlay) */
typedef struct rspq_profile_slot_s {
    uint64_t total_ticks;   ///< The total number of RCP ticks that were spent running in this slot
    uint64_t sample_count;  ///< The number of individual samples that were recorded
    const char *name;       ///< The name of this slot, if it is used; NULL otherwise
} rspq_profile_slot_t;

/** @brief RSPQ Profiling data */
typedef struct rspq_profile_data_s {
    rspq_profile_slot_t slots[RSPQ_PROFILE_SLOT_COUNT];     ///< The list of slots
    uint64_t total_ticks;                                   ///< The total elapsed RCP ticks since the last reset
    uint64_t rdp_busy_ticks;                                ///< The accumulated ticks sampled from DP_BUSY
    uint64_t frame_count;                                   ///< The number of recorded frames since the last reset
} rspq_profile_data_t;

/**
 * @brief Start the rspq profiler
 *
 * This registers the profiler overlay, resets the profile data and enqueues
 * the marker that starts the first frame. It must be called after #rspq_init
 * and after the overlays that should be profiled have been registered
 * (overlays registered later are not named in the dump).
 */
void rspq_profile_start(void);

/**
 * @brief Stop the rspq profiler
 *
 * This unregisters the profiler overlay. Call it only when no frame marker
 * is pending, for instance after #rspq_wait.
 */
void rspq_profile_stop(void);

/** @brief Reset the rspq profiler and discard any recorded samples */
void rspq_profile_reset(void);

/** @brief Mark the end of the current frame to the rspq profiler */
void rspq_profile_next_frame(void);

/** @brief Dump the recorded data to the debug log */
void rspq_profile_dump(void);

/** @brief Copy the recorded data */
void rspq_profile_get_data(rspq_profile_data_t *data);

#ifdef __cplusplus
}
#endif

#endif
