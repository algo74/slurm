/*****************************************************************************\
 *  backfill.h - part of "Slurm-LDMS" project
 *****************************************************************************
 *  Copyright (C) 2003-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2010 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *  Modifed by Alexander Goponenko
 *  Copyright (C) 2020 Alexander Goponenko, University of Central Florida.
*
 *  Distributed with no warranty under the GNU General Public License.
 *  See the GNU General Public License for more details.
\*****************************************************************************/


#ifndef _SLURM_BACKFILL_H
#define _SLURM_BACKFILL_H

/* backfill_agent - detached thread periodically attempts to backfill jobs */
extern void *backfill_agent(void *args);

/* Terminate backfill_agent */
extern void stop_backfill_agent(void);

/* Note that slurm.conf has changed */
extern void backfill_reconfig(void);

/**
 * NOTE: Node leeway is the option to allow backfill reserve only the number of nodes
 * but not the exact set of nodes. This is useful to reduce fragmentation.
*/
void backfill_config_allow_node_leeway(bool value);

#endif	/* _SLURM_BACKFILL_H */
