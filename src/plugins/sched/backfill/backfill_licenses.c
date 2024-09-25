/*****************************************************************************\
*  backfill_licenses.c - part of "Slurm-LDMS" project 
*****************************************************************************
*  Copyright (C) 2020 Alexander Goponenko, University of Central Florida.
*  Distributed with no warranty under the GNU General Public License.
*  See the GNU General Public License for more details.
\*****************************************************************************/

#include "backfill_licenses.h"

#include "src/common/xstring.h"
#include "src/slurmctld/licenses.h"
#include "src/slurmctld/node_scheduler.h"

// #include "remote_estimates.h"

#define NOT_IMPLEMENTED 9999

#define LUSTRE "lustre"

extern pthread_mutex_t license_mutex; /* from "src/slurmctld/licenses.c" */

/**
 * Entry for regular licence tracking
 */
typedef struct lt_entry_struct {
  char *name;
  uint32_t total;
  utracker_int_t ut;
} lt_entry_t;

/**
 * Entry for two-group adaptive licence tracking
 */
typedef struct two_group_entry_struct {
  char *name;
  uint32_t total;
  utracker_int_t ut;
  utracker_int_t st;
  double r_star;
  double r_bar;
  double r_target;  // target rate per node
  int r_star_target; // recalculated target value per cluster
  uint32_t n_total;
  double random; // [0,1] determine how much over the target is allowed at the current round
  // TODO
} two_group_entry_t;

/**
 * Entry for temporary array of job details to calculate "star" values
*/
typedef struct star_job_detail_struct {
  job_record_t *job;
  // int n_nodes;
  double duration_in_min;
  // int lustre;
  double area;
  double lustre_per_node;
  double lustre_volume;
} star_job_details_t;

static void _lt_entry_destroy(void *x) {
  lt_entry_t *entry = (lt_entry_t *)x;
  if (entry) {
    xfree(entry->name);
    ut_int_destroy(entry->ut);
    xfree(entry);
  }
}

static void _two_group_entry_destroy(void *x) {
  two_group_entry_t *entry = (two_group_entry_t *)x;
  if (entry) {
    xfree(entry->name);
    ut_int_destroy(entry->ut);
    if (entry->st) ut_int_destroy(entry->st);
    xfree(entry);
  }
}

static backfill_licenses_config_t config_state = BACKFILL_LICENSES_AWARE;
static int config_total_node_count = -1;
static char *config_lustre_log_filename = NULL;
static bool config_trace_nodes = false;

backfill_licenses_config_t configure_backfill_licenses(
    backfill_licenses_config_t config) {
  backfill_licenses_config_t old_config = config_state;
  config_state = config;
  return old_config;
}

int configure_total_node_count(int config) {
  int old_config = config_total_node_count;
  config_total_node_count = config; 
  return old_config;
}

bool configure_trace_nodes(int config) {
  bool old_config = config_trace_nodes;
  config_trace_nodes = config; 
  return old_config;
}

char *configure_lustre_log_filename(char *filename)
{
  char *old_config = config_lustre_log_filename;
  config_lustre_log_filename = filename;
  return old_config;
}

static time_t _convert_time_floor(time_t t, int resolution) {
  return (t / resolution) * resolution;
}

static time_t _convert_time_fwd(time_t d, int resolution) {
  return (d / resolution + 1) * resolution;
}

/**
 * function for qsort
*/
static int _compare_star_job_details (const void *a, const void *b) {
  double diff = (*(star_job_details_t **)a)->lustre_per_node - (*(star_job_details_t **)b)->lustre_per_node;
  return (diff > 0) - (diff < 0);
}

/* AG: copied from src/slurmctld/licenses.c */
/* Find a license_t record by license name (for use by list_find_first) */
static int _license_find_rec(void *x, void *key) {
  licenses_t *license_entry = (licenses_t *)x;
  char *name = (char *)key;

  if ((license_entry->name == NULL) || (name == NULL)) return 0;
  if (xstrcmp(license_entry->name, name)) return 0;
  return 1;
}

/* AG: adapted from get_total_license_cnt from src/slurmctld/licenses.c */
static uint32_t _license_cnt(List licenses_l, char *name) {
  uint32_t count = -1;
  licenses_t *lic;
  if (licenses_l) {
    lic = list_find_first(licenses_l, _license_find_rec, name);
    if (lic) count = lic->total;
  }
  return count;
}

// uint32_t _get_job_lustre_count(job_record_t *job_ptr) {
//   uint32_t result = _license_cnt(job_ptr->license_list, LUSTRE);
//   if (result != -1) return result;
//   remote_estimates_t estimates;
//   reset_remote_estimates(&estimates);
//   get_job_utilization_from_remote(job_ptr, &estimates);
//   return estimates.lustre;
// }

uint32_t _get_job_node_count(job_record_t *job_ptr) {
  uint32_t min_nodes, req_nodes = 0, max_nodes;
  debug5("%s: %pJ: node count: %d", __func__, job_ptr, job_ptr->node_cnt);
  debug5("%s: %pJ: node count wag: %d", __func__, job_ptr,
         job_ptr->node_cnt_wag);
  if (job_ptr->node_cnt > 0) return job_ptr->node_cnt;
  if (job_ptr->node_cnt_wag > 0) return job_ptr->node_cnt_wag;
  if (job_ptr->details && job_ptr->details->max_nodes > 0) {
    return job_ptr->details->max_nodes;
    debug5("%s: %pJ: max nodes: %d", __func__, job_ptr,
           job_ptr->details->max_nodes);
  }
  // TODO: maybe the following is better than the previous
  part_record_t *part_ptr =
      job_ptr->part_ptr ? job_ptr->part_ptr : list_peek(job_ptr->part_ptr_list);
  uint32_t qos_flags = 0;  // TODO: figure out what it does and set it properly
  int error_code = get_node_cnts(job_ptr, qos_flags, part_ptr, &min_nodes,
                                 &req_nodes, &max_nodes);
  if (error_code != SLURM_SUCCESS) {
    error("%s: %pJ: get_node_cnts error: %d", __func__, job_ptr, error_code);
  }
  debug5("%s: %pJ: max/req/min nodes: %d/%d/%d", __func__, job_ptr, max_nodes,
         req_nodes, min_nodes);
  return req_nodes;
}

/* Find a lt_entry_t record by license name in license tracker (for use by
 * list_find_first) */
static int _lt_find_lic_name(void *x, void *key) {
  lt_entry_t *entry = (lt_entry_t *)x;
  char *name = (char *)key;

  if ((entry->name == NULL) || (name == NULL)) return 0;
  if (xstrcmp(entry->name, name)) return 0;
  return 1;
}

void dump_lic_tracker(lic_tracker_p lt) {
  ListIterator iter = list_iterator_create(lt->other_licenses);
  lt_entry_t *entry;
  debug3("dumping licenses tracker; resolution: %d", lt->resolution);
  while ((entry = list_next(iter))) {
    debug3("license: %s, total: %d", entry->name, entry->total);
    ut_int_dump(entry->ut);
  }
  if (lt->node_entry) {
    entry = lt->node_entry;
    debug3("nodes total: %d", entry->total);
    ut_int_dump(entry->ut);
  }
  list_iterator_destroy(iter);
  if (lt->lustre.type == BACKFILL_LICENSES_AWARE) {
    lt_entry_t *entry = lt->lustre.vp_entry;
    debug3("license: %s, total: %d", entry->name, entry->total);
    ut_int_dump(entry->ut);
  } else if (lt->lustre.type == BACKFILL_LICENSES_TWO_GROUP) {
    two_group_entry_t *entry = lt->lustre.vp_entry;
    debug3("Lustre limits, total: %d", entry->total);
    ut_int_dump(entry->ut);
    debug3("Lustre star, target: %d", entry->r_star_target);
    ut_int_dump(entry->st);
  } else {
    error("%s (%d): Not implemented license type", __func__, __LINE__);
  }
}

static void _lt_return_single_lic(lic_tracker_p lt, char *name, uint32_t value,
                                  job_record_t *job_ptr) {
  lt_entry_t *lt_entry =
      list_find_first(lt->other_licenses, _lt_find_lic_name, name);
  if (lt_entry) {
    time_t t = _convert_time_fwd(job_ptr->end_time, lt->resolution);
    ut_int_remove_till_end(lt_entry->ut, t, value);
  } else {
    error("%s: Job %pJ returned unknown license \"%s\"", __func__, job_ptr,
          name);
  }
}

static void _lt_return_lustre(lic_tracker_p lt, int lustre_value,
                              job_record_t *job_ptr) {
  if (!lt->lustre.vp_entry) {
    error("%s: Job %pJ returned unknown license \"%s\"", __func__, job_ptr,
          LUSTRE);
  }
  if (lt->lustre.type == BACKFILL_LICENSES_AWARE) {
    lt_entry_t *lt_entry = lt->lustre.vp_entry;
    time_t t = _convert_time_fwd(job_ptr->end_time, lt->resolution);
    ut_int_remove_till_end(lt_entry->ut, t, lustre_value);
  } else if (lt->lustre.type == BACKFILL_LICENSES_TWO_GROUP) {
    two_group_entry_t *lt_entry = lt->lustre.vp_entry;
    time_t t = _convert_time_fwd(job_ptr->end_time, lt->resolution);
    // return normal lustre license
    ut_int_remove_till_end(lt_entry->ut, t, lustre_value);
    // return star lustre
    int star_value = (int) (0.5 + (double)lustre_value - _get_job_node_count(job_ptr) * lt_entry->r_bar);
    ut_int_remove_till_end(lt_entry->st, t, star_value);
  } else
    error("%s (%d): Not implemented license type (%d)", __func__, __LINE__, lt->lustre.type);
}

/**
 *  "returns" licenses used by the job to the license tracker
 * IN job_ptr - job record
 * IN estimates - remote estimates for the job
 * IN/OUT lt - license tracker
 *             Modified: licenses will be returned
 *                       lustre_offset will be updated if estimates are used
 * NOTE: the function assumes that the job is running
 */
static void _lt_process_running_job(lic_tracker_p lt, job_record_t *job_ptr,
                                    remote_estimates_t *estimates) {
  /*AG TODO: implement reservations */

  // AG TODO: better way to merge estimates with user data
  int lustre_value = estimates->lustre;
  bool lustre_found = false;
  if (job_ptr->license_list) {
    ListIterator j_iter = list_iterator_create(job_ptr->license_list);
    licenses_t *license_entry;
    while ((license_entry = list_next(j_iter))) {
      if (xstrcmp(license_entry->name, LUSTRE) == 0) {
        lustre_value = license_entry->total;
        lustre_found = true;
      } else
        _lt_return_single_lic(lt, license_entry->name, license_entry->total,
                              job_ptr);
    }
    list_iterator_destroy(j_iter);
  }
  if (lustre_value > 0) {
    _lt_return_lustre(lt, lustre_value, job_ptr);
    if (!lustre_found) {
      lt->lustre_offset += lustre_value;
    }
  }
  debug3("%s: Job %pJ returned licenses", __func__, job_ptr);
  dump_lic_tracker(lt);
}

void destroy_lic_tracker(lic_tracker_p lt) {
  if (lt) {
    list_destroy(lt->other_licenses);
    if (lt->lustre.type == BACKFILL_LICENSES_AWARE) {
      _lt_entry_destroy(lt->lustre.vp_entry);
    } else if (lt->lustre.type == BACKFILL_LICENSES_TWO_GROUP) {
      _two_group_entry_destroy(lt->lustre.vp_entry);
    } else {
      error("%s (%d): Not implemented license type (%d)", __func__, __LINE__, lt->lustre.type);
    }
    if (lt->node_entry) {
      _lt_entry_destroy(lt->node_entry);
    }
    xfree(lt);
  }
}

uint32_t _get_total_nodes_count() {
  debug5("%s: avail_node_bitmap count: %d", __func__, bit_set_count(avail_node_bitmap));
  debug5("%s: idle_node_bitmap count: %d", __func__, bit_set_count(idle_node_bitmap));
  debug5("%s: rs_node_bitmap count: %d", __func__, bit_set_count(rs_node_bitmap));
  if (config_total_node_count > 0) {
    debug5("%s: using config_total_node_count: %d", __func__,
           config_total_node_count);
    return config_total_node_count;
  }
  bitstr_t *bitmap = bit_copy(avail_node_bitmap);
  /* Make "resuming" nodes available to be scheduled in backfill */
  bit_or(bitmap, rs_node_bitmap);
  return bit_set_count(bitmap);
}

/**
 * 
*/
void _setup_two_groups(two_group_entry_t *entry, lt_entry_t *lt_entry, time_t call_time) {
  entry->name = lt_entry->name;
  entry->total = lt_entry->total;
  entry->ut = lt_entry->ut;
  entry->st = NULL;
  entry->n_total = _get_total_nodes_count();
  // set defaults
  entry->r_bar = 0.0;
  entry->r_star = 0.0;
  entry->r_target = 0.0;
  entry->random = 0.5;
  entry->r_star_target = entry->total;
  size_t n_jobs = list_count(job_list);
  debug5("%s: number of jobs: %zu", __func__, n_jobs);
  if (n_jobs == 0) {
    return;
  }
  ListIterator job_iterator = list_iterator_create(job_list);
  job_record_t *tmp_job_ptr = list_next(job_iterator);
  if (!tmp_job_ptr) {
    list_iterator_destroy(job_iterator);
    return;
  }
  // Note: now we cannot just return from the function; we need to delete the objects we create
  star_job_details_t *star_job_details = xmalloc(sizeof(star_job_details_t) * n_jobs);
  debug5("%s: star_job_details: %p", __func__, star_job_details);
  star_job_details_t **sortable_star_details = xmalloc(sizeof(star_job_details_t*) * n_jobs);
  debug5("%s: sortable_star_details: %p", __func__, sortable_star_details);
  size_t n_pending_jobs = 0;
  // first pass through jobs - calculating target rate per node
  double total_area = 0;
  double total_volume = 0;
  double pending_area = 0;
  int used_nodes_count = 0;
  int used_lustre_count = 0;
  for ( ; tmp_job_ptr; tmp_job_ptr = list_next(job_iterator)) {
    if (!IS_JOB_RUNNING(tmp_job_ptr) && 
        // !IS_JOB_SUSPENDED(tmp_job_ptr) &&
        !IS_JOB_PENDING(tmp_job_ptr)) {  
      // Consider only running and pending jobs
      // TODO: figure out if suspended jobs are appropriate
      continue;
    }
    int nodes_count = _get_job_node_count(tmp_job_ptr);
    debug5("%s: %pJ: node count: %d", __func__, tmp_job_ptr, nodes_count);
    remote_estimates_t estimates;
    reset_remote_estimates(&estimates);
    get_job_utilization_from_remote(tmp_job_ptr, &estimates);
    int lustre_count = _license_cnt(tmp_job_ptr->license_list, LUSTRE);
    if (lustre_count == -1) lustre_count = estimates.lustre;
    debug5("%s: %pJ: lustre count: %d", __func__, tmp_job_ptr, lustre_count);
    double duration_in_min;
    if (IS_JOB_PENDING(tmp_job_ptr)) {
      duration_in_min = (estimates.timelimit > 0 && estimates.timelimit < tmp_job_ptr->time_limit) ? 
          estimates.timelimit : tmp_job_ptr->time_limit;
      // star_job_details[n_pending_jobs].duration_in_min = duration_in_min;
      if (n_pending_jobs < n_jobs) {  // need to check because new jobs may arrive and we do not have enough space for all
        double area = nodes_count * duration_in_min;
        pending_area += area;
        star_job_details[n_pending_jobs].area = area;
        star_job_details[n_pending_jobs].lustre_volume = lustre_count * duration_in_min;
        star_job_details[n_pending_jobs].job = tmp_job_ptr;
        star_job_details[n_pending_jobs].lustre_per_node = (double)lustre_count/ (double)nodes_count;
        sortable_star_details[n_pending_jobs] = star_job_details + n_pending_jobs;
        ++n_pending_jobs;
      } else {
        error("%s: number of pending jobs is more than anticipated (%zu), job %pJ dropped", __func__, n_jobs, tmp_job_ptr);
      }
    } else { // running job
      duration_in_min = (double)(tmp_job_ptr->end_time - call_time) / 60.0;
      // adjust duration if the job was estimated to run less than its time limit
      if (estimates.timelimit > 0 && estimates.timelimit < tmp_job_ptr->time_limit) {
        duration_in_min -= tmp_job_ptr->time_limit - estimates.timelimit;
      }
      used_lustre_count += lustre_count;
      used_nodes_count += nodes_count;
    }
    if (duration_in_min < 0) {
      debug5("%s: %pJ: finished %f minutes ago - skipping", __func__, tmp_job_ptr,
              -duration_in_min);
    } else {
      total_area += duration_in_min * nodes_count;
      total_volume += duration_in_min * lustre_count;
    }
  }
  list_iterator_destroy(job_iterator);
  entry->r_target = total_area > 0 ? total_volume / total_area : (double)entry->total / (double) entry->n_total;
  debug5("%s: r_target: %f", __func__, entry->r_target);
  // calculating r_star
  if (n_pending_jobs == 0) {
    // keep defaults
  } else {
    qsort(sortable_star_details, n_pending_jobs, sizeof(star_job_details_t *), _compare_star_job_details);
    debug5("%s: sorted star details", __func__);
    double target_area = 0.5 * pending_area;
    debug5("%s: target area: %f", __func__, target_area);
    int i = 0;
    double area = sortable_star_details[i]->area;
    debug5("%s: i: %d, area: %f", __func__, i, area);
    while (i < (n_pending_jobs-1) && area < target_area) {
      ++i;
      area += sortable_star_details[i]->area;
      debug5("%s: i: %d, area: %f", __func__, i, area);
    }
    entry->r_star = sortable_star_details[i]->lustre_per_node;
    // second pass through job - calculating r_bar
    total_area = 0;
    total_volume = 0;
    for (i = 0; i < n_pending_jobs && sortable_star_details[i]->lustre_per_node <= entry->r_star; i++) {
      total_area += sortable_star_details[i]->area;
      total_volume += sortable_star_details[i]->lustre_volume;
    }
    entry->r_bar = total_area > 0 ? total_volume / total_area : 0;
    // initializing "star" tracker
    entry->r_star_target = (int)(0.5 + (double)entry->n_total * (entry->r_target - entry->r_bar));
  }
  xfree(star_job_details);
  xfree(sortable_star_details);
  int star_start_value = used_lustre_count - (int)(0.5 + used_nodes_count * entry->r_bar);
  entry->st = ut_int_create(star_start_value);
}

lic_tracker_p init_lic_tracker(int resolution) {
  licenses_t *license_entry;
  ListIterator iter;
  lic_tracker_p res = NULL;
  job_record_t *tmp_job_ptr;

  time_t now = time(NULL);

  // clear_remote_estimate_cache();  // The cache is cleared before the scheduling round in _attempt_backfill

  /* create licenses tracker and set initial parameters (before processing running jobs) */
  slurm_mutex_lock(&license_mutex);
  if (license_list) {
    res = xmalloc(sizeof(lic_tracker_t));
    res->other_licenses = list_create(_lt_entry_destroy);
    res->resolution = resolution;
    res->lustre_offset = 0;
    res->lustre.type = config_state;
    res->lustre.vp_entry = NULL;
    res->node_entry = NULL;
    iter = list_iterator_create(license_list);
    while ((license_entry = list_next(iter))) {
      lt_entry_t *entry = xmalloc(sizeof(lt_entry_t));
      entry->name = xstrdup(license_entry->name);
      entry->total = license_entry->total;
      int start_value;
      debug3("%s: LIC: %s, Internal: %d, External: %d, Total: %d", __func__,
             entry->name, license_entry->used, license_entry->r_used, entry->total);
      if (license_entry->used < license_entry->r_used) {
        start_value = license_entry->r_used;
      } else {
        start_value = license_entry->used;
      }
      entry->ut = ut_int_create(start_value);
      if (xstrcmp(entry->name, LUSTRE) == 0) {
        // if we corrected the lustre "used" value, we reduce the offset (make it negative)
        // so that we do not overcorrect later the effect of the estimated jobs requrement
        res->lustre_offset = (int)license_entry->used - (int)start_value;
        // set up lustre entry  
        if (res->lustre.type == BACKFILL_LICENSES_TWO_GROUP) {
          two_group_entry_t *entry2 = xmalloc(sizeof(two_group_entry_t));
          _setup_two_groups(entry2, entry, now);
          xfree(entry);
          debug5("%s: r_star: %f, r_bar: %f, r_star_target: %d, limit: %d",
                 __func__, entry2->r_star, entry2->r_bar,
                 entry2->r_star_target, entry2->total);
          res->lustre.vp_entry = entry2;
        } else {
          res->lustre.vp_entry = entry;
        }
      } else {
        // for all licenses that are not "lustre"
        list_push(res->other_licenses, entry);
      }
    }
    list_iterator_destroy(iter);
  }
  slurm_mutex_unlock(&license_mutex);

  if (!res) return NULL; // if we have no license tracker by now, no reason to continue
  // FIXME: what about tracking nodes?


  /*AG TODO: implement reservations */

  /* AG: initializing node entry */
  lt_entry_t *node_entry = NULL; // node entry is null if we do not track nodes
  int used_node_count = 0;  // to calculate the count of nodes used by running jobs
  if (config_trace_nodes) {
    node_entry = xmalloc(sizeof(lt_entry_t)); 
    res->node_entry = node_entry;
    node_entry->total = _get_total_nodes_count();
    node_entry->ut = ut_int_create(0);  // we will update the initial value later
  }

  /* process running jobs */
  if (!job_list) {
    return res;
  }
  ListIterator job_iterator = list_iterator_create(job_list);
  while ((tmp_job_ptr = list_next(job_iterator))) {
    if (!IS_JOB_RUNNING(tmp_job_ptr) && !IS_JOB_SUSPENDED(tmp_job_ptr)) {
      continue;
    }
    time_t end_time = tmp_job_ptr->end_time;
    if (end_time == 0) {
      error("%s: Active %pJ has zero end_time", __func__, tmp_job_ptr);
      continue;
    }
    if (end_time < now) {
      debug3("%s: %pJ might have finished -- yet processing normally", __func__,
             tmp_job_ptr);
    }
    // get estimates
    remote_estimates_t estimates;
    reset_remote_estimates(&estimates);
    get_job_utilization_from_remote(tmp_job_ptr, &estimates);

    // process the node count
    if (node_entry) {
      int job_node_count = _get_job_node_count(tmp_job_ptr);
      time_t t = _convert_time_fwd(tmp_job_ptr->end_time, res->resolution);
      ut_int_remove_till_end(node_entry->ut, t, job_node_count);
      used_node_count += job_node_count;
    }

    // process licenses
    if (tmp_job_ptr->license_list == NULL && estimates.lustre == 0) {
      debug3("%s: %pJ has no licenses -- skipping", __func__, tmp_job_ptr);
      continue;
    } 
    // return licenses used by the job
    _lt_process_running_job(res, tmp_job_ptr, &estimates);
  }
  list_iterator_destroy(job_iterator);

  // correct lustre offest
  if (res->lustre_offset > 0) {
    if (res->lustre.type == BACKFILL_LICENSES_AWARE) {
      lt_entry_t *lt_entry = res->lustre.vp_entry;
      ut_int_add(lt_entry->ut, res->lustre_offset);
    } else if (res->lustre.type == BACKFILL_LICENSES_TWO_GROUP) {
      two_group_entry_t *lt_entry = res->lustre.vp_entry;
      ut_int_add(lt_entry->ut, res->lustre_offset);
    } else
      error("%s (%d): Not implemented license type", __func__, __LINE__);
  }
  // correct node offest
  if (node_entry) {
    ut_int_add(node_entry->ut, used_node_count);
  }
  // log the information
  if (config_lustre_log_filename) {
    FILE *log_file = fopen(config_lustre_log_filename, "a");
    if (!log_file) {
      error("%s (%d): cannot open file \"%s\" for logging", __func__, __LINE__, config_lustre_log_filename);
    } else {
      int total = 0;
      int used = 0;
      int star_used = 0;
      double r_star = 0;
      double r_bar = 0;
      double r_target = 0 ;  // target rate per node
      int r_star_target = 0;
      if (res->lustre.type == BACKFILL_LICENSES_AWARE) {
        lt_entry_t *lt_entry = res->lustre.vp_entry;
        total = lt_entry->total;
        used = ut_get_initial_value(lt_entry->ut);
      } else if (res->lustre.type == BACKFILL_LICENSES_TWO_GROUP) {
        two_group_entry_t *lt_entry = res->lustre.vp_entry;
        total = lt_entry->total;
        used = ut_get_initial_value(lt_entry->ut);
        star_used = ut_get_initial_value(lt_entry->st);
        r_star = lt_entry->r_star;
        r_bar = lt_entry->r_bar;
        r_target = lt_entry->r_target;
        r_star_target = lt_entry->r_star_target;
      } else
        error("%s (%d): Not implemented license type", __func__, __LINE__);
      fprintf(log_file, "%ld, %d, %d, %d, %f, %f, %f, %d\n", 
            now, total, used, star_used, r_star, r_bar, r_target, r_star_target);
      fclose(log_file);
    }
    
  }

  return res;
}

int backfill_licenses_overlap(lic_tracker_p lt, job_record_t *job_ptr,
                              remote_estimates_t *estimates, time_t when) 
{
  time_t check = when;
  backfill_licenses_test_job(lt, job_ptr, estimates, &check); 
  int res = check != when;
  if (res) {
    debug3("%s: %pJ overlaps; scheduled: %ld, allowed: %ld", __func__, job_ptr,
           when, check);
    dump_lic_tracker(lt);
  }
  return res;
}

int backfill_licenses_test_job(lic_tracker_p lt, job_record_t *job_ptr,
                               remote_estimates_t *estimates, time_t *when) {
  /*AG TODO: implement reservations */
  /*AG FIXME: should probably use job_ptr->min_time if present */
  /*AG TODO: refactor algorithm */
  /* In case the job is scheduled,
   * we would like the job to fit perfectly if possible.
   * Thus, we will adjust time and duration
   * to what would be used for scheduling. */
  // AG TODO: better way to merge estimates with user data
  int lustre_value = estimates->lustre;
  if (job_ptr->license_list == NULL && lustre_value == 0) {
    debug3("%s: %pJ has no licenses -- skipping", __func__, job_ptr);
    return SLURM_SUCCESS;
  }
  // update lustre_value if explicitly set for the job
  int job_lustre_requirement = _license_cnt(job_ptr->license_list, LUSTRE);
  if (job_lustre_requirement == -1) {
    // We will use estimate, so we clip it if above total available
    // TODO (AG): refactor this
    if (lustre_value > 0 && lt->lustre.vp_entry) {
      // NOTE: we log later the error
      // when lustre_value > 0 && lt->lustre.vp_entry==NULL
      if (lt->lustre.type == BACKFILL_LICENSES_AWARE) {
        lt_entry_t *lt_entry = lt->lustre.vp_entry;
        if (lustre_value >= lt_entry->total) lustre_value = lt_entry->total - 1;
      } else if (lt->lustre.type == BACKFILL_LICENSES_TWO_GROUP) {
        two_group_entry_t *lt_entry = lt->lustre.vp_entry;
        if (lustre_value >= lt_entry->total) lustre_value = lt_entry->total - 1;
      } else
        error("%s (%d): Not implemented license type", __func__, __LINE__);
    }
  } else {
    // using provided value
    lustre_value = job_lustre_requirement;
  }
  // TODO: is it correct to move orig_start backward?
  time_t orig_start = _convert_time_floor(*when, lt->resolution);
  time_t duration = _convert_time_fwd(job_ptr->time_limit * 60, lt->resolution);

  int rc = SLURM_SUCCESS;
  ListIterator j_iter = job_ptr->license_list
                            ? list_iterator_create(job_ptr->license_list)
                            : NULL;
  licenses_t *license_entry;
  lt_entry_t *lt_entry;
  time_t curr_start = orig_start;
  time_t prev_start = orig_start;
  enum { FIRST_TIME, RESET, CONTINUE, ERROR } status = RESET;
  while (status == RESET) {
    status = FIRST_TIME;
    debug5("%s: %pJ: inside while loop", __func__, job_ptr);
    if (j_iter) {
      list_iterator_reset(j_iter);
      while ((license_entry = list_next(j_iter))) {
        if (xstrcmp(license_entry->name, LUSTRE) == 0) {
          continue;  // lustre is checked separately
        }
        if (license_entry->total == 0) {
          continue;
        }
        lt_entry = list_find_first(lt->other_licenses, _lt_find_lic_name,
                                   license_entry->name);
        if (lt_entry) {
          curr_start =
              ut_int_when_below(lt_entry->ut, prev_start, duration,
                                lt_entry->total - license_entry->total + 1);
          if (curr_start == -1) {
            error("%s: Job %pJ will never get %d license \"%s\"", __func__,
                  job_ptr, license_entry->total, license_entry->name);
            status = ERROR;
            rc = SLURM_ERROR;
            break;
          }
          if (status == FIRST_TIME) {
            prev_start = curr_start;
            status = CONTINUE;
          } else if (curr_start > prev_start) {
            prev_start = curr_start;
            status = RESET;
            break;
          }
        } else {
          error("%s: Job %pJ require unknown license \"%s\"", __func__, job_ptr,
                license_entry->name);
          rc = SLURM_ERROR;
          status = ERROR;
          break;
        }
      }
    }
    debug5("%s: %pJ: done with \"other\" licenses, moving to node count", __func__,
           job_ptr);
    if (status != ERROR && status != RESET && lt->node_entry) {
      lt_entry_t *node_entry = lt->node_entry;
      int job_node_count = _get_job_node_count(job_ptr);
      curr_start =
          ut_int_when_below(node_entry->ut, prev_start, duration,
                            node_entry->total - job_node_count + 1);
      if (curr_start == -1) {
        error("%s: Job %pJ will never get %d nodes", __func__,
              job_ptr, job_node_count);
        status = ERROR;
        rc = SLURM_ERROR;
        break; // NOTE: "continue" should also work here
      }
      if (status == FIRST_TIME) {
        prev_start = curr_start;
        status = CONTINUE;
      } else if (curr_start > prev_start) {
        prev_start = curr_start;
        status = RESET;
        continue;
      }
    }
    debug5("%s: %pJ: done with \"other\" licenses (and nodes), moving to lustre", __func__,
           job_ptr);
    if (status != ERROR && status != RESET && lustre_value > 0) {
      if (!lt->lustre.vp_entry) {
        error(
            "%s: Job %pJ is estimated to require lustre which is not in the "
            "licenses list",
            __func__, job_ptr);
        rc = SLURM_ERROR;
        status = ERROR;
        continue; // NOTE: break should also work here
      }
      utracker_int_t ut = NULL;
      int total = -1;
      if (lt->lustre.type == BACKFILL_LICENSES_AWARE) {
        lt_entry = lt->lustre.vp_entry;
        ut = lt_entry->ut;
        total = lt_entry->total;
      } else if (lt->lustre.type == BACKFILL_LICENSES_TWO_GROUP) {
        two_group_entry_t *lt_entry = lt->lustre.vp_entry;
        ut = lt_entry->ut;
        total =  lt_entry->total;
      } else
        error("%s (%d): Not implemented license type", __func__, __LINE__);
      debug5(
          "%s: %pJ: trying lustre ut_int_when_below, prev_start: %ld "
          "lustre_value: "
          "%d",
          __func__, job_ptr, prev_start, lustre_value);
      curr_start =
          ut_int_when_below(ut, prev_start, duration, total - lustre_value + 1);
      debug5("%s: %pJ: exited lustre ut_int_when_below. Result: %ld", __func__,
             job_ptr, curr_start);

      if (curr_start == -1) {
        error("%s: Job %pJ will never get %d license \"%s\"", __func__, job_ptr,
              lustre_value, LUSTRE);
        status = ERROR;
        rc = SLURM_ERROR;
        continue;
      } else if (status == FIRST_TIME) {
        prev_start = curr_start;
        status = CONTINUE;
      } else if (curr_start > prev_start) {
        prev_start = curr_start;
        status = RESET;
        continue;
      } // done checking "normal lustre"

      // if we reached here we should already agree on everything including lustre limit (status==CONTINUE)
      // and the only thing we need to check is the "lustre star target"
      if (lt->lustre.type == BACKFILL_LICENSES_TWO_GROUP) {
        two_group_entry_t *lt_entry = lt->lustre.vp_entry;
        int node_count = _get_job_node_count(job_ptr);
        // we only check if the job is not a "zero job"
        if (lustre_value > (int) (node_count * lt_entry->r_star)) { 
          double star_value = (double)lustre_value - (lt_entry->r_bar * (double)node_count);
          // FIXME: implement logic that allows to go over the target
          double min_target = lt_entry->r_star_target - star_value;
          if (min_target < 0.0) min_target = 0.0;
          double delta = lt_entry->r_star_target - min_target;
          int used_target = (int) (0.5 + min_target + lt_entry->random * delta);
          debug5("%s: %pJ: min target: %f, max target: %d, used target: %d", __func__,
                 job_ptr, min_target, lt_entry->r_star_target, used_target);
          curr_start = ut_int_when_below(lt_entry->st, prev_start, duration,
                                         used_target + 1);
          if (curr_start == -1) {
            error("%s: BUG: Job %pJ cannot get %d \"lustre star target\" - should not have happened",
                  __func__, job_ptr, used_target);
            status = ERROR;
            rc = SLURM_ERROR;
            continue;
          }
          if (curr_start > prev_start) {
            prev_start = curr_start;
            status = RESET;
          }
        }
      }  // done checking the "lustre star target"
    } // done checking lustre
  }
  if (j_iter) list_iterator_destroy(j_iter);
  /* if the job fits at the requested time, don't update "when"
   * to avoid rounding.
   * Otherwise, update it. */
  if (curr_start != orig_start) {
    *when = curr_start;
  }
  return rc;
}

int backfill_licenses_alloc_job(lic_tracker_p lt, job_record_t *job_ptr,
                                remote_estimates_t *estimates, time_t start,
                                time_t end) {
  /*AG TODO: implement reservations */
  if (job_ptr->license_list == NULL && estimates->lustre == 0) {
    debug3("%s: %pJ has NULL license list -- skipping", __func__, job_ptr);
    return SLURM_SUCCESS;
  }
  int lustre_value = estimates->lustre;
  lt_entry_t *lt_entry;
  start = _convert_time_floor(start, lt->resolution);
  end = _convert_time_fwd(end, lt->resolution);
  if (job_ptr->license_list) {
    ListIterator j_iter = list_iterator_create(job_ptr->license_list);
    licenses_t *license_entry;
    while (NULL != (license_entry = list_next(j_iter))) {
      if (xstrcmp(license_entry->name, LUSTRE) == 0) {
        lustre_value = license_entry->total;
      } else {
        lt_entry = list_find_first(lt->other_licenses, _lt_find_lic_name,
                                   license_entry->name);
        if (lt_entry) {
          ut_int_add_usage(lt_entry->ut, start, end, license_entry->total);
        } else {
          error("%s: Job %pJ require unknown license \"%s\"", __func__, job_ptr,
                license_entry->name);
        }
      }
    }
    list_iterator_destroy(j_iter);
  }

  if (lt->node_entry) {
    lt_entry = lt->node_entry;
    ut_int_add_usage(lt_entry->ut, start, end, _get_job_node_count(job_ptr));
  }

  if (lustre_value > 0) {
    if (!lt->lustre.vp_entry) {
      error("%s: Job %pJ is estimated to use lustre which is not configured",
            __func__, job_ptr);
    } else if (lt->lustre.type == BACKFILL_LICENSES_AWARE) {
      lt_entry = lt->lustre.vp_entry;
        ut_int_add_usage(lt_entry->ut, start, end, lustre_value);     
    } else if (lt->lustre.type == BACKFILL_LICENSES_TWO_GROUP) {
      two_group_entry_t *lt_entry = lt->lustre.vp_entry;
      ut_int_add_usage(lt_entry->ut, start, end, lustre_value);
      int node_count = _get_job_node_count(job_ptr);
      // we only adjust star tracker if the job is not a "zero job"
      if (lustre_value > (int)(node_count * lt_entry->r_star)) {
        int star_value =
            lustre_value - (int)(0.5 + lt_entry->r_bar * (double)node_count);
        if (star_value > 0)
          ut_int_add_usage(lt_entry->st, start, end, star_value);
      }
    } else
      error("%s (%d): Not implemented license type", __func__, __LINE__);
  }
  debug3("%s: allocated licenses for %pJ :", __func__, job_ptr);
  dump_lic_tracker(lt);
  return SLURM_SUCCESS;
}
