/*****************************************************************************\
 *  usage_tracker.h - part of "Slurm-LDMS" project 
 *****************************************************************************
 *  Copyright (C) 2020 Alexander Goponenko, University of Central Florida.
 *
 *  Distributed with no warranty under the GNU General Public License.
 *  See the GNU General Public License for more details.
\*****************************************************************************/

#ifndef USAGE_TRACKER_H_
#define USAGE_TRACKER_H_

#include <time.h>

#include "src/common/list.h"

typedef List utracker_int_t;

typedef ListIterator utiterator_t;

void ut_int_add_usage(utracker_int_t ut,
               time_t start, time_t end,
               int value);

/**
 * returns the time of the beginning of first interval
 * not earlier than time "after" of duration "duration" in tracker "ut"
 * during which the tracked value is below "max_value"
 * or -1 if no such interval
 */
time_t ut_int_when_below(utracker_int_t ut,
                   time_t after, time_t duration,
                   int max_value);

void ut_int_remove_till_end(utracker_int_t ut,
                      time_t start, int usage);

utracker_int_t ut_int_create(int start_value);

void ut_int_destroy(utracker_int_t ut);

void ut_int_dump(utracker_int_t ut);

/**
 * adds the value to the whole step function
 */
void ut_int_add(utracker_int_t ut, int value);

/**
 * gets initial value of the step function
*/
int ut_get_initial_value(utracker_int_t ut);

#endif /* USAGE_TRACKER_H_ */
