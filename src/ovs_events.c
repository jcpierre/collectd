/**
 * collectd - src/ovs_events.c
 *
 * Copyright(c) 2016 Intel Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *   Volodymyr Mytnyk <volodymyrx.mytnyk@intel.com>
 **/

#include "common.h" /* auxiliary functions */

#include "utils_ovs.h" /* OVS helpers */

#define OVS_EVENTS_IFACE_NAME_SIZE 128
#define OVS_EVENTS_IFACE_UUID_SIZE 64
#define OVS_EVENTS_EXT_IFACE_ID_SIZE 64
#define OVS_EVENTS_EXT_VM_UUID_SIZE 64
#define OVS_EVENTS_OVS_DB_URL_SIZE 64
#define OVS_EVENTS_PLUGIN "ovs_events"
#define OVS_EVENTS_CTX_LOCK                                                    \
  for (int __i = ovs_events_ctx_lock(); __i != 0; __i = ovs_events_ctx_unlock())
#define OVS_EVENTS_CONFIG_ERROR(option)                                        \
  do {                                                                         \
    ERROR(OVS_EVENTS_PLUGIN ": read '%s' config option failed", option);       \
    goto failure;                                                              \
  } while (0)

/* Link status type */
enum ovs_events_link_status_e { DOWN, UP };
typedef enum ovs_events_link_status_e ovs_events_link_status_t;

/* Interface info */
struct ovs_events_iface_info_s {
  char name[OVS_EVENTS_IFACE_NAME_SIZE];           /* interface name */
  char uuid[OVS_EVENTS_IFACE_UUID_SIZE];           /* interface UUID */
  char ext_iface_id[OVS_EVENTS_EXT_IFACE_ID_SIZE]; /* external interface id */
  char ext_vm_uuid[OVS_EVENTS_EXT_VM_UUID_SIZE];   /* external VM UUID */
  ovs_events_link_status_t link_status;            /* interface link status */
  struct ovs_events_iface_info_s *next;            /* next interface info */
};
typedef struct ovs_events_iface_info_s ovs_events_iface_info_t;

/* Interface list */
struct ovs_events_iface_list_s {
  char name[OVS_EVENTS_IFACE_NAME_SIZE]; /* interface name */
  struct ovs_events_iface_list_s *next;  /* next interface info */
};
typedef struct ovs_events_iface_list_s ovs_events_iface_list_t;

/* OVS events configuration data */
struct ovs_events_config_s {
  _Bool send_notification; /* sent notification to collectd? */
  char ovs_db_server_url[OVS_EVENTS_OVS_DB_URL_SIZE]; /* OVS DB server URL */
  ovs_events_iface_list_t *ifaces;                    /* interface info */
};
typedef struct ovs_events_config_s ovs_events_config_t;

/* OVS events context type */
struct ovs_events_ctx_s {
  pthread_mutex_t mutex;      /* mutex to lock the context */
  ovs_db_t *ovs_db;           /* pointer to OVS DB instance */
  ovs_events_config_t config; /* plugin config */
  char *ovs_db_select_params; /* OVS DB select parameter request */
  _Bool is_db_available;      /* specify whether OVS DB is available */
};
typedef struct ovs_events_ctx_s ovs_events_ctx_t;

/*
 * Private variables
 */
static ovs_events_ctx_t ovs_events_ctx = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .config = {.send_notification = 0, /* do not send notification */
               .ovs_db_server_url =
                   "tcp:127.0.0.1:6640", /* use default OVS DB URL */
               .ifaces = NULL},
    .ovs_db_select_params = NULL,
    .is_db_available = 0,
    .ovs_db = NULL};

/* This function is used only by "OVS_EVENTS_CTX_LOCK" define (see above).
 * It always returns 1 when context is locked.
 */
static inline int ovs_events_ctx_lock() {
  pthread_mutex_lock(&ovs_events_ctx.mutex);
  return (1);
}

/* This function is used only by "OVS_EVENTS_CTX_LOCK" define (see above).
 * It always returns 0 when context is unlocked.
 */
static inline int ovs_events_ctx_unlock() {
  pthread_mutex_unlock(&ovs_events_ctx.mutex);
  return (0);
}

/* Check if given interface name exists in configuration file. It
 * returns 1 if exists otherwise 0. If no interfaces are configured,
 * -1 is returned
 */
static int ovs_events_config_iface_exists(const char *ifname) {
  if (ovs_events_ctx.config.ifaces == NULL)
    return -1;

  /* check if given interface exists */
  for (ovs_events_iface_list_t *iface = ovs_events_ctx.config.ifaces; iface;
       iface = iface->next)
    return (strcmp(ifname, iface->name) == 0);

  return 0;
}

/* Get OVS DB select parameter request based on rfc7047,
 * "Transact" & "Select" section
 */
static inline char *ovs_events_get_select_params() {
  int ret = 0;
  size_t buff_size = 0;
  size_t offset = 0;
  char *buff = NULL;
  char *new_buff = NULL;
  const char params_fmt[] = "[\"Open_vSwitch\"%s]";
  const char option_fmt[] = ",{\"op\":\"select\",\"table\":\"Interface\","
                            "\"where\":[[\"name\",\"==\",\"%s\"]],"
                            "\"columns\":[\"link_state\",\"external_ids\","
                            "\"name\",\"_uuid\"]}";
  const char default_opt[] = ",{\"op\":\"select\",\"table\":\"Interface\","
                             "\"where\":[],\"columns\":[\"link_state\","
                             "\"external_ids\",\"name\",\"_uuid\"]}";
  /* setup OVS DB interface condition */
  for (ovs_events_iface_list_t *iface = ovs_events_ctx.config.ifaces; iface;
       iface = iface->next, offset += ret) {
    /* allocate new buffer (format size + ifname len is good enough) */
    buff_size += (sizeof(option_fmt) + strlen(iface->name));
    new_buff = realloc(buff, buff_size);
    if (new_buff == NULL)
      goto failure;
    buff = new_buff;
    ret = ssnprintf(buff + offset, buff_size, option_fmt, iface->name);
    if (ret < 0)
      goto failure;
  }
  /* if no interfaces are configured, use default params */
  if (buff == NULL) {
    buff = strdup(default_opt);
    offset = strlen(default_opt);
  }

  /* allocate memory for OVS DB select params */
  buff_size = offset + sizeof(params_fmt);
  new_buff = malloc(buff_size);
  if (new_buff == NULL)
    goto failure;

  /* create OVS DB select params */
  if (ssnprintf(new_buff, buff_size, params_fmt, buff) < 0)
    goto failure;

  sfree(buff);
  return new_buff;

failure:
  sfree(new_buff);
  sfree(buff);
  return NULL;
}

/* Release memory allocated for configuration data */
static void ovs_events_config_free() {
  ovs_events_iface_list_t *del_iface = NULL;
  sfree(ovs_events_ctx.ovs_db_select_params);
  while (ovs_events_ctx.config.ifaces) {
    del_iface = ovs_events_ctx.config.ifaces;
    ovs_events_ctx.config.ifaces = ovs_events_ctx.config.ifaces->next;
    sfree(del_iface);
  }
}

/* Parse plugin configuration file and store the config
 * in allocated memory. Returns negative value in case of error.
 */
static int ovs_events_plugin_config(oconfig_item_t *ci) {
  ovs_events_iface_list_t *new_iface;

  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;
    if (strcasecmp("SendNotification", child->key) == 0) {
      if (cf_util_get_boolean(child, &ovs_events_ctx.config.send_notification) <
          0)
        OVS_EVENTS_CONFIG_ERROR(child->key);
    } else if (strcasecmp("OvsDbServerUrl", child->key) == 0) {
      if (cf_util_get_string_buffer(
              child, ovs_events_ctx.config.ovs_db_server_url,
              sizeof(ovs_events_ctx.config.ovs_db_server_url)) < 0)
        OVS_EVENTS_CONFIG_ERROR(child->key);
    } else if (strcasecmp("Interfaces", child->key) == 0) {
      for (int j = 0; j < child->values_num; j++) {
        /* check value type */
        if (child->values[j].type != OCONFIG_TYPE_STRING) {
          ERROR(OVS_EVENTS_PLUGIN
                ": given interface name is not a string [idx=%d]",
                j);
          goto failure;
        }
        /* allocate memory for configured interface */
        if ((new_iface = malloc(sizeof(*new_iface))) == NULL) {
          ERROR(OVS_EVENTS_PLUGIN ": malloc () copy interface name fail");
          goto failure;
        } else {
          /* store interface name */
          sstrncpy(new_iface->name, child->values[j].value.string,
                   sizeof(new_iface->name));
          new_iface->next = ovs_events_ctx.config.ifaces;
          ovs_events_ctx.config.ifaces = new_iface;
          DEBUG(OVS_EVENTS_PLUGIN ": found monitored interface \"%s\"",
                new_iface->name);
        }
      }
    } else {
      ERROR(OVS_EVENTS_PLUGIN ": option '%s' is not allowed here", child->key);
      goto failure;
    }
  }
  return (0);

failure:
  ovs_events_config_free();
  return (-1);
}

/* Dispatch OVS interface link status event to collectd */
static void
ovs_events_dispatch_notification(const ovs_events_iface_info_t *ifinfo) {
  const char *msg_link_status = NULL;
  notification_t n = {
      NOTIF_FAILURE, cdtime(), "", "", OVS_EVENTS_PLUGIN, "", "", "", NULL};

  /* convert link status to message string */
  switch (ifinfo->link_status) {
  case UP:
    msg_link_status = "UP";
    n.severity = NOTIF_OKAY;
    break;
  case DOWN:
    msg_link_status = "DOWN";
    n.severity = NOTIF_WARNING;
    break;
  default:
    ERROR(OVS_EVENTS_PLUGIN ": unknown interface link status");
    return;
  }

  /* add interface metadata to the notification */
  if (plugin_notification_meta_add_string(&n, "uuid", ifinfo->uuid) < 0) {
    ERROR(OVS_EVENTS_PLUGIN ": add interface uuid meta data failed");
    return;
  }

  if (strlen(ifinfo->ext_vm_uuid) > 0)
    if (plugin_notification_meta_add_string(&n, "vm-uuid",
                                            ifinfo->ext_vm_uuid) < 0) {
      ERROR(OVS_EVENTS_PLUGIN ": add interface vm-uuid meta data failed");
      return;
    }

  if (strlen(ifinfo->ext_iface_id) > 0)
    if (plugin_notification_meta_add_string(&n, "iface-id",
                                            ifinfo->ext_iface_id) < 0) {
      ERROR(OVS_EVENTS_PLUGIN ": add interface iface-id meta data failed");
      return;
    }

  /* fill the notification data */
  ssnprintf(n.message, sizeof(n.message),
            "link state of \"%s\" interface has been changed to \"%s\"",
            ifinfo->name, msg_link_status);
  sstrncpy(n.host, hostname_g, sizeof(n.host));
  sstrncpy(n.plugin_instance, ifinfo->name, sizeof(n.plugin_instance));
  sstrncpy(n.type, "gauge", sizeof(n.type));
  sstrncpy(n.type_instance, "link_status", sizeof(n.type_instance));
  plugin_dispatch_notification(&n);
}

/* Dispatch OVS interface link status value to collectd */
static void
ovs_events_link_status_submit(const ovs_events_iface_info_t *ifinfo) {
  value_t values[1];
  value_list_t vl = VALUE_LIST_INIT;
  meta_data_t *meta = NULL;

  /* add interface metadata to the submit value */
  if ((meta = meta_data_create()) != NULL) {
    if (meta_data_add_string(meta, "uuid", ifinfo->uuid) < 0)
      ERROR(OVS_EVENTS_PLUGIN ": add interface uuid meta data failed");

    if (strlen(ifinfo->ext_vm_uuid) > 0)
      if (meta_data_add_string(meta, "vm-uuid", ifinfo->ext_vm_uuid) < 0)
        ERROR(OVS_EVENTS_PLUGIN ": add interface vm-uuid meta data failed");

    if (strlen(ifinfo->ext_iface_id) > 0)
      if (meta_data_add_string(meta, "iface-id", ifinfo->ext_iface_id) < 0)
        ERROR(OVS_EVENTS_PLUGIN ": add interface iface-id meta data failed");
    vl.meta = meta;
  } else
    ERROR(OVS_EVENTS_PLUGIN ": create metadata failed");

  values[0].gauge = (gauge_t)ifinfo->link_status;
  vl.time = cdtime();
  vl.values = values;
  vl.values_len = STATIC_ARRAY_SIZE(values);
  sstrncpy(vl.host, hostname_g, sizeof(vl.host));
  sstrncpy(vl.plugin, OVS_EVENTS_PLUGIN, sizeof(vl.plugin));
  sstrncpy(vl.plugin_instance, ifinfo->name, sizeof(vl.plugin_instance));
  sstrncpy(vl.type, "gauge", sizeof(vl.type));
  sstrncpy(vl.type_instance, "link_status", sizeof(vl.type_instance));
  plugin_dispatch_values(&vl);
  meta_data_destroy(meta);
}

/* Dispatch OVS DB terminate connection event to collectd */
static void ovs_events_dispatch_terminate_notification(const char *msg) {
  notification_t n = {
      NOTIF_FAILURE, cdtime(), "", "", OVS_EVENTS_PLUGIN, "", "", "", NULL};
  sstrncpy(n.message, msg, sizeof(n.message));
  sstrncpy(n.host, hostname_g, sizeof(n.host));
  plugin_dispatch_notification(&n);
}

/* Get OVS DB interface information and stores it into
 * ovs_events_iface_info_t structure */
static int ovs_events_get_iface_info(yajl_val jobject,
                                     ovs_events_iface_info_t *ifinfo) {
  yajl_val jexternal_ids = NULL;
  yajl_val jvalue = NULL;
  yajl_val juuid = NULL;
  const char *state = NULL;

  /* check YAJL type */
  if (!YAJL_IS_OBJECT(jobject))
    return (-1);

  /* try to find external_ids, name and link_state fields */
  jexternal_ids = ovs_utils_get_value_by_key(jobject, "external_ids");
  if (jexternal_ids == NULL || ifinfo == NULL)
    return (-1);

  /* get iface-id from external_ids field */
  jvalue = ovs_utils_get_map_value(jexternal_ids, "iface-id");
  if (jvalue != NULL && YAJL_IS_STRING(jvalue))
    sstrncpy(ifinfo->ext_iface_id, YAJL_GET_STRING(jvalue),
             sizeof(ifinfo->ext_iface_id));

  /* get vm-uuid from external_ids field */
  jvalue = ovs_utils_get_map_value(jexternal_ids, "vm-uuid");
  if (jvalue != NULL && YAJL_IS_STRING(jvalue))
    sstrncpy(ifinfo->ext_vm_uuid, YAJL_GET_STRING(jvalue),
             sizeof(ifinfo->ext_vm_uuid));

  /* get interface uuid */
  jvalue = ovs_utils_get_value_by_key(jobject, "_uuid");
  if (jvalue == NULL || !YAJL_IS_ARRAY(jvalue) ||
      YAJL_GET_ARRAY(jvalue)->len != 2)
    return (-1);
  juuid = YAJL_GET_ARRAY(jvalue)->values[1];
  if (juuid == NULL || !YAJL_IS_STRING(juuid))
    return (-1);
  sstrncpy(ifinfo->uuid, YAJL_GET_STRING(juuid), sizeof(ifinfo->uuid));

  /* get interface name */
  jvalue = ovs_utils_get_value_by_key(jobject, "name");
  if (jvalue == NULL || !YAJL_IS_STRING(jvalue))
    return (-1);
  sstrncpy(ifinfo->name, YAJL_GET_STRING(jvalue), sizeof(ifinfo->name));

  /* get OVS DB interface link status */
  jvalue = ovs_utils_get_value_by_key(jobject, "link_state");
  if (jvalue != NULL && ((state = YAJL_GET_STRING(jvalue)) != NULL)) {
    /* convert OVS table link state to link status */
    if (strcmp(state, "up") == 0)
      ifinfo->link_status = UP;
    else if (strcmp(state, "down") == 0)
      ifinfo->link_status = DOWN;
  }
  return (0);
}

/* Process OVS DB update table event. It handles link status update event(s)
 * and dispatches the value(s) to collectd if interface name matches one of
 * interfaces specified in configuration file.
 */
static void ovs_events_table_update_cb(yajl_val jupdates) {
  yajl_val jnew_val = NULL;
  yajl_val jupdate = NULL;
  yajl_val jrow_update = NULL;
  ovs_events_iface_info_t ifinfo;

  /* JSON "Interface" table update example:
   * ---------------------------------
   * {"Interface":
   *  {
   *   "9adf1db2-29ca-4140-ab22-ae347a4484de":
   *    {
   *     "new":
   *      {
   *       "name":"br0",
   *       "link_state":"up"
   *      },
   *     "old":
   *      {
   *       "link_state":"down"
   *      }
   *    }
   *  }
   * }
   */
  if (!YAJL_IS_OBJECT(jupdates) || !(YAJL_GET_OBJECT(jupdates)->len > 0)) {
    ERROR(OVS_EVENTS_PLUGIN ": unexpected OVS DB update event received");
    return;
  }
  /* verify if this is a table event */
  jupdate = YAJL_GET_OBJECT(jupdates)->values[0];
  if (!YAJL_IS_OBJECT(jupdate)) {
    ERROR(OVS_EVENTS_PLUGIN ": unexpected table update event received");
    return;
  }
  /* go through all row updates  */
  for (int row_index = 0; row_index < YAJL_GET_OBJECT(jupdate)->len;
       ++row_index) {
    jrow_update = YAJL_GET_OBJECT(jupdate)->values[row_index];

    /* check row update */
    jnew_val = ovs_utils_get_value_by_key(jrow_update, "new");
    if (jnew_val == NULL) {
      ERROR(OVS_EVENTS_PLUGIN ": unexpected row update received");
      return;
    }
    /* get OVS DB interface information */
    if (ovs_events_get_iface_info(jnew_val, &ifinfo) < 0) {
      ERROR(OVS_EVENTS_PLUGIN
            " :unexpected interface information data received");
      return;
    }
    if (ovs_events_config_iface_exists(ifinfo.name) != 0)
      /* dispatch notification */
      ovs_events_dispatch_notification(&ifinfo);
  }
}

/* OVD DB reply callback. It parses reply, receives
 * interface information and dispatches the info to
 * collecd
 */
static void ovs_events_poll_result_cb(yajl_val jresult, yajl_val jerror) {
  yajl_val *jvalues = NULL;
  yajl_val jvalue = NULL;
  ovs_events_iface_info_t ifinfo;

  if (!YAJL_IS_NULL(jerror)) {
    ERROR(OVS_EVENTS_PLUGIN "error received by OVS DB server");
    return;
  }

  /* result should be an array */
  if (!YAJL_IS_ARRAY(jresult)) {
    ERROR(OVS_EVENTS_PLUGIN "invalid data (array is expected)");
    return;
  }

  /* go through all rows and get interface info */
  jvalues = YAJL_GET_ARRAY(jresult)->values;
  for (int i = 0; i < YAJL_GET_ARRAY(jresult)->len; i++) {
    jvalue = ovs_utils_get_value_by_key(jvalues[i], "rows");
    if (jvalue == NULL || !YAJL_IS_ARRAY(jvalue)) {
      ERROR(OVS_EVENTS_PLUGIN "invalid data (array of rows is expected)");
      return;
    }
    /* get interfaces info */
    for (int j = 0; j < YAJL_GET_ARRAY(jvalue)->len; j++) {
      memset(&ifinfo, 0, sizeof(ifinfo));
      if (ovs_events_get_iface_info(YAJL_GET_ARRAY(jvalue)->values[j],
                                    &ifinfo) < 0) {
        ERROR(OVS_EVENTS_PLUGIN
              "unexpected interface information data received");
        return;
      }
      DEBUG("name=%s, uuid=%s, ext_iface_id=%s, ext_vm_uuid=%s", ifinfo.name,
            ifinfo.uuid, ifinfo.ext_iface_id, ifinfo.ext_vm_uuid);
      ovs_events_link_status_submit(&ifinfo);
    }
  }
}

/* Setup OVS DB table callback. It subscribes to OVS DB 'Interface' table
 * to receive link status event(s).
 */
static void ovs_events_conn_initialize(ovs_db_t *pdb) {
  int ret = 0;
  const char tb_name[] = "Interface";
  const char *columns[] = {"_uuid", "external_ids", "name", "link_state", NULL};

  /* register update link status event if needed */
  if (ovs_events_ctx.config.send_notification) {
    ret = ovs_db_table_cb_register(pdb, tb_name, columns,
                                   ovs_events_table_update_cb, NULL,
                                   OVS_DB_TABLE_CB_FLAG_MODIFY);
    if (ret < 0) {
      ERROR(OVS_EVENTS_PLUGIN ": register OVS DB update callback failed");
      return;
    }
  }
  OVS_EVENTS_CTX_LOCK { ovs_events_ctx.is_db_available = 1; }
  DEBUG(OVS_EVENTS_PLUGIN ": OVS DB has been initialized");
}

/* OVS DB terminate connection notification callback */
static void ovs_events_conn_terminate() {
  const char msg[] = "OVS DB connection has been lost";
  if (ovs_events_ctx.config.send_notification)
    ovs_events_dispatch_terminate_notification(msg);
  WARNING(OVS_EVENTS_PLUGIN ": %s", msg);
  OVS_EVENTS_CTX_LOCK { ovs_events_ctx.is_db_available = 0; }
}

/* Read OVS DB interface link status callback */
static int ovs_events_plugin_read(user_data_t *ud) {
  (void)ud; /* unused argument */
  _Bool is_connected = 0;
  OVS_EVENTS_CTX_LOCK { is_connected = ovs_events_ctx.is_db_available; }
  if (is_connected)
    if (ovs_db_send_request(ovs_events_ctx.ovs_db, "transact",
                            ovs_events_ctx.ovs_db_select_params,
                            ovs_events_poll_result_cb) < 0) {
      ERROR(OVS_EVENTS_PLUGIN ": get interface info failed");
      return (-1);
    }
  return (0);
}

/* Initialize OVS plugin */
static int ovs_events_plugin_init(void) {
  ovs_db_t *ovs_db = NULL;
  ovs_db_callback_t cb = {.post_conn_init = ovs_events_conn_initialize,
                          .post_conn_terminate = ovs_events_conn_terminate};

  DEBUG(OVS_EVENTS_PLUGIN ": OVS DB url = %s",
        ovs_events_ctx.config.ovs_db_server_url);

  /* generate OVS DB select condition based on list on configured interfaces */
  ovs_events_ctx.ovs_db_select_params = ovs_events_get_select_params();
  if (ovs_events_ctx.ovs_db_select_params == NULL) {
    ERROR(OVS_EVENTS_PLUGIN ": fail to get OVS DB select condition");
    goto ovs_events_failure;
  }

  /* initialize OVS DB */
  ovs_db = ovs_db_init(ovs_events_ctx.config.ovs_db_server_url, &cb);
  if (ovs_db == NULL) {
    ERROR(OVS_EVENTS_PLUGIN ": fail to connect to OVS DB server");
    goto ovs_events_failure;
  }

  /* store OVS DB handler */
  OVS_EVENTS_CTX_LOCK { ovs_events_ctx.ovs_db = ovs_db; }

  DEBUG(OVS_EVENTS_PLUGIN ": plugin has been initialized");
  return (0);

ovs_events_failure:
  ERROR(OVS_EVENTS_PLUGIN ": plugin initialize failed");
  /* release allocated memory */
  ovs_events_config_free();
  return (-1);
}

/* Shutdown OVS plugin */
static int ovs_events_plugin_shutdown(void) {
  /* destroy OVS DB */
  if (ovs_db_destroy(ovs_events_ctx.ovs_db))
    ERROR(OVS_EVENTS_PLUGIN ": OVSDB object destroy failed");

  /* release memory allocated for config */
  ovs_events_config_free();

  DEBUG(OVS_EVENTS_PLUGIN ": plugin has been destroyed");
  return (0);
}

/* Register OVS plugin callbacks */
void module_register(void) {
  plugin_register_complex_config(OVS_EVENTS_PLUGIN, ovs_events_plugin_config);
  plugin_register_init(OVS_EVENTS_PLUGIN, ovs_events_plugin_init);
  plugin_register_complex_read(NULL, OVS_EVENTS_PLUGIN, ovs_events_plugin_read,
                               0, NULL);
  plugin_register_shutdown(OVS_EVENTS_PLUGIN, ovs_events_plugin_shutdown);
}
