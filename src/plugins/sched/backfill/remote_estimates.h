/*****************************************************************************\
 *  remote_estimates.h - part of "Slurm-LDMS" project 
 *****************************************************************************
 *  Copyright (C) 2020 Alexander Goponenko, University of Central Florida.
 *
 *  Distributed with no warranty under the GNU General Public License.
 *  See the GNU General Public License for more details.
\*****************************************************************************/

#ifndef SRC_PLUGINS_SCHED_REMOTE_ESTIMATES_H_
#define SRC_PLUGINS_SCHED_REMOTE_ESTIMATES_H_

#include "ctype.h"

#include "slurm/slurm.h"
#include "src/slurmctld/slurmctld.h"
#include "src/common/xmalloc.h"

typedef struct remote_estimates_s
{
  int timelimit;
  int lustre;
} remote_estimates_t;


/**
 * Sets the estimate structure all zeroes.
*/
static inline void reset_remote_estimates(remote_estimates_t *estimates) {
  estimates->lustre = 0;
  estimates->timelimit = 0;
}


// inline remote_estimates_t *new_remote_estmates()
// {
//   remote_estimates_t *res = xmalloc(sizeof(remote_estimates_t));
//   reset_remote_estimates(res);
//   return res;
// }

/**
 * Initializes server name and port configuration.
 * Note that already established connections are not refreshed.
 */
void config_vinsnl_server(char *server, char *port);

/*
  * Returns 0 if all good 1 if not timelimit, 2 if no lustre, and 3 if none.
  * Updates "results" with the obtained utilization estimates.
  * Caller keeps the ownership of all arguments.
  */
int get_job_utilization_from_remote(job_record_t *job_ptr, remote_estimates_t *results);

/**
 * Clears the cache of the remote estimates, etc.
 *
 */
void clear_remote_estimate_cache();

/**
 * Resets the inner state.
 * This function may be only needed for testing.
 */
void reset_connection();

#endif // SRC_PLUGINS_SCHED_REMOTE_ESTIMATES_H_