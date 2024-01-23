/*****************************************************************************\
 *  topology_3d_torus.c - Support for 3-Dimension torus interconnect
 *	topology, default for Cray XT systems
 *****************************************************************************
 *  Copyright (C) 2009 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "config.h"

#include <signal.h>
#include <sys/types.h>

#include "slurm/slurm_errno.h"
#include "src/common/bitstring.h"
#include "src/common/log.h"
#include "src/common/node_conf.h"
#include "src/common/xstring.h"

#include "../common/common_topo.h"

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  Slurm uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *      <application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "task" for task control) and <method> is a description
 * of how this plugin satisfies that application.  Slurm will only load
 * a task plugin if the plugin_type string has a prefix of "task/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */
const char plugin_name[]        = "topology 3d_torus plugin";
const char plugin_type[]        = "topology/3d_torus";
const uint32_t plugin_id = TOPOLOGY_PLUGIN_3DTORUS;
const uint32_t plugin_version   = SLURM_VERSION_NUMBER;

extern void nodes_to_hilbert_curve(void);

/*
 * init() is called when the plugin is loaded, before any other functions
 *	are called.  Put global initialization here.
 */
extern int init(void)
{
	verbose("%s loaded", plugin_name);
	return SLURM_SUCCESS;
}

/*
 * fini() is called when the plugin is removed. Clear any allocated
 *	storage here.
 */
extern int fini(void)
{
	return SLURM_SUCCESS;
}

/*
 * topo_build_config - build or rebuild system topology information
 *	after a system startup or reconfiguration.
 */
extern int topology_p_build_config(void)
{
	return SLURM_SUCCESS;
}

extern int topology_p_eval_nodes(topology_eval_t *topo_eval)
{
	return common_topo_choose_nodes(topo_eval);
}

/*
 * topo_generate_node_ranking  -  populate node_rank fields
 */
extern bool topology_p_generate_node_ranking(void)
{
	nodes_to_hilbert_curve();
	return true;
}

/*
 * topo_get_node_addr - build node address and the associated pattern
 *      based on the topology information
 *
 * in 3d_torus plugin, only use node name as the topology address
 */
extern int topology_p_get_node_addr(char *node_name, char **paddr,
				    char **ppattern)
{
	return common_topo_get_node_addr(node_name, paddr, ppattern);
}

extern int topology_p_split_hostlist(hostlist_t *hl, hostlist_t ***sp_hl,
				     int *count, uint16_t tree_width)
{
	return common_topo_split_hostlist_treewidth(
		hl, sp_hl, count, tree_width);
}

extern int topology_p_topology_free(void *topoinfo_ptr)
{
	return SLURM_SUCCESS;
}

extern int topology_p_topology_get(void **topoinfo_pptr)
{
	return SLURM_SUCCESS;
}

extern int topology_p_topology_pack(void *topoinfo_ptr, buf_t *buffer,
				    uint16_t protocol_version)
{
	return SLURM_SUCCESS;
}

extern int topology_p_topology_print(void *topoinfo_ptr, char *nodes_list,
				     char **out)
{
	*out = NULL;
	return SLURM_SUCCESS;
}

extern int topology_p_topology_unpack(void **topoinfo_pptr, buf_t *buffer,
				      uint16_t protocol_version)
{
	return SLURM_SUCCESS;
}
