/*****************************************************************************\
*  backfill_configure.h - part of "Slurm-LDMS" project 
*****************************************************************************
*  Copyright (C) 2024 Alexander Goponenko, University of Central Florida.
*  Distributed with no warranty under the GNU General Public License.
*  See the GNU General Public License for more details.
\*****************************************************************************/
/**
 * This file contains declarations for functions that configure the backfill plugin.
 * 
 * Configuration parameters are read from a JSON file.
 * Configuration file format:
 * {
 *   "server": {   # if null, the server is disabled
 *     "name": "<server_name>",
 *     "port": <server_port>|"<server_port>"
 *   },
 *   "backfill_type": "AWARE"=null|"TWO_GROUP", # default is null (AWARE)
 *   "two_group_fraction": {0.0..1.0}, # only used if backfill_type is "TWO_GROUP", default is 0.5
 *   "track_nodes": true|false, # default is false (CHECK); determines if new node tracking is enabled 
 *   "lustre_log_path": "filename",
 * }
 * 
 */

void backfill_configure();