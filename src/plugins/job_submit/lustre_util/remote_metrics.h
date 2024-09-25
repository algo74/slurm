/*****************************************************************************\
 *  remote_metrics.h - part of "Slurm-LDMS" project 
 *****************************************************************************
 *  Copyright (C) 2020 Alexander Goponenko, University of Central Florida.
 *
 *  Distributed with no warranty under the GNU General Public License.
 *  See the GNU General Public License for more details.
\*****************************************************************************/

#ifndef _SLURM_LUSTRE_UTIL_REMOTE_METRICS_H
#define _SLURM_LUSTRE_UTIL_REMOTE_METRICS_H

typedef struct remote_metric_agent_arg_struct {
  char *addr;
  char *port;
} remote_metric_agent_arg_t;

/* backfill_agent - detached thread periodically attempts to read remote metrics */
extern void * remote_metrics_agent(void *args);

/* Terminate backfill_agent */
extern void stop_remote_metrics_agent(void);

#endif  /* _SLURM_LUSTRE_UTIL_REMOTE_METRICS_H */
