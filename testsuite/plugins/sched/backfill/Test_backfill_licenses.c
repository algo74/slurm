
#include "unity.h"
#include "execinfo.h"
#include "signal.h"

#include "list.h"
#include "log.h"

#include "src/plugins/sched/backfill/backfill_licenses.h"
// #include "src/plugins/sched/backfill/cJSON.h"
// char *get_variety_id(job_record_t *job_ptr);
#include "src/slurmctld/licenses.h"
#include "src/slurmctld/slurmctld.h"
#include "src/common/xstring.h"

#include "override_internal.h"

#define COUNT_OF(x) \
  ((sizeof(x) / sizeof(0 [x])) / ((size_t)(!(sizeof(x) % sizeof(0 [x])))))

#define N 1024

/************************************************************
 Mocks
************************************************************/

static time_t _my_time = 10000;
static time_func_p old_time_func;

static time_t my_time(time_t *arg) {
  return _my_time;
}

int _default_get_job_utilization_from_remote(job_record_t *job_ptr,
                                    remote_estimates_t *results) {
  return 3;
}

int (*mock_job_utilization_from_remote)(job_record_t *, remote_estimates_t *) =
    _default_get_job_utilization_from_remote;

int get_job_utilization_from_remote(job_record_t *job_ptr,
                                             remote_estimates_t *results) {
  return mock_job_utilization_from_remote(job_ptr, results);
}

void _clear_server_strings() {
  mock_job_utilization_from_remote =
    _default_get_job_utilization_from_remote;
}

/** bitmaps **/


/************************************************************
 INTERNALS (duplicated here from implementation)
************************************************************/

/** from Slurm core **/
uint16_t accounting_enforce = 0;

/** for usage_tracker **/
typedef struct ut_int_struct {
  time_t start;
  int value;
} ut_int_item_t;

/** for backfill_licenses **/

typedef struct lt_entry_struct {
  char *name;
  uint32_t total;
  utracker_int_t ut;
} lt_entry_t;

typedef struct two_group_entry_struct {
  char *name;
  uint32_t total;
  utracker_int_t ut;
  utracker_int_t st;
  double r_star;
  double r_bar;
  double r_target;    // target rate per node
  int r_star_target;  // recalculated target value per cluster
  uint32_t n_total;
  double random;  // [0,1] determine how much over the target is allowed at the
                  // current round
  // TODO
} two_group_entry_t;

static int _lt_find_lic_name(void *x, void *key) {
  lt_entry_t *entry = (lt_entry_t *)x;
  char *name = (char *)key;

  if ((entry->name == NULL) || (name == NULL)) return 0;
  if (xstrcmp(entry->name, name)) return 0;
  return 1;
}

static lt_entry_t *_entry_from_lt(lic_tracker_p lt, char *name) {
  if (lt) {
    lt_entry_t *lt_entry;
    if (strcmp(name, "lustre") == 0) {
      return lt->lustre.vp_entry;
    } else {
      return list_find_first(lt->other_licenses, _lt_find_lic_name, name);
    }
  } else
    return NULL;
}

static utracker_int_t _ut_from_lt(lic_tracker_p lt, char *name) {
  lt_entry_t *lt_entry = _entry_from_lt(lt, name); 
  if (lt_entry) {
    return lt_entry->ut;
  } else {
    return NULL;
  }
}

/************************************************************
 IMPLEMENTATION-SPECIFIC HELPERS
************************************************************/

static void _assert_ut_match(utracker_int_t expected, utracker_int_t actual,
                             bool strict, char *comment) {
  // TODO(AG): debug
  static char message[N + 1];
  xfree(message);
  size_t ie = 0;
  ListIterator ex_it = list_iterator_create(expected);
  ListIterator ac_it = list_iterator_create(actual);
  ut_int_item_t *ex_next = list_next(ex_it);
  ut_int_item_t *ac_next = list_next(ac_it);
  int ex_prev = ex_next->value;
  int ac_prev = ac_next->value;
  if (ac_next->start != ex_next->start) {
    snprintf(message, N, "%s: Start times expected: %ld, actual: %ld", comment,
             ex_next->start, ac_next->start);
    TEST_FAIL_MESSAGE(message);
  };
  if (ac_next->value != ex_next->value) {
    snprintf(message, N, "%s: Start values expected: %d, actual: %d", comment,
             ex_next->value, ac_next->value);
    TEST_FAIL_MESSAGE(message);
  };
  ex_next = list_next(ex_it);
  ac_next = list_next(ac_it);
  int ex_i = 2, ac_i = 2;
  while (ex_next && ac_next) {
    // printf("expected step: %d (%ld, %d), actual step: %d(%ld, %d)\n",
    //        ex_i, ex_next->start, ex_next->value, ac_i, ac_next->start,
    //        ac_next->value);
    // fflush(stdout);
    if (ex_next->start != ac_next->start) {
      if (strict) {
        snprintf(message, N, "%s: Step %d times expected: %ld, actual: %ld",
                 comment, ex_i, ex_next->start, ac_next->start);
        TEST_FAIL_MESSAGE(message);
      } else if (ex_next->start > ac_next->start) {
        if (ac_next->value != ex_prev) {
          snprintf(message, N,
                   "%s: Time %ld (before expected step %d, actual step %d) "
                   "value expected: %d, actual: %d",
                   comment, ac_next->start, ex_i, ac_i, ex_prev,
                   ac_next->value);
          TEST_FAIL_MESSAGE(message);
        } else {
          ac_prev = ac_next->value;
          ac_i += 1;
          ac_next = list_next(ac_it);
          continue;
        }
      } else /* (ex_next->start < ac_next->start) */ {
        if (ex_next->value != ac_prev) {
          snprintf(message, N,
                   "%s: Time %ld (expected step %d, before actual step %d) "
                   "value expected: %d, actual: %d",
                   comment, ex_next->start, ex_i, ac_i, ex_next->value,
                   ac_prev);
          TEST_FAIL_MESSAGE(message);
        } else {
          ex_prev = ex_next->value;
          ex_i += 1;
          ex_next = list_next(ex_it);
          continue;
        }
      }
    } else /* (ex_next->start == ac_next->start) */ {
      if (ex_next->value != ac_next->value) {
        snprintf(message, N,
                 "%s: Time %ld (expected step %d, actual step %d) value "
                 "expected: %d, actual: %d",
                 comment, ex_next->start, ex_i, ac_i, ex_next->value,
                 ac_next->value);
        TEST_FAIL_MESSAGE(message);
      } else {
        ex_prev = ex_next->value;
        ex_i += 1;
        ex_next = list_next(ex_it);
        ac_prev = ac_next->value;
        ac_i += 1;
        ac_next = list_next(ac_it);
        continue;
      }
    }
  }
  if (strict && (ac_next || ex_next)) {
    if (ex_next) {
      snprintf(message, N, "%s: Step %d missing (time: %ld, value: %d)",
               comment, ex_i, ex_next->start, ex_next->value);
    } else {
      snprintf(message, N, "%s: Step %d (time: %ld, value: %d) is not expected",
               comment, ex_i, ac_next->start, ac_next->value);
    }
    TEST_FAIL_MESSAGE(message);
  }
}

static double _get_r_target(lic_tracker_p lt) {
  two_group_entry_t *entry = lt->lustre.vp_entry;
  return entry->r_target;
}

static double _get_r_star(lic_tracker_p lt) {
  two_group_entry_t *entry = lt->lustre.vp_entry;
  return entry->r_star;
}

static double _get_r_bar(lic_tracker_p lt) {
  two_group_entry_t *entry = lt->lustre.vp_entry;
  return entry->r_bar;
}

static double _get_star_random(lic_tracker_p lt) {
  two_group_entry_t *entry = lt->lustre.vp_entry;
  return entry->random;
}

static int _get_r_star_target(lic_tracker_p lt) {
  two_group_entry_t *entry = lt->lustre.vp_entry;
  return entry->r_star_target;
}

static int _get_n_total(lic_tracker_p lt) {
  two_group_entry_t *entry = lt->lustre.vp_entry;
  return entry->n_total;
}

utracker_int_t _get_star_tracker(lic_tracker_p lt) {
  two_group_entry_t *entry = lt->lustre.vp_entry;
  return entry->st;
}

/************************************************************
 HELPERS
************************************************************/

typedef struct job_precursor_struct {
  int id;
  uint32_t time_limit;  // minutes
  int start_time;       // shift in seconds from "now"
  uint32_t nodes;
  uint32_t job_state;
  int lustre; // -1 if not set
} job_precursor_t;

typedef struct step_func_step_struct {
  int start;
  int value;
  //step_func_step_t *next;
} step_func_step_t;

typedef struct step_func_struct {
  step_func_step_t *steps;
  size_t len;
} step_func_t;

#define STEP_FUNC(name, ...)                           \
  static step_func_step_t body_##name[] = __VA_ARGS__; \
  step_func_t name = {body_##name, COUNT_OF(body_##name)}


static utracker_int_t _ut_from_step_func(step_func_t *sf) {
  if (sf->len == 0) {
    return NULL;
  }
  int prev_value = sf->steps[0].value;
  utracker_int_t res = ut_int_create(prev_value);
  for(int i=0; i<sf->len; ++i) {
    ut_int_remove_till_end(res, sf->steps[i].start,
                           prev_value - sf->steps[i].value);
    prev_value = sf->steps[i].value;
  }
  return res;
}

static void _destroy_licenses(void) {
  if (job_list)
    list_destroy(job_list);
  job_list = NULL;
}

static void _destroy_jobs(void) {
  if (license_list)
    list_destroy(license_list);
  license_list = NULL;
}

static void _job_delete(void *x) {
  job_record_t *entry = (job_record_t *)x;
  if (entry) {
    if (entry->details) {
      xfree(entry->details);
    }
    xfree(entry);
  }
}

static void _init_job_list(){
  job_list = list_create(_job_delete);
}

static void _add_job(job_record_t *entry) {
  list_append(job_list, entry);
}

static job_record_t *_create_job(job_precursor_t *prec, time_t now) {
  job_record_t *job = xmalloc(sizeof(job_record_t));
  memset(job, 0, sizeof(job_record_t));
  job->magic = JOB_MAGIC;
  job->details = xmalloc(sizeof(struct job_details));
  memset(job->details, 0, sizeof(struct job_details));
  job->job_id = prec->id;
  job->time_limit = prec->time_limit;
  job->start_time = (time_t) ((long) now + (long) prec->start_time);
  job->end_time = job->time_limit * 60 + job->start_time;
  job->node_cnt = prec->nodes;
  job->details->min_nodes = prec->nodes;
  job->details->max_nodes = prec->nodes;
  job->job_state = prec->job_state;
  job->array_task_id = NO_VAL;
  if(prec->lustre >= 0) {
    job->license_list = list_create(license_free_rec);
    licenses_t *license_entry = xmalloc(sizeof(licenses_t));
    license_entry->name = "lustre";
    license_entry->total = prec->lustre;
    list_push(job->license_list, license_entry);
  }
  return job;
}

static void _add_jobs(int count, job_precursor_t precs[], time_t when) {
  // time_t now = time(NULL);
  // time_t now = 0;
  for(int i=0; i<count; ++i) {
    _add_job(_create_job(&precs[i], when));
  }
}

static void _add_jobs_several_times(int count, job_precursor_t precs[], int how_many, time_t when, int time_increment, int start_job_id) {
  int job_id = start_job_id;
  for(int n=0; n<how_many; ++n, when+=time_increment) {
    for(int i=0; i<count; ++i, ++job_id) {
      job_record_t *job = _create_job(&precs[i], when);
      job->job_id = job_id;
      _add_job(job);
    }
  }
}

static void _reset_jobs() {

}

/************************************************************
 TEST REUSABLES
************************************************************/

job_precursor_t jobs0[3] = {{1, 100, 1000, 10, JOB_PENDING, 100},
                            {2, 100, 2000, 10, JOB_PENDING, 100},
                            {3, 100, 3000, 10, JOB_PENDING, 100}};

job_precursor_t jobs1[3] = {{4, 100, 6000, 10, JOB_RUNNING, -1},
                            {5, 100, 6200, 10, JOB_RUNNING, -1},
                            {6, 100, 6300, 10, JOB_RUNNING, -1}};

job_precursor_t jobs2[3] = {{4, 100, 6000, 10, JOB_RUNNING, 100},
                            {5, 100, 6200, 10, JOB_RUNNING, 100},
                            {6, 100, 6300, 10, JOB_RUNNING, 300}};

job_precursor_t jobs_cycle1[3] = {{200, 100, 6000, 10, JOB_PENDING, 100},
                                  {201, 100, 6200, 10, JOB_PENDING, 100},
                                  {202, 100, 6300, 10, JOB_PENDING, 300}};

int _jobs1a_get_job_utilization_from_remote(job_record_t *job_ptr,
                                            remote_estimates_t *results) {
  switch (job_ptr->job_id) {
    case 4:
    case 5:
    case 6:
      results->lustre = 100;
      return 1;
    default:
      return 3;
  }
}

int _jobs1b_get_job_utilization_from_remote(job_record_t *job_ptr,
                                          remote_estimates_t *results) {
  switch (job_ptr->job_id) {
    case 4:
      results->lustre = 200;
      return 1;
    case 5:
      results->lustre = 300;
      return 1;
    case 6:
      results->lustre = 400;
      return 1;
    case 101:
      results->lustre = 400;
      return 1;
    case 102:
      results->lustre = 1400;
      return 1;
    default:
      return 3;
  }
}

int _jobs2a_get_job_utilization_from_remote(job_record_t *job_ptr,
                                            remote_estimates_t *results)
{
  switch (job_ptr->job_id) {
    case 4:
      results->lustre = 200;
      results->timelimit = 12;
      return 1;
    case 5:
      results->lustre = 300;
      results->timelimit = 14;
      return 1;
    case 6:
      results->lustre = 400;
      results->timelimit = 16;
      return 1;
    case 101:
      results->lustre = 400;
      results->timelimit = 18;
      return 1;
    case 102:
      results->lustre = 1400;
      results->timelimit = 20;
      return 1;
    default:
      results->timelimit = 10;
      return 2;
  }
}

/**
 * Creats a list with only "lustre" license
*/
static void _create_licenses1(void) {
  license_list = list_create(license_free_rec);
  licenses_t *license_entry = xmalloc(sizeof(licenses_t));
  license_entry->name = xstrdup("lustre");
  license_entry->total = 1000;
  license_entry->used = 500;
  list_push(license_list, license_entry);
}


/************************************************************
 TESTS
************************************************************/

static void handler(int sig) {
  void *array[10];
  size_t size;

  // get void*'s for all entries on the stack
  size = backtrace(array, 10);

  // print out all the frames to stderr
  fprintf(stderr, "Error: signal %d:\n", sig);
  backtrace_symbols_fd(array, size, STDERR_FILENO);
  exit(1);
}

void setUp(void) {
  // nothing
  fflush(stderr);
  fflush(stdout);
  TEST_ASSERT_NULL_MESSAGE(license_list,
                           "license_list must be NULL before setup");
  TEST_ASSERT_NULL_MESSAGE(job_list,
                           "job_list must be NULL before setup");
  _reset_jobs();
  _clear_server_strings();
  old_time_func = set_unit_test_override_time(my_time);
}

void tearDown(void) {
  configure_backfill_licenses(BACKFILL_LICENSES_AWARE);
  configure_total_node_count(-1);
  _destroy_licenses();
  _destroy_jobs();
  TEST_ASSERT_NULL_MESSAGE(license_list,
                           "license_list must be NULL after each test");
  set_unit_test_override_time(old_time_func);
}

void test_step_func() {
  /*
    Checks mostly the "internal" test functions - not the actual production code
  */
  STEP_FUNC(test, {{10, 20}, {30, 40}});
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, test.len, "Lenght should be correct");
  TEST_ASSERT_EQUAL_INT_MESSAGE(10, test.steps[0].start, "First step start");
  TEST_ASSERT_EQUAL_INT_MESSAGE(20, test.steps[0].value, "First step value");
  TEST_ASSERT_EQUAL_INT_MESSAGE(30, test.steps[1].start, "Second step start");
  TEST_ASSERT_EQUAL_INT_MESSAGE(40, test.steps[1].value, "Second step value");
}

void test_assert_ut_match() {
  /*
    Checks mostly the "internal" test functions - not the actual production code
  */
  utracker_int_t first = ut_int_create(100);
  ut_int_add_usage(first, 10, 20, 50);
  STEP_FUNC(sf, {{-1, 100}, {10, 150}, {20, 100}});
  utracker_int_t second = _ut_from_step_func(&sf);
  _assert_ut_match(first, second, false, "not strict match");
  _assert_ut_match(first, second, true, "final match");
  list_destroy(first);
  list_destroy(second);
}

void test_init_lic_tracker_no_licenses() {
  lic_tracker_p lt = init_lic_tracker(60);
  TEST_ASSERT_NULL_MESSAGE(lt, "lic_tracker must be NULL if no licenses are set up");
  destroy_lic_tracker(lt);
}

void test_init_lic_tracker_no_job_list() {
  _create_licenses1();
  TEST_ASSERT_NOT_NULL_MESSAGE(license_list, "Must create license list");
  lic_tracker_p lt = init_lic_tracker(60);
  TEST_ASSERT_NOT_NULL_MESSAGE(
      lt, "lic_tracker must be created");
  lt_entry_t *lt_entry = _entry_from_lt(lt, "lustre");
  TEST_ASSERT_NOT_NULL_MESSAGE(lt_entry, "lustre lt_entry must be created");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1000, lt_entry->total, "lustre max must be set");
  utracker_int_t ut = _ut_from_lt(lt, "lustre");
  STEP_FUNC(sf, {{-1, 500}});
  utracker_int_t expected = _ut_from_step_func(&sf);
  _assert_ut_match(expected, ut, true, "strict");
  ut_int_destroy(expected);
  destroy_lic_tracker(lt);
}

void test_init_lic_tracker_empty_job_list() {
  _create_licenses1();
  _init_job_list();
  lic_tracker_p lt = init_lic_tracker(60);
  TEST_ASSERT_NOT_NULL_MESSAGE(lt, "lic_tracker must be created");
  utracker_int_t ut = _ut_from_lt(lt, "lustre");
  TEST_ASSERT_NOT_NULL_MESSAGE(ut, "lustre tracker must be created");
  STEP_FUNC(sf, {{-1, 500}});
  utracker_int_t expected = _ut_from_step_func(&sf);
  _assert_ut_match(expected, ut, true, "strict");
  ut_int_destroy(expected);
  destroy_lic_tracker(lt);
}

void test_init_lic_tracker_no_running_jobs() {
  _create_licenses1();
  _init_job_list();
  _add_jobs(COUNT_OF(jobs0), jobs0, 0);
  lic_tracker_p lt = init_lic_tracker(60);
  utracker_int_t ut = _ut_from_lt(lt, "lustre");
  TEST_ASSERT_NOT_NULL_MESSAGE(ut, "lustre tracker must be created");
  STEP_FUNC(sf, {{-1, 500}});
  utracker_int_t expected = _ut_from_step_func(&sf);
  _assert_ut_match(expected, ut, true, "strict");
  ut_int_destroy(expected);
  destroy_lic_tracker(lt);
}

void test_init_lic_tracker_running_jobs1a() {
  /*
   * no licenses in the jobs
   * same predictions for all jobs
   */
  mock_job_utilization_from_remote = _jobs1a_get_job_utilization_from_remote;
  _create_licenses1();
  _init_job_list();
  // time_t now = time(NULL);
  time_t now = 0;
  _add_jobs(COUNT_OF(jobs1), jobs1, now);
  lic_tracker_p lt = init_lic_tracker(60);
  utracker_int_t ut = _ut_from_lt(lt, "lustre");
  TEST_ASSERT_NOT_NULL_MESSAGE(ut, "lustre tracker must be created");
  STEP_FUNC(sf, {
                    {-1, 800},
                    {12060, 700},
                    {12240, 600},
                    {12360, 500},
                });
  utracker_int_t expected = _ut_from_step_func(&sf);
  _assert_ut_match(expected, ut, true, "strict");
  ut_int_destroy(expected);
  destroy_lic_tracker(lt);
}

void test_init_lic_tracker_running_jobs1b() {
  /*
   * no licenses in the jobs
   * different predictions for the jobs
   */
  mock_job_utilization_from_remote = _jobs1b_get_job_utilization_from_remote;
  _create_licenses1();
  _init_job_list();
  // time_t now = time(NULL);
  time_t now = 0;
  _add_jobs(COUNT_OF(jobs1), jobs1, now);
  lic_tracker_p lt = init_lic_tracker(60);
  utracker_int_t ut = _ut_from_lt(lt, "lustre");
  TEST_ASSERT_NOT_NULL_MESSAGE(ut, "lustre tracker must be created");
  STEP_FUNC(sf, {
                    {-1, 1400},
                    {12060, 1200},
                    {12240, 900},
                    {12360, 500},
                });
  utracker_int_t expected = _ut_from_step_func(&sf);
  _assert_ut_match(expected, ut, true, "strict");
  ut_int_destroy(expected);
  destroy_lic_tracker(lt);
}

void test_init_lic_tracker_running_jobs2() {
  /*
      * set licenses in the jobs
   */
  mock_job_utilization_from_remote = _jobs1b_get_job_utilization_from_remote;
  _create_licenses1();
  _init_job_list();
  // time_t now = time(NULL);
  time_t now = 0;
  _add_jobs(COUNT_OF(jobs2), jobs2, now);
  lic_tracker_p lt = init_lic_tracker(60);
  utracker_int_t ut = _ut_from_lt(lt, "lustre");
  TEST_ASSERT_NOT_NULL_MESSAGE(ut, "lustre tracker must be created");
  STEP_FUNC(sf, {
                    {-1, 500},
                    {12060, 400},
                    {12240, 300},
                    {12360, 0},
                });
  utracker_int_t expected = _ut_from_step_func(&sf);
  _assert_ut_match(expected, ut, true, "strict");
  ut_int_destroy(expected);
  destroy_lic_tracker(lt);
}

void test_backfill_licenses_test_job_1() {
  /*

  */
 /* setup */
  // mock_job_utilization_from_remote = _jobs1b_get_job_utilization_from_remote;
  _create_licenses1();
  _init_job_list();
  // time_t now = time(NULL);
  // time_t now = 0;
  _add_jobs(COUNT_OF(jobs2), jobs2, 0);
  lic_tracker_p lt = init_lic_tracker(60);
  lt_entry_t *lt_entry = _entry_from_lt(lt, "lustre");
  TEST_ASSERT_NOT_NULL_MESSAGE(lt_entry, "lustre lt_entry must be created");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1000, lt_entry->total,
                                "lustre max must be set");
  utracker_int_t ut = _ut_from_lt(lt, "lustre");
  TEST_ASSERT_NOT_NULL_MESSAGE(ut, "lustre tracker must be created");
  STEP_FUNC(sf, {
                    {-1, 500},
                    {12060, 400},
                    {12240, 300},
                    {12360, 0},
                });
  utracker_int_t expected = _ut_from_step_func(&sf);
  // printf("testing\n");
  // fflush(stdout);
  _assert_ut_match(expected, ut, true,
                   "initial ut is same as test_init_lic_tracker_running_jobs2");
  ut_int_destroy(expected);
  /* now test backfill licenses */
  job_precursor_t prec = { 101, 100, 3000, 10, JOB_PENDING, -1 };
  job_record_t * job_ptr = _create_job(&prec, 0);
  remote_estimates_t estimates = {0, 0};
  time_t when = 10000;
  int rc = backfill_licenses_test_job(lt, job_ptr, &estimates, &when);
  TEST_ASSERT_EQUAL_INT64_MESSAGE(10000, when, "job with zero lustre can start right away");
  TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
  estimates.lustre = 600;
  rc = backfill_licenses_test_job(lt, job_ptr, &estimates, &when);
  TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc,
                                "job can be scheduled");
  TEST_ASSERT_EQUAL_INT64_MESSAGE(12060, when,
                                  "job with 600 lustre can start when job 4 finishes");
  estimates.lustre = 1200;
  rc = backfill_licenses_test_job(lt, job_ptr, &estimates, &when);
  TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc,
                                "job with overestimate can be scheduled");
  TEST_ASSERT_EQUAL_INT64_MESSAGE(
      12360, when, "job with lustre overestimate can start when all jobs finish");

  _job_delete(job_ptr);
  destroy_lic_tracker(lt);
}

void test_backfill_licenses_test_job_and_alloc_job_estimates() {
  /*
    TODO: docs
  */
  /* setup */
  _create_licenses1();
  _init_job_list();
  _add_jobs(COUNT_OF(jobs2), jobs2, 0);
  lic_tracker_p lt = init_lic_tracker(60);
  lt_entry_t *lt_entry = _entry_from_lt(lt, "lustre");
  TEST_ASSERT_NOT_NULL_MESSAGE(lt_entry, "lustre lt_entry must be created");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1000, lt_entry->total,
                                "lustre max must be set");
  utracker_int_t ut = _ut_from_lt(lt, "lustre");
  TEST_ASSERT_NOT_NULL_MESSAGE(ut, "lustre tracker must be created");
  STEP_FUNC(sf, {
                    {-1, 500},
                    {12060, 400},
                    {12240, 300},
                    {12360, 0},
                });
  utracker_int_t expected = _ut_from_step_func(&sf);
  _assert_ut_match(expected, ut, true,
                   "initial ut is same as test_init_lic_tracker_running_jobs2");
  ut_int_destroy(expected);
  /* test backfill licenses on original step function */
  job_precursor_t prec1 = {101, 100, 3000, 10, JOB_PENDING, -1};
  job_record_t *job_ptr1 = _create_job(&prec1, 0);
  remote_estimates_t estimates1 = {0, 600};
  time_t when = 10000;
  int rc = backfill_licenses_test_job(lt, job_ptr1, &estimates1, &when);
  TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
  TEST_ASSERT_EQUAL_INT64_MESSAGE(
      12060, when, "job with 600 lustre can start when job 4 finishes");
  when = 10000;
  job_precursor_t prec2 = {102, 100, 3000, 10, JOB_PENDING, -1};
  job_record_t *job_ptr2 = _create_job(&prec2, 0);
  remote_estimates_t estimates2 = {0, 1200};
  rc = backfill_licenses_test_job(lt, job_ptr2, &estimates2, &when);
  TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc,
                                "job with overestimate can be scheduled");
  TEST_ASSERT_EQUAL_INT64_MESSAGE(
      12360, when,
      "job with lustre overestimate can start when all jobs finish");
  /* allocate a job */
  rc = backfill_licenses_alloc_job(lt, job_ptr2, &estimates2, when,
                                   when + prec2.time_limit * 60);
  TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc,
                                "job with overestimate can be allocated");
  STEP_FUNC(sf2, {
                    {-1, 500},
                    {12060, 400},
                    {12240, 300},
                    {12360, 1200},
                    {18420, 0},
                });
  expected = _ut_from_step_func(&sf2);
  _assert_ut_match(expected, ut, true,
                   "ut after allocation job with timelimit=100min and lustre=1200");
  ut_int_destroy(expected);
  /* test backfill licenses on the modified step function */
  when = 10000;
  rc = backfill_licenses_test_job(lt, job_ptr1, &estimates1, &when);
  TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
  TEST_ASSERT_EQUAL_INT64_MESSAGE(
      18420, when, "job with 600 lustre can start when job 102 finishes");
  when = 10000;
  job_ptr1->time_limit = 4;
  rc = backfill_licenses_test_job(lt, job_ptr1, &estimates1, &when);
  TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
  TEST_ASSERT_EQUAL_INT64_MESSAGE(
      12060, when,
      "4 min job with 600 lustre can still start when job 4 finishes");
  when = 10000;
  estimates1.lustre = 0;
  rc = backfill_licenses_test_job(lt, job_ptr1, &estimates1, &when);
  TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
  TEST_ASSERT_EQUAL_INT64_MESSAGE(
      10000, when,
      "job with no lustre can still start right away");
  _job_delete(job_ptr1);
  _job_delete(job_ptr2);
  destroy_lic_tracker(lt);
}

void test_backfill_licenses_test_job_and_alloc_job_presets() {
  /*
    TODO: docs
  */
  /* setup */
  _create_licenses1();
  _init_job_list();
  _add_jobs(COUNT_OF(jobs2), jobs2, 0);
  lic_tracker_p lt = init_lic_tracker(60);
  lt_entry_t *lt_entry = _entry_from_lt(lt, "lustre");
  TEST_ASSERT_NOT_NULL_MESSAGE(lt_entry, "lustre lt_entry must be created");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1000, lt_entry->total,
                                "lustre max must be set");
  utracker_int_t ut = _ut_from_lt(lt, "lustre");
  TEST_ASSERT_NOT_NULL_MESSAGE(ut, "lustre tracker must be created");
  STEP_FUNC(sf, {
                    {-1, 500},
                    {12060, 400},
                    {12240, 300},
                    {12360, 0},
                });
  utracker_int_t expected = _ut_from_step_func(&sf);
  _assert_ut_match(expected, ut, true,
                   "initial ut is same as test_init_lic_tracker_running_jobs2");
  ut_int_destroy(expected);
  /* test backfill licenses on original step function */
  job_precursor_t prec1 = {101, 100, 3000, 10, JOB_PENDING, 600};
  job_record_t *job_ptr1 = _create_job(&prec1, 0);
  remote_estimates_t estimates1 = {0, 0};
  time_t when = 10000;
  int rc = backfill_licenses_test_job(lt, job_ptr1, &estimates1, &when);
  TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
  TEST_ASSERT_EQUAL_INT64_MESSAGE(
      12060, when, "job with 600 lustre can start when job 4 finishes");
  when = 10000;
  job_precursor_t prec2 = {102, 100, 3000, 10, JOB_PENDING, 1200};
  job_record_t *job_ptr2 = _create_job(&prec2, 0);
  remote_estimates_t estimates2 = {0, 0};
  rc = backfill_licenses_test_job(lt, job_ptr2, &estimates2, &when);
  TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_ERROR, rc,
                                "job with overerequirement cannot be scheduled");
  TEST_ASSERT_EQUAL_INT64_MESSAGE(
      -1, when,
      "job with overerequirement will never start");
  /* allocate a job */
  rc = backfill_licenses_alloc_job(lt, job_ptr2, &estimates2, 12360,
                                   12360 + prec2.time_limit * 60);
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      SLURM_SUCCESS, rc, "reservation with overerequirement can be allocated");
  STEP_FUNC(sf2, {
                     {-1, 500},
                     {12060, 400},
                     {12240, 300},
                     {12360, 1200},
                     {18420, 0},
                 });
  expected = _ut_from_step_func(&sf2);
  _assert_ut_match(
      expected, ut, true,
      "ut after allocation job with timelimit=100min and lustre=1200");
  ut_int_destroy(expected);
  /* test backfill licenses on the modified step function */
  when = 10000;
  rc = backfill_licenses_test_job(lt, job_ptr1, &estimates1, &when);
  TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
  TEST_ASSERT_EQUAL_INT64_MESSAGE(
      18420, when, "job with 600 lustre can start when job 102 finishes");
  when = 10000;
  job_ptr1->time_limit = 4;
  rc = backfill_licenses_test_job(lt, job_ptr1, &estimates1, &when);
  TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
  TEST_ASSERT_EQUAL_INT64_MESSAGE(
      12060, when,
      "4 min job with 600 lustre can still start when job 4 finishes");
  _job_delete(job_ptr1);
  _job_delete(job_ptr2);
  destroy_lic_tracker(lt);
}

void test_backfill_licenses_overlap() {
  /*
    TODO: docs
  */
  /* setup */
  _create_licenses1();
  _init_job_list();
  _add_jobs(COUNT_OF(jobs2), jobs2, 0);
  lic_tracker_p lt = init_lic_tracker(60);
  lt_entry_t *lt_entry = _entry_from_lt(lt, "lustre");
  TEST_ASSERT_NOT_NULL_MESSAGE(lt_entry, "lustre lt_entry must be created");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1000, lt_entry->total,
                                "lustre max must be set");
  utracker_int_t ut = _ut_from_lt(lt, "lustre");
  TEST_ASSERT_NOT_NULL_MESSAGE(ut, "lustre tracker must be created");
  STEP_FUNC(sf, {
                    {-1, 500},
                    {12060, 400},
                    {12240, 300},
                    {12360, 0},
                });
  utracker_int_t expected = _ut_from_step_func(&sf);
  _assert_ut_match(expected, ut, true,
                   "initial ut is same as test_init_lic_tracker_running_jobs2");
  ut_int_destroy(expected);
  /* test overlap on original step function */
  job_precursor_t prec1 = {101, 100, 3000, 10, JOB_PENDING, -1};
  job_record_t *job_ptr1 = _create_job(&prec1, 0);
  remote_estimates_t estimates1 = {0, 600};
  // int rc = backfill_licenses_overlap(lt, job_ptr1, &estimates1, 1000);
  TEST_ASSERT_TRUE_MESSAGE(
      backfill_licenses_overlap(lt, job_ptr1, &estimates1, 10000),
      "job cannot be scheduled");
  TEST_ASSERT_FALSE_MESSAGE(
      backfill_licenses_overlap(lt, job_ptr1, &estimates1, 12060),
      "job can be scheduled");
  TEST_ASSERT_FALSE_MESSAGE(
      backfill_licenses_overlap(lt, job_ptr1, &estimates1, 12160),
      "job can be scheduled");
  job_precursor_t prec2 = {102, 100, 3000, 10, JOB_PENDING, -1};
  job_record_t *job_ptr2 = _create_job(&prec2, 0);
  remote_estimates_t estimates2 = {0, 1200};
  TEST_ASSERT_TRUE_MESSAGE(
      backfill_licenses_overlap(lt, job_ptr2, &estimates2, 12160),
      "job cannot be scheduled");
  TEST_ASSERT_FALSE_MESSAGE(
      backfill_licenses_overlap(lt, job_ptr2, &estimates2, 12360),
      "job can be scheduled");
  /* allocate a job */
  int rc = backfill_licenses_alloc_job(lt, job_ptr2, &estimates2, 12360,
                                   12360 + prec2.time_limit * 60);
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      SLURM_SUCCESS, rc, "reservation with overerequirement can be allocated");
  STEP_FUNC(sf2, {
                     {-1, 500},
                     {12060, 400},
                     {12240, 300},
                     {12360, 1200},
                     {18420, 0},
                 });
  expected = _ut_from_step_func(&sf2);
  _assert_ut_match(
      expected, ut, true,
      "ut after allocation job with timelimit=100min and lustre=1200");
  ut_int_destroy(expected);
  /* test overlap on the modified step function */
  TEST_ASSERT_TRUE_MESSAGE(
      backfill_licenses_overlap(lt, job_ptr1, &estimates1, 10000),
      "job cannot be scheduled");
  TEST_ASSERT_TRUE_MESSAGE(
      backfill_licenses_overlap(lt, job_ptr1, &estimates1, 12060),
      "job cannot be scheduled anymore");
  TEST_ASSERT_TRUE_MESSAGE(
      backfill_licenses_overlap(lt, job_ptr1, &estimates1, 12160),
      "job cannot be scheduled anymore");
  job_ptr1->time_limit = 4;
  TEST_ASSERT_TRUE_MESSAGE(
      backfill_licenses_overlap(lt, job_ptr1, &estimates1, 10000),
      "job cannot be scheduled");
  TEST_ASSERT_FALSE_MESSAGE(
      backfill_licenses_overlap(lt, job_ptr1, &estimates1, 12060),
      "job can be scheduled again");
  TEST_ASSERT_TRUE_MESSAGE(
      backfill_licenses_overlap(lt, job_ptr1, &estimates1, 12160),
      "job cannot be scheduled");
  estimates1.lustre = 0;
  TEST_ASSERT_FALSE_MESSAGE(
      backfill_licenses_overlap(lt, job_ptr1, &estimates1, 10000),
      "job can be scheduled anywhere");
  TEST_ASSERT_FALSE_MESSAGE(
      backfill_licenses_overlap(lt, job_ptr1, &estimates1, 12060),
      "job can be scheduled anywhere");
  TEST_ASSERT_FALSE_MESSAGE(
      backfill_licenses_overlap(lt, job_ptr1, &estimates1, 12160),
      "job can be scheduled anywhere");
  TEST_ASSERT_FALSE_MESSAGE(
      backfill_licenses_overlap(lt, job_ptr1, &estimates1, 15160),
      "job can be scheduled anywhere");
  _job_delete(job_ptr1);
  _job_delete(job_ptr2);
  destroy_lic_tracker(lt);
}

void set_wa() {
  configure_backfill_licenses(BACKFILL_LICENSES_TWO_GROUP);
  configure_total_node_count(100);
}

void test_wa_step_func() {
  set_wa();
  test_step_func();
}
void test_wa_assert_ut_match() {
  set_wa();
  test_assert_ut_match();
}
void test_wa_init_lic_tracker_no_licenses() {
  set_wa();
  test_init_lic_tracker_no_licenses();
}
void test_wa_init_lic_tracker_no_job_list() {
  set_wa();
  test_init_lic_tracker_no_job_list();
}
void test_wa_init_lic_tracker_empty_job_list() {
  set_wa();
  test_init_lic_tracker_empty_job_list();
}
void test_wa_init_lic_tracker_no_running_jobs() {
  set_wa();
  test_init_lic_tracker_no_running_jobs();
}
void test_wa_init_lic_tracker_running_jobs1a() {
  set_wa();
  test_init_lic_tracker_running_jobs1a();
}
void test_wa_init_lic_tracker_running_jobs1b() {
  set_wa();
  test_init_lic_tracker_running_jobs1b();
}
void test_wa_init_lic_tracker_running_jobs2() {
  set_wa();
  test_init_lic_tracker_running_jobs2();
}
void test_wa_backfill_licenses_test_job_1() {
  set_wa();
  test_backfill_licenses_test_job_1();
}
void test_wa_backfill_licenses_test_job_and_alloc_job_estimates() {
  set_wa();
  test_backfill_licenses_test_job_and_alloc_job_estimates();
}
void test_wa_backfill_licenses_test_job_and_alloc_job_presets() {
  set_wa();
  test_backfill_licenses_test_job_and_alloc_job_presets();
}
void test_wa_backfill_licenses_overlap() {
  set_wa();
  test_backfill_licenses_overlap();
}

void test_wa_init_lic_tracker_one_pending_job()
{
  set_wa();
  // TODO: check if needed
  // mock_job_utilization_from_remote = _jobs1b_get_job_utilization_from_remote;
  _create_licenses1();
  _init_job_list();
  // time_t now = time(NULL);
  time_t now = 0;
  job_precursor_t prec = {101, 100, 3000, 10, JOB_PENDING, -1};
  job_record_t *job_ptr = _create_job(&prec, 0);
  _add_job(job_ptr);
  lic_tracker_p lt = init_lic_tracker(60);
  utracker_int_t ut = _ut_from_lt(lt, "lustre");
  TEST_ASSERT_NOT_NULL_MESSAGE(ut, "lustre tracker must be created");
  STEP_FUNC(sf, {
                    {-1, 500},
                });
  utracker_int_t expected = _ut_from_step_func(&sf);
  _assert_ut_match(expected, ut, true, "strict");
  ut_int_destroy(expected);
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 0, _get_r_target(lt), "R_target calculated properly when one pending job");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 0, _get_r_star(lt), "R_star calculated properly when one pending job");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 0, _get_r_bar(lt), "R_bar calculated properly when one pending job");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, _get_r_star_target(lt), "R_star_target calculated properly when one pending job");
  destroy_lic_tracker(lt);
}

void test_wa_init_lic_tracker_one_pending_job_with_lustre()
{
  set_wa();
  // Predictions are used to set the job's lustre
  mock_job_utilization_from_remote = _jobs1b_get_job_utilization_from_remote;
  _create_licenses1();
  _init_job_list();
  // time_t now = time(NULL);
  time_t now = 0;
  job_precursor_t prec = {101, 100, 3000, 10, JOB_PENDING, -1};
  job_record_t *job_ptr = _create_job(&prec, 0);
  _add_job(job_ptr);
  lic_tracker_p lt = init_lic_tracker(60);
  utracker_int_t ut = _ut_from_lt(lt, "lustre");
  TEST_ASSERT_NOT_NULL_MESSAGE(ut, "lustre tracker must be created");
  STEP_FUNC(sf, {
                    {-1, 500},
                });
  utracker_int_t expected = _ut_from_step_func(&sf);
  _assert_ut_match(expected, ut, true, "strict");
  ut_int_destroy(expected);
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 40, _get_r_target(lt), "R_target calculated properly when one pending job");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 40, _get_r_star(lt), "R_star calculated properly when one pending job");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 40, _get_r_bar(lt), "R_bar calculated properly when one pending job");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, _get_r_star_target(lt), "R_star_target calculated properly when one pending job");
  destroy_lic_tracker(lt);
}

void test_wa_backfill_licenses_star1() {
  set_wa();
  _create_licenses1();
  _init_job_list();
  _add_jobs(COUNT_OF(jobs2), jobs2, 0);
  /* no pending jobs */
  lic_tracker_p lt = init_lic_tracker(60);
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 17.0769, _get_r_target(lt), "R_target calculated properly when no pending jobs");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 0, _get_r_star(lt), "R_star calculated properly when no pending jobs");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 0, _get_r_bar(lt), "R_bar calculated properly when no pending jobs");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1000, _get_r_star_target(lt), "R_star_target calculated properly when no pending jobs");
  {
    utracker_int_t st = _get_star_tracker(lt);
    STEP_FUNC(sf, {
                      {-1, 500},
                      {12060, 400},
                      {12240, 300},
                      {12360, 0},
                  });
    utracker_int_t expected = _ut_from_step_func(&sf);
    _assert_ut_match(expected, st, true, "strict");
    ut_int_destroy(expected);
    /* now test backfill licenses */
    /* first job */
    job_precursor_t prec = {101, 100, 3000, 10, JOB_PENDING, -1};
    job_record_t *job_ptr = _create_job(&prec, 0);
    remote_estimates_t estimates = {0, 300};
    time_t when = 10000;
    int rc = backfill_licenses_test_job(lt, job_ptr, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10000, when, "first job can start right away");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    backfill_licenses_alloc_job(lt, job_ptr, &estimates, when, when + 10*60);
    /* second job */
    job_precursor_t prec2 = {102, 100, 3000, 10, JOB_PENDING, -1};
    job_record_t *job_ptr2 = _create_job(&prec2, 0);
    // estimates = {0, 300};
    // when = 10000;
    rc = backfill_licenses_test_job(lt, job_ptr2, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10620, when, "second \"lustre 300\" job can start after job 101");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    estimates.lustre = 600;
    rc = backfill_licenses_test_job(lt, job_ptr2, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(12060, when, "second \"lustre 600\" job can start after job 4");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    _job_delete(job_ptr);
    _job_delete(job_ptr2);
  }
  destroy_lic_tracker(lt);
  /* no pending jobs - bigger lustre limit */
  licenses_t *license = list_peek(license_list);
  license->total = 10000;
  lt = init_lic_tracker(60);
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 17.0769, _get_r_target(lt), "R_target calculated properly when no pending jobs");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 0, _get_r_star(lt), "R_star calculated properly when no pending jobs");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 0, _get_r_bar(lt), "R_bar calculated properly when no pending jobs");
  TEST_ASSERT_EQUAL_INT_MESSAGE(10000, _get_r_star_target(lt), "R_star_target calculated properly when no pending jobs");
  {
    utracker_int_t st = _get_star_tracker(lt);
    STEP_FUNC(sf, {
                      {-1, 500},
                      {12060, 400},
                      {12240, 300},
                      {12360, 0},
                  });
    utracker_int_t expected = _ut_from_step_func(&sf);
    _assert_ut_match(expected, st, true, "strict");
    ut_int_destroy(expected);
    /* now test backfill licenses */
    /* first job */
    job_precursor_t prec = {101, 100, 3000, 10, JOB_PENDING, -1};
    job_record_t *job_ptr = _create_job(&prec, 0);
    remote_estimates_t estimates = {0, 300};
    time_t when = 10000;
    int rc = backfill_licenses_test_job(lt, job_ptr, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10000, when, "first job can start right away");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    backfill_licenses_alloc_job(lt, job_ptr, &estimates, when, when + 10*60);
    /* second job */
    job_precursor_t prec2 = {102, 100, 3000, 10, JOB_PENDING, -1};
    job_record_t *job_ptr2 = _create_job(&prec2, 0);
    // estimates = {0, 300};
    // when = 10000;
    rc = backfill_licenses_test_job(lt, job_ptr2, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10000, when, "with bigger limt, second \"lustre 300\" job can start right away");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    estimates.lustre = 600;
    rc = backfill_licenses_test_job(lt, job_ptr2, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10000, when, "with bigger limt, second \"lustre 600\" job can start right away");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    _job_delete(job_ptr);
    _job_delete(job_ptr2);
  }
  destroy_lic_tracker(lt);
  /* 30 pending jobs */
  _add_jobs_several_times(COUNT_OF(jobs_cycle1), jobs_cycle1, 10, 0, 400, 1000);
  lt = init_lic_tracker(60);
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 16.6809, _get_r_target(lt), "R_target calculated properly with 30 pending jobs");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 10, _get_r_star(lt), "R_star calculated properly with 30 pending jobs");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 10, _get_r_bar(lt), "R_bar calculated properly with 30 pending jobs");
  TEST_ASSERT_EQUAL_INT_MESSAGE(668, _get_r_star_target(lt), "R_star_target calculated properly with 30 pending jobs");
  destroy_lic_tracker(lt);
  /* 300 penidng jobs*/
  _add_jobs_several_times(COUNT_OF(jobs_cycle1), jobs_cycle1, 90, 4000, 400, 1000);
  lt = init_lic_tracker(60);
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 16.6681, _get_r_target(lt), "R_target calculated properly with 300 pending jobs");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 10, _get_r_star(lt), "R_star calculated properly with 300 pending jobs");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 10, _get_r_bar(lt), "R_bar calculated properly with 300 pending jobs");
  TEST_ASSERT_EQUAL_INT_MESSAGE(667, _get_r_star_target(lt), "R_star_target calculated properly with 300 pending jobs");
  {
    utracker_int_t st = _get_star_tracker(lt);
    STEP_FUNC(sf, {
                      {-1, 200},
                      // {12060, 200},
                      // {12240, 200},
                      {12360, 0},
                  });
    utracker_int_t expected = _ut_from_step_func(&sf);
    _assert_ut_match(expected, st, true, "strict");
    ut_int_destroy(expected);
    /* now test backfill licenses */
    /* first job */
    job_precursor_t prec = {101, 100, 3000, 10, JOB_PENDING, -1};
    job_record_t *job_ptr = _create_job(&prec, 0);
    remote_estimates_t estimates = {0, 300};
    time_t when = 10000;
    int rc = backfill_licenses_test_job(lt, job_ptr, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10000, when, "with penidng jobs, first job can start right away");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    backfill_licenses_alloc_job(lt, job_ptr, &estimates, when, when + 10 * 60);
    STEP_FUNC(sf1, {
                      {-1, 200},
                      {9960, 400},
                      {10620, 200},
                      // {12240, 200},
                      {12360, 0},
                  });
    expected = _ut_from_step_func(&sf1);
    _assert_ut_match(expected, st, true, "strict");
    ut_int_destroy(expected);
    /* second job */
    job_precursor_t prec2 = {102, 100, 3000, 10, JOB_PENDING, -1};
    job_record_t *job_ptr2 = _create_job(&prec2, 0);
    // estimates = {0, 300};
    // when = 10000;
    rc = backfill_licenses_test_job(lt, job_ptr2, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10000, when, "with penidng jobs, second \"lustre 300\" job can start right away");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    estimates.lustre = 600;
    rc = backfill_licenses_test_job(lt, job_ptr2, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10000, when, "with penidng jobs, second \"lustre 600\" job can start right away");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    estimates.lustre = 700;
    rc = backfill_licenses_test_job(lt, job_ptr2, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10620, when, "with penidng jobs, second \"lustre 700\" job can start after job 101");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    estimates.lustre = 3000;
    rc = backfill_licenses_test_job(lt, job_ptr2, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10620, when, "with penidng jobs, second \"lustre 3000\" job can start after job 101");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    estimates.lustre = 10000;
    rc = backfill_licenses_test_job(lt, job_ptr2, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(12360, when, "with penidng jobs, second \"lustre 10000\" job can start after all jobs");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    _job_delete(job_ptr);
    _job_delete(job_ptr2);
  }
  destroy_lic_tracker(lt);
}

void test_wa_backfill_licenses_star2()
{
  set_wa();
  mock_job_utilization_from_remote = _jobs2a_get_job_utilization_from_remote;
  _create_licenses1();
  _init_job_list();
  _add_jobs(COUNT_OF(jobs2), jobs2, 0);
  /* no pending jobs */
  lic_tracker_p lt = init_lic_tracker(60);
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 17.0769, _get_r_target(lt), "R_target calculated properly when no pending jobs");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 0, _get_r_star(lt), "R_star calculated properly when no pending jobs");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 0, _get_r_bar(lt), "R_bar calculated properly when no pending jobs");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1000, _get_r_star_target(lt), "R_star_target calculated properly when no pending jobs");
  {
    utracker_int_t st = _get_star_tracker(lt);
    STEP_FUNC(sf, {
                      {-1, 500},
                      {12060, 400},
                      {12240, 300},
                      {12360, 0},
                  });
    utracker_int_t expected = _ut_from_step_func(&sf);
    _assert_ut_match(expected, st, true, "strict");
    ut_int_destroy(expected);
    /* now test backfill licenses */
    /* first job */
    job_precursor_t prec = {101, 100, 3000, 10, JOB_PENDING, -1};
    job_record_t *job_ptr = _create_job(&prec, 0);
    remote_estimates_t estimates = {0, 300};
    time_t when = 10000;
    int rc = backfill_licenses_test_job(lt, job_ptr, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10000, when, "first job can start right away");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    backfill_licenses_alloc_job(lt, job_ptr, &estimates, when, when + 10 * 60);
    /* second job */
    job_precursor_t prec2 = {102, 100, 3000, 10, JOB_PENDING, -1};
    job_record_t *job_ptr2 = _create_job(&prec2, 0);
    // estimates = {0, 300};
    // when = 10000;
    rc = backfill_licenses_test_job(lt, job_ptr2, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10620, when, "second \"lustre 300\" job can start after job 101");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    estimates.lustre = 600;
    rc = backfill_licenses_test_job(lt, job_ptr2, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(12060, when, "second \"lustre 600\" job can start after job 4");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    _job_delete(job_ptr);
    _job_delete(job_ptr2);
  }
  destroy_lic_tracker(lt);
  /* no pending jobs - bigger lustre limit */
  licenses_t *license = list_peek(license_list);
  license->total = 10000;
  lt = init_lic_tracker(60);
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 17.0769, _get_r_target(lt), "R_target calculated properly when no pending jobs");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 0, _get_r_star(lt), "R_star calculated properly when no pending jobs");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 0, _get_r_bar(lt), "R_bar calculated properly when no pending jobs");
  TEST_ASSERT_EQUAL_INT_MESSAGE(10000, _get_r_star_target(lt), "R_star_target calculated properly when no pending jobs");
  {
    utracker_int_t st = _get_star_tracker(lt);
    STEP_FUNC(sf, {
                      {-1, 500},
                      {12060, 400},
                      {12240, 300},
                      {12360, 0},
                  });
    utracker_int_t expected = _ut_from_step_func(&sf);
    _assert_ut_match(expected, st, true, "strict");
    ut_int_destroy(expected);
    /* now test backfill licenses */
    /* first job */
    job_precursor_t prec = {101, 100, 3000, 10, JOB_PENDING, -1};
    job_record_t *job_ptr = _create_job(&prec, 0);
    remote_estimates_t estimates = {0, 300};
    time_t when = 10000;
    int rc = backfill_licenses_test_job(lt, job_ptr, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10000, when, "first job can start right away");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    backfill_licenses_alloc_job(lt, job_ptr, &estimates, when, when + 10 * 60);
    /* second job */
    job_precursor_t prec2 = {102, 100, 3000, 10, JOB_PENDING, -1};
    job_record_t *job_ptr2 = _create_job(&prec2, 0);
    // estimates = {0, 300};
    // when = 10000;
    rc = backfill_licenses_test_job(lt, job_ptr2, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10000, when, "with bigger limt, second \"lustre 300\" job can start right away");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    estimates.lustre = 600;
    rc = backfill_licenses_test_job(lt, job_ptr2, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10000, when, "with bigger limt, second \"lustre 600\" job can start right away");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    _job_delete(job_ptr);
    _job_delete(job_ptr2);
  }
  destroy_lic_tracker(lt);
  /* 30 pending jobs */
  _add_jobs_several_times(COUNT_OF(jobs_cycle1), jobs_cycle1, 10, 0, 400, 1000);
  lt = init_lic_tracker(60);
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 16.7755, _get_r_target(lt), "R_target calculated properly with 30 pending jobs");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 10, _get_r_star(lt), "R_star calculated properly with 30 pending jobs");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 10, _get_r_bar(lt), "R_bar calculated properly with 30 pending jobs");
  TEST_ASSERT_EQUAL_INT_MESSAGE(678, _get_r_star_target(lt), "R_star_target calculated properly with 30 pending jobs");
  destroy_lic_tracker(lt);
  /* 300 penidng jobs*/
  _add_jobs_several_times(COUNT_OF(jobs_cycle1), jobs_cycle1, 90, 4000, 400, 1000);
  lt = init_lic_tracker(60);
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 16.680965, _get_r_target(lt), "R_target calculated properly with 300 pending jobs");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 10, _get_r_star(lt), "R_star calculated properly with 300 pending jobs");
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.0001, 10, _get_r_bar(lt), "R_bar calculated properly with 300 pending jobs");
  TEST_ASSERT_EQUAL_INT_MESSAGE(668, _get_r_star_target(lt), "R_star_target calculated properly with 300 pending jobs");
  {
    utracker_int_t st = _get_star_tracker(lt);
    STEP_FUNC(sf, {
                      {-1, 200},
                      // {12060, 200},
                      // {12240, 200},
                      {12360, 0},
                  });
    utracker_int_t expected = _ut_from_step_func(&sf);
    _assert_ut_match(expected, st, true, "strict");
    ut_int_destroy(expected);
    /* now test backfill licenses */
    /* first job */
    job_precursor_t prec = {101, 100, 3000, 10, JOB_PENDING, -1};
    job_record_t *job_ptr = _create_job(&prec, 0);
    remote_estimates_t estimates = {0, 300};
    time_t when = 10000;
    int rc = backfill_licenses_test_job(lt, job_ptr, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10000, when, "with penidng jobs, first job can start right away");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    backfill_licenses_alloc_job(lt, job_ptr, &estimates, when, when + 10 * 60);
    STEP_FUNC(sf1, {
                       {-1, 200},
                       {9960, 400},
                       {10620, 200},
                       // {12240, 200},
                       {12360, 0},
                   });
    expected = _ut_from_step_func(&sf1);
    _assert_ut_match(expected, st, true, "strict");
    ut_int_destroy(expected);
    /* second job */
    job_precursor_t prec2 = {102, 100, 3000, 10, JOB_PENDING, -1};
    job_record_t *job_ptr2 = _create_job(&prec2, 0);
    // estimates = {0, 300};
    // when = 10000;
    rc = backfill_licenses_test_job(lt, job_ptr2, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10000, when, "with penidng jobs, second \"lustre 300\" job can start right away");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    estimates.lustre = 600;
    rc = backfill_licenses_test_job(lt, job_ptr2, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10000, when, "with penidng jobs, second \"lustre 600\" job can start right away");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    estimates.lustre = 700;
    rc = backfill_licenses_test_job(lt, job_ptr2, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10620, when, "with penidng jobs, second \"lustre 700\" job can start after job 101");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    estimates.lustre = 3000;
    rc = backfill_licenses_test_job(lt, job_ptr2, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10620, when, "with penidng jobs, second \"lustre 3000\" job can start after job 101");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    estimates.lustre = 10000;
    rc = backfill_licenses_test_job(lt, job_ptr2, &estimates, &when);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(12360, when, "with penidng jobs, second \"lustre 10000\" job can start after all jobs");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SLURM_SUCCESS, rc, "job can be scheduled");
    _job_delete(job_ptr);
    _job_delete(job_ptr2);
  }
  destroy_lic_tracker(lt);
}

int main(int argc, char * argv[]) {
  signal(SIGSEGV, handler);  // install our handler
  log_options_t log_options = {LOG_LEVEL_DEBUG5, LOG_LEVEL_QUIET,
                               LOG_LEVEL_QUIET, 1, 0};
  printf("%d\n", log_init("LOG", log_options, SYSLOG_FACILITY_DAEMON, NULL));
  // init mocks;
  if (!avail_node_bitmap) avail_node_bitmap = bit_alloc(8);
  if (!idle_node_bitmap) idle_node_bitmap = bit_alloc(8);
  if (!rs_node_bitmap) rs_node_bitmap = bit_alloc(8);
  UNITY_BEGIN();
  RUN_TEST(test_step_func);
  RUN_TEST(test_assert_ut_match);
  RUN_TEST(test_init_lic_tracker_no_licenses);
  RUN_TEST(test_init_lic_tracker_no_job_list);
  RUN_TEST(test_init_lic_tracker_empty_job_list);
  RUN_TEST(test_init_lic_tracker_no_running_jobs);
  RUN_TEST(test_init_lic_tracker_running_jobs1a);
  RUN_TEST(test_init_lic_tracker_running_jobs1b);
  RUN_TEST(test_init_lic_tracker_running_jobs2);
  RUN_TEST(test_backfill_licenses_test_job_1);
  RUN_TEST(test_backfill_licenses_test_job_and_alloc_job_estimates);
  RUN_TEST(test_backfill_licenses_test_job_and_alloc_job_presets);
  RUN_TEST(test_backfill_licenses_overlap);
  RUN_TEST(test_wa_step_func);
  RUN_TEST(test_wa_assert_ut_match);
  RUN_TEST(test_wa_init_lic_tracker_no_licenses);
  RUN_TEST(test_wa_init_lic_tracker_no_job_list);
  RUN_TEST(test_wa_init_lic_tracker_empty_job_list);
  RUN_TEST(test_wa_init_lic_tracker_no_running_jobs);
  RUN_TEST(test_wa_init_lic_tracker_running_jobs1a);
  RUN_TEST(test_wa_init_lic_tracker_running_jobs1b);
  RUN_TEST(test_wa_init_lic_tracker_running_jobs2);
  RUN_TEST(test_wa_backfill_licenses_test_job_1);
  RUN_TEST(test_wa_backfill_licenses_test_job_and_alloc_job_estimates);
  RUN_TEST(test_wa_backfill_licenses_test_job_and_alloc_job_presets);
  RUN_TEST(test_wa_backfill_licenses_overlap);
  RUN_TEST(test_wa_backfill_licenses_star1);
  RUN_TEST(test_wa_backfill_licenses_star2);
  RUN_TEST(test_wa_init_lic_tracker_one_pending_job);
  RUN_TEST(test_wa_init_lic_tracker_one_pending_job_with_lustre);
  return UNITY_END();
}