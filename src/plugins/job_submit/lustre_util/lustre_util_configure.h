/*****************************************************************************\
 *  lustre_util_configure.h - part of "Slurm-LDMS" project 
 *****************************************************************************
 *  Copyright (C) 2020 Alexander Goponenko, University of Central Florida.
 *
 *  Distributed with no warranty under the GNU General Public License.
 *  See the GNU General Public License for more details.
\*****************************************************************************/
/**
 * TODO
*/
int lustre_util_configure();

/**
 * TODO
 * caller get the ownership of the name and port stings
 */
void get_server_address(const char **name, const char **port);

/**
 * TODO
 * caller get the ownership of the name and port stings
*/
void update_and_get_server_address(const char **name, const char **port);