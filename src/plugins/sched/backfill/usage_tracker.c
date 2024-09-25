/*****************************************************************************\
 *  usage_tracker.c - part of "Slurm-LDMS" project 
 *****************************************************************************
 *  Copyright (C) 2020 Alexander Goponenko, University of Central Florida.
 *
 *  Distributed with no warranty under the GNU General Public License.
 *  See the GNU General Public License for more details.
\*****************************************************************************/

#include <time.h>
#include <stdio.h>

#include "src/common/list.h"
#include "src/common/xmalloc.h"
#include "src/common/xassert.h"
#include "usage_tracker.h"

#include "src/common/log.h"
#define log(...) debug3(__VA_ARGS__)


typedef struct ut_int_struct {
  time_t start;
  int value;
} ut_int_item_t;


static void
_delete_item(void *x) {
  ut_int_item_t* item = (ut_int_item_t*) x;
  xfree(item);
}


static ut_int_item_t*
_create_item(time_t start, int value) {
  ut_int_item_t *item = xmalloc(sizeof(ut_int_item_t));
  item->start = start;
  item->value = value;
  return item;
}


void
ut_int_add_usage(utracker_int_t ut,
               time_t start, time_t end,
               int usage) {
  xassert(start>0);
  xassert(end>start);
  if (usage == 0)
    // do nothing
    return;

  utiterator_t it = list_iterator_create(ut);
  ut_int_item_t *prev;
  ut_int_item_t *next = list_next(it);
  do {
    prev = next;
    next = list_next(it);
  } while(next && next->start < start);
  int prev_value = prev->value;
  int old_value = prev_value;
  if (!next || next->start > start) {
    // add new item to "split the interval"
    prev_value += usage;
    ut_int_item_t *new_item = _create_item(start, prev_value);
    list_insert(it, new_item);
  }
  if (next && next->value + usage == prev_value) {
    // delete next for optimization
    old_value = next->value;
    ut_int_item_t *deleted_item = list_remove(it);
    xassert(deleted_item == next);
    _delete_item(deleted_item);
    next = list_next(it);
  }
  while(next && next->start < end) {
    old_value = next->value;
    prev_value = (next->value += usage);
    next = list_next(it);
  }
  if (!next || next->start > end) {
    // add new item to "split the interval"
    ut_int_item_t *new_item = _create_item(end, old_value);
    list_insert(it, new_item);
  } else if (next->value == prev_value) {
    xassert(next->start == end);
    // delete next for optimization
    ut_int_item_t *deleted_item = list_remove(it);
    xassert(deleted_item == next);
    _delete_item(deleted_item);
    next = list_next(it);
  }
}


void
ut_int_remove_till_end(utracker_int_t ut,
                      time_t start, int usage) {
  xassert(start>0);
  if (usage == 0)
    // do nothing
    return;

  utiterator_t it = list_iterator_create(ut);
  ut_int_item_t *prev;
  ut_int_item_t *next = list_next(it);
  do {
    prev = next;
    next = list_next(it);
  } while(next && next->start < start);
  int prev_value = prev->value;
  if (!next || next->start > start) {
    // add new item to "split the interval"
    prev_value -= usage;
    ut_int_item_t *new_item = _create_item(start, prev_value);
    list_insert(it, new_item);
  }
  if (!next) {
    return;
  }
  if (next->value - usage == prev_value) {
    // delete next for optimization
    ut_int_item_t *deleted_item = list_remove(it);
    xassert(deleted_item == next);
    _delete_item(deleted_item);
    next = list_next(it);
  }
  while(next) {
    next->value -= usage;
    next = list_next(it);
  }
}



void ut_int_add(utracker_int_t ut, int value)
{
  if (value == 0)
    // do nothing
    return;

  utiterator_t it = list_iterator_create(ut);
  ut_int_item_t *next = list_next(it);
  while (next)
  {
    next->value += value;
    next = list_next(it);
  }
}


time_t
ut_int_when_below(utracker_int_t ut,
                   time_t after, time_t duration,
                   int max_value){
  xassert(after>0);
  xassert(duration>0);
  utiterator_t it = list_iterator_create(ut);
  ut_int_item_t *prev;
  ut_int_item_t *next = list_next(it);
  do {
    prev = next;
    next = list_next(it);
  } while(next && next->start < after);
  while(1) {
    while(prev->value >= max_value) {
      if (!next) {
        return(-1);
      }
      prev = next;
      next = list_next(it);
    }
    time_t start = prev->start > after ? prev->start : after;
    time_t end = start + duration;
    while(prev->value < max_value) {
      if (!next || next->start >= end) {
        return start;
      }
      prev = next;
      next = list_next(it);
    }
  }
}


utracker_int_t
ut_int_create(int start_value){
  List list = list_create(_delete_item);
  list_append(list, _create_item((time_t)-1, start_value));
  return list;
}


void
ut_int_destroy(utracker_int_t ut) {
  list_destroy(ut);
}


static void
ut_int_dump_item(ut_int_item_t *item) {
  char buff[32];
  log("%24.24s (%lld): %d", ctime_r(&(item->start), buff),
      (long long)item->start, item->value);
}


static int
_dump_item(void *item, void *arg) {
  ut_int_dump_item((ut_int_item_t *)item);
  return 1;
}


void
ut_int_dump(utracker_int_t ut) {
  log("--------------------------------");
  list_for_each(ut, _dump_item, NULL);
  log("--------------------------------");
}

int ut_get_initial_value(utracker_int_t ut) 
{
  ut_int_item_t *head = list_peek(ut);
  return head->value;
}
