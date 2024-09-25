/*****************************************************************************\
 *  lustre_util_configure.c - part of "Slurm-LDMS" project 
 *****************************************************************************
 *  Copyright (C) 2024 Alexander Goponenko, University of Central Florida.
 *
 *  Distributed with no warranty under the GNU General Public License.
 *  See the GNU General Public License for more details.
\*****************************************************************************/
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "src/common/log.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

static const char *ENVVAR_FILENAME = "VINSNL_CONFIG";
// static const size_t MAX_INT_STRING = ((CHAR_BIT * sizeof(int)) / 3 + 2);
enum { MAX_INT_STRING = ((CHAR_BIT * sizeof(int)) / 3 + 2) };

static pthread_rwlock_t config_lock = PTHREAD_RWLOCK_INITIALIZER;
static time_t last_config_time = 0;
static bool been_read_config = false;
static char *server_name = NULL;
static char *server_port = NULL;

static bool _config_vnlsnl_server(time_t reference_time, time_t config_time, char *server, char *port)
{
  bool been_updated = false;
  slurm_rwlock_wrlock(&config_lock);
  if (reference_time == last_config_time) {
    last_config_time = config_time;
    been_read_config = true;
    if (server_name) xfree(server_name);
    if (server_port) xfree(server_port);
    server_name = server ? xstrdup(server) : NULL;
    server_port = port ? xstrdup(port) : NULL;
    been_updated = true;
  } else {
    info("%s: reference time (%zu) and last_config_time mistmatch (%zu)", __func__, reference_time, config_time);
  }
  slurm_rwlock_unlock(&config_lock);
  return been_updated;
}

void lustre_util_configure()
{
  char *filename = getenv(ENVVAR_FILENAME);
  if (!filename) {
    info("%s: Env. variable \"%s\" is not set; VINSNL is not configured", __func__, ENVVAR_FILENAME);
    return;
  }
  struct stat file_stat;
  bool done_updating = false;
  while (!done_updating) {
    memset(&file_stat, 0, sizeof(file_stat));
    int rc = stat(filename, &file_stat);
    if (rc != 0) {
      info("%s: Cannot access information about the file \"%s\"; VINSNL is not configured", __func__, filename);
      return;
    }
    // file exists or such...
    slurm_rwlock_rdlock(&config_lock);
    time_t reference_time = last_config_time;
    slurm_rwlock_unlock(&config_lock);
    if (been_read_config && reference_time == file_stat.st_mtime) {
      debug5("%s: Config file \"%s\" is unchanged; VINSNL is not configured", __func__, filename);
      return;
    }
    // either first config or the config file was changed
    size_t file_size = file_stat.st_size;
    char *file_content = xmalloc(file_size + 2);
    if (!file_content) {
      error("%s: cannot allocate %zu bytes to read the config file (%s); VINSNL is not configured", __func__, file_size + 1, filename);
      return;
    }
    memset(file_content, 0, file_size + 2);
    FILE *fp = fopen(filename, "rb");
    size_t bytes_read = fread(file_content, 1, file_size + 1, fp);
    fclose(fp);
    if (bytes_read > file_size) {
      info("%s: file \"%s\" longer than anticipated; restarting", __func__, filename);
      xfree(file_content);
      done_updating = false;
      continue;
    }
    // the configuration file has been read
    cJSON *config_json = cJSON_Parse(file_content);
    xfree(file_content);
    if (!config_json) {
      error("%s: cannot understand the file \"%s\"; VINSNL is not configured", __func__, filename);
      return;
    }
    // configuration JSON has been read
    // configure server address
    cJSON *server = cJSON_GetObjectItem(config_json, "server");
    if (!server) {
      info("%s: Config file \"%s\" missing server settings; server is not configured", __func__, filename);
      done_updating = true;
    } else if (cJSON_IsNull(server)) {
      info("%s: Config file \"%s\": server is disabled", __func__, filename);
      if (_config_vnlsnl_server(reference_time, file_stat.st_mtime, NULL, NULL)) {
        done_updating = true;
      }
    } else {
      cJSON *server_name = cJSON_GetObjectItem(server, "name");
      if (server_name == NULL || !cJSON_IsString(server_name) || server_name->valuestring == NULL) {
        error("%s: file \"%s\": server name error; VINSNL is not configured", __func__, filename);
        done_updating = true;
      } else {
        // server name is good; checking the port
        bool been_good = true;
        char *port_value = NULL;
        char port_buffer[MAX_INT_STRING + 1] = {0};
        cJSON *server_port = cJSON_GetObjectItem(server, "port");
        if (server_port == NULL) {
          error("%s: file \"%s\": server port empty", __func__, filename);
          been_good = false;
        } else if (cJSON_IsString(server_port)) {
          if (server_port->valuestring == NULL) {
            error("%s: file \"%s\": server port empty string", __func__, filename);
            been_good = false;
          } else {
            port_value = server_port->valuestring;
          }
        } else if (cJSON_IsNumber(server_port)) {
          if (server_port->valueint <= 0) {
            error("%s: file \"%s\": server port <= 0: %d", __func__, filename, server_port->valueint);
            been_good = false;
          } else {
            size_t char_printed = snprintf(port_buffer, MAX_INT_STRING, "%d", server_port->valueint);
            if (char_printed > MAX_INT_STRING) {
              error("%s: file \"%s\": int to string convertion too long for server port: %d", __func__, filename, server_port->valueint);
              been_good = false;
            } else {
              port_value = port_buffer;
            }
          }
        } else {
          been_good = false;
        }
        if (been_good) {
          info("%s: Config file \"%s\": setting server to %s:%s", __func__, filename, server_name->valuestring, port_value);
          done_updating = _config_vnlsnl_server(reference_time, file_stat.st_mtime, server_name->valuestring, port_value);
        } else {
          error("%s: file \"%s\": server port error; VINSNL is not configured", __func__, filename);
          done_updating = true;
        }
      }
    }
    cJSON_Delete(config_json);
  }
}

void get_server_address(char **name, char **port)
{
  slurm_rwlock_rdlock(&config_lock);
  *name = xstrdup(server_name);
  *port = xstrdup(server_port);
  slurm_rwlock_unlock(&config_lock);
}

void update_and_get_server_address(char **name, char **port)
{
  lustre_util_configure();
  get_server_address(name, port);
}