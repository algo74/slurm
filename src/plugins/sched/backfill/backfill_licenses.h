/*****************************************************************************\
 *  backfill_licenses.h - part of "Slurm-LDMS" project 
 *****************************************************************************
 *  Copyright (C) 2020 Alexander Goponenko, University of Central Florida.
 *
 *  Distributed with no warranty under the GNU General Public License.
 *  See the GNU General Public License for more details.
\*****************************************************************************/

#ifndef SRC_PLUGINS_SCHED_BACKFILL_BACKFILL_LICENSES_H_
#define SRC_PLUGINS_SCHED_BACKFILL_BACKFILL_LICENSES_H_

#include "src/slurmctld/slurmctld.h"

#include "usage_tracker.h"
#include "remote_estimates.h"

typedef enum {
  // NOTE: original backfill is not iplemented directly; it can be emulated by using "zero" predictions
  BACKFILL_LICENSES_AWARE, 
  BACKFILL_LICENSES_TWO_GROUP // workload-adaptive implementation uses this
} backfill_licenses_config_t;

typedef struct lic_tracker_struct {
  List other_licenses;  // regular license tracking entries
  struct {
    backfill_licenses_config_t type;
    void *vp_entry;  // any type of tracking entry
  } lustre; 
  void *node_entry; // to track overall nodes
  int resolution;
  int lustre_offset;
} lic_tracker_t;

typedef lic_tracker_t *lic_tracker_p;

/**
 * Sets the internal backfill licenses configuration to the provided value and returns the previous value of the parameter.
 * See backfill_licenses_config_t for possible values.
*/
backfill_licenses_config_t configure_backfill_licenses(
    backfill_licenses_config_t config);

int configure_total_node_count(int config);

/**
 * Sets the internal filename to the provided value and returns the previous value of the parameter.
 * NOTE: the caller is responsible fo destrying the returned string.
 * The function gets the ownership of the provided string until it is reset.
*/
char *configure_lustre_log_filename(char *filename);

/**
 * Sets the "trace nodes" flag to the provided value and returns the previous value of the parameter.
 * NOTE: "trace nodes" option enables the tracking of nodes same way as licenses.
 *       It is needed if "node leeway" is used in the main backfill algorithm.
*/
bool configure_trace_nodes(int config);

/**
 * Creates and initializes a new license tracker.
 * IN resolution - the time resolution of the tracker in seconds
 * RET: the new tracker, owned by the caller (must be destroyed with destroy_lic_tracker)
*/
lic_tracker_p init_lic_tracker(int resolution);

void destroy_lic_tracker(lic_tracker_p);

void dump_lic_tracker(lic_tracker_p lt);

/**
 * Test when the licenses required for a job are available
 * IN job_ptr   - job identification
 * IN/OUT when  - time to check after/when available
 * RET: SLURM_SUCCESS, EAGAIN (not available now), SLURM_ERROR (never runnable)
 */
int backfill_licenses_test_job(lic_tracker_p lt, job_record_t *job_ptr, remote_estimates_t *estimates, time_t *when);

int backfill_licenses_overlap(lic_tracker_p lt, job_record_t *job_ptr, remote_estimates_t *estimates, time_t when);

/**
 * Preallocate the licenses required for a just scheduled job
 * IN job_ptr - job identification
 * RET SLURM_SUCCESS or failure code
 */
int backfill_licenses_alloc_job(lic_tracker_p lt, job_record_t *job_ptr, remote_estimates_t *estimates,
                                time_t start, time_t end);

#endif /* SRC_PLUGINS_SCHED_BACKFILL_BACKFILL_LICENSES_H_ */
