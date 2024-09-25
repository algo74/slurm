/*****************************************************************************\
*  backfill_configure.c - part of "Slurm-LDMS" project 
*****************************************************************************
*  Copyright (C) 2024 Alexander Goponenko, University of Central Florida.
*  Distributed with no warranty under the GNU General Public License.
*  See the GNU General Public License for more details.
\*****************************************************************************/
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "backfill_licenses.h"
#include "backfill.h"
#include "cJSON.h"
#include "remote_estimates.h"
#include "src/common/log.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

                                                               static const char *ENVVAR_FILENAME = "VINSNL_CONFIG";
enum { MAX_INT_STRING = ((CHAR_BIT * sizeof(int)) / 3 + 2) };

static time_t last_config_time = 0;
static bool been_read_config = false;

static void _disable_track_nodes() 
{
  backfill_config_allow_node_leeway(false);
  configure_trace_nodes(false);
}

static void _enable_track_nodes() 
{
  backfill_config_allow_node_leeway(true);
  configure_trace_nodes(true);
}

void backfill_configure()
{
  char *filename = getenv(ENVVAR_FILENAME);
  if (!filename) {
    info("%s: Env. variable \"%s\" is not set; VINSNL is not configured", __func__, ENVVAR_FILENAME);
    return;
  }
  struct stat buffer;
  memset(&buffer, 0, sizeof(buffer));
  int rc = stat(filename, &buffer);
  if (rc != 0) {
    info("%s: Cannot access information about the file \"%s\"; VINSNL is not configured", __func__, filename);
    return;
  }
  // file exists or such...
  if (been_read_config && last_config_time == buffer.st_mtime) {
    debug5("%s: Config file \"%s\" is unchanged; VINSNL is not configured", __func__, filename);
    return;
  }
  // either first config or the config file was changed
  been_read_config = true;
  last_config_time = buffer.st_mtime;
  size_t file_size = buffer.st_size;
  char *file_content = xmalloc(file_size + 2);
  if (!file_content) {
    error("%s: cannot allocate %zu bytes to read the config file (%s); VINSNL is not configured", __func__, file_size + 1, filename);
    return;
  }
  memset(file_content, 0, file_size + 2);
  FILE *fp = fopen(filename, "rb");
  if (!fp) {
    error("%s: error %d while opening the config file (%s); VINSNL is not configured", __func__, errno, filename);
    error("error %d is \"%s\"", errno, strerror(errno));
    xfree(file_content);
    return;
  }
  size_t bytes_read = fread(file_content, 1, file_size + 1, fp);
  fclose(fp);
  if (bytes_read > file_size) {
    // TODO: reread the file
    error("%s: file \"%s\" longer than anticipated; VINSNL is not configured", __func__, filename);
    xfree(file_content);
    return;
  }
  // debug5("%s: file \"%s\" read", __func__, filename);
  cJSON *config_json = cJSON_Parse(file_content);
  xfree(file_content);
  if (!config_json) {
    error("%s: cannot understand the file \"%s\"; VINSNL is not configured", __func__, filename);
    return;
  }
  // debug5("%s: JSON %s read", __func__, file_content);
  // configure server address
  cJSON *server = cJSON_GetObjectItem(config_json, "server");
  if (!server) {
    info("%s: Config file \"%s\" missing server settings; server is not configured", __func__, filename);
  } else if (cJSON_IsNull(server)) {
    info("%s: Config file \"%s\": server is disabled", __func__, filename);
    config_vinsnl_server(NULL, NULL);
  } else {
    cJSON *server_name = cJSON_GetObjectItem(server, "name");
    if (server_name == NULL || !cJSON_IsString(server_name) || server_name->valuestring == NULL) {
      error("%s: file \"%s\": server name error; VINSNL is not configured", __func__, filename);
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
        config_vinsnl_server(server_name->valuestring, port_value);
      } else {
        error("%s: file \"%s\": server port error; VINSNL is not configured", __func__, filename);
      }
    }
  }
  // configure backfill options
  cJSON *total_nodes = cJSON_GetObjectItem(config_json, "total_nodes");
  if (!total_nodes) {
    info("%s: Config file \"%s\" missing total nodes: not configured", __func__, filename);
    // configure_total_node_count(-1);
  } else if (cJSON_IsNull(total_nodes)) {
    info("%s: Config file \"%s\" total nodes is NULL: reset to default", __func__, filename);
    configure_total_node_count(-1);
  } else if (!cJSON_IsNumber(total_nodes)) {
    error("%s: Config file \"%s\": total nodes must be a number: not configured", __func__, filename);
  } else {
    info("%s: Config file \"%s\": setting total nodes to %d", __func__, filename, total_nodes->valueint);
    configure_total_node_count(total_nodes->valueint);
  }
  cJSON *backfill_type = cJSON_GetObjectItem(config_json, "backfill_type");
  if (!backfill_type) {
    info("%s: Config file \"%s\" missing backfill type: not configured", __func__, filename);
    // configure_backfill_licenses(BACKFILL_LICENSES_AWARE);
  } else if (cJSON_IsNull(backfill_type)) {
    info("%s: Config file \"%s\" backfill type is NULL: reset to default", __func__, filename);
    configure_backfill_licenses(BACKFILL_LICENSES_AWARE);
  } else if (!cJSON_IsString(backfill_type)) {
    error("%s: Config file \"%s\": backfill type must be a string", __func__, filename);
  } else {
    for (char *p = backfill_type->valuestring; *p; ++p) *p = tolower(*p);
    if (0 == xstrcmp(backfill_type->valuestring, "aware")) {
      info("%s: Config file \"%s\": setting backfill type to \"AWARE\"", __func__, filename);
      configure_backfill_licenses(BACKFILL_LICENSES_AWARE);
    } else if (0 == xstrcmp(backfill_type->valuestring, "two_group")) {
      info("%s: Config file \"%s\": setting backfill type to \"TWO_GROUP\"", __func__, filename);
      configure_backfill_licenses(BACKFILL_LICENSES_TWO_GROUP);
    } else {
      error("%s: Config file \"%s\": unknown backfill type (%s): not configured", __func__, filename, backfill_type->valuestring);
    }
  }
  // configure Lustre log file name
  char *log_filename_string = NULL;
  cJSON *log_filename = cJSON_GetObjectItem(config_json, "lustre_log_path");
  if (log_filename && cJSON_IsString(log_filename)) {
    log_filename_string = xstrdup(log_filename->valuestring);
  }
  char *old_string = configure_lustre_log_filename(log_filename_string);
  info("%s: Config file \"%s\": changing Lustre log path from \"%s\" to \"%s\"", __func__, filename, old_string, log_filename_string);
  if (old_string) xfree(old_string);

  // configure node tracking
  cJSON *track_nodes = cJSON_GetObjectItem(config_json, "track_nodes");
  if (!track_nodes) {
    info("%s: Config file \"%s\" missing track nodes option: disabled", __func__, filename);
    _disable_track_nodes();
  } else if (cJSON_IsBool(track_nodes)) {
    if (cJSON_IsTrue(track_nodes)) {
      info("%s: Config file \"%s\" enabling node track mode", __func__, filename);
      _enable_track_nodes();
    } else {
      info("%s: Config file \"%s\" disabling node track mode", __func__, filename);
      _disable_track_nodes();
    }
  } else {
    info("%s: Config file \"%s\" track nodes option not understood: disabled", __func__, filename);
    _enable_track_nodes();
  }

  cJSON_Delete(config_json);
}