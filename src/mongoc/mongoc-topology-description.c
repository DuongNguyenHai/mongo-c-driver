/*
 * Copyright 2014 MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mongoc-array-private.h"
#include "mongoc-error.h"
#include "mongoc-server-description-private.h"
#include "mongoc-topology-description-private.h"
#include "mongoc-trace.h"

static void
_mongoc_topology_server_dtor (void *server_,
                              void *ctx_)
{
   mongoc_server_description_destroy ((mongoc_server_description_t *)server_);
}

/*
 *--------------------------------------------------------------------------
 *
 * mongoc_topology_description_init --
 *
 *       Initialize the given topology description
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */
void
mongoc_topology_description_init (mongoc_topology_description_t     *description,
                                  mongoc_topology_description_type_t type,
                                  mongoc_topology_cb_t              *cb)
{
   ENTRY;

   bson_return_if_fail (description);
   bson_return_if_fail (type == MONGOC_TOPOLOGY_UNKNOWN ||
                        type == MONGOC_TOPOLOGY_SINGLE ||
                        type == MONGOC_TOPOLOGY_RS_NO_PRIMARY);

   memset (description, 0, sizeof (*description));

   description->type = type;
   description->servers = mongoc_set_new(8, _mongoc_topology_server_dtor, NULL);
   description->set_name = NULL;
   description->compatible = true;
   description->compatibility_error = NULL;
   description->stale = true;

   if (cb) {
      memcpy (&description->cb, cb, sizeof (*cb));
   }

   EXIT;
}

/*
 *--------------------------------------------------------------------------
 *
 * mongoc_topology_description_destroy --
 *
 *       Destroy allocated resources within @description
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */
void
mongoc_topology_description_destroy (mongoc_topology_description_t *description)
{
   ENTRY;

   BSON_ASSERT(description);

   mongoc_set_destroy(description->servers);

   if (description->set_name) {
      bson_free (description->set_name);
   }

   if (description->compatibility_error) {
      bson_free (description->compatibility_error);
   }

   EXIT;
}

/* find the primary, then stop iterating */
static bool
_mongoc_topology_description_has_primary_cb (void *item,
                                             void *ctx /* OUT */)
{
   mongoc_server_description_t *server = item;
   mongoc_server_description_t **primary = ctx;

   /* TODO should this include MONGOS? */
   if (server->type == MONGOC_SERVER_RS_PRIMARY || server->type == MONGOC_SERVER_STANDALONE) {
      *primary = item;
      return false;
   }
   return true;
}

/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_topology_description_has_primary --
 *
 *       If topology has a primary, return it.
 *
 * Returns:
 *       A pointer to the primary, or NULL.
 *
 * Side effects:
 *       None
 *
 *--------------------------------------------------------------------------
 */
static mongoc_server_description_t *
_mongoc_topology_description_has_primary (mongoc_topology_description_t *description)
{
   mongoc_server_description_t *primary = NULL;

   mongoc_set_for_each(description->servers, _mongoc_topology_description_has_primary_cb, &primary);

   return primary;
}

static bool
_mongoc_topology_description_server_is_candidate (
   mongoc_server_description_type_t   desc_type,
   mongoc_read_mode_t                 read_mode,
   mongoc_topology_description_type_t topology_type)
{
   switch ((int)topology_type) {
   case MONGOC_TOPOLOGY_SINGLE:
      switch ((int)desc_type) {
      case MONGOC_SERVER_STANDALONE:
         return true;
      default:
         return false;
      }

   case MONGOC_TOPOLOGY_RS_NO_PRIMARY:
   case MONGOC_TOPOLOGY_RS_WITH_PRIMARY:
      switch ((int)read_mode) {
      case MONGOC_READ_PRIMARY:
         switch ((int)desc_type) {
         case MONGOC_SERVER_POSSIBLE_PRIMARY:
         case MONGOC_SERVER_RS_PRIMARY:
            return true;
         default:
            return false;
         }
      case MONGOC_READ_SECONDARY:
         switch ((int)desc_type) {
         case MONGOC_SERVER_RS_SECONDARY:
            return true;
         default:
            return false;
         }
      default:
         switch ((int)desc_type) {
         case MONGOC_SERVER_POSSIBLE_PRIMARY:
         case MONGOC_SERVER_RS_PRIMARY:
         case MONGOC_SERVER_RS_SECONDARY:
            return true;
         default:
            return false;
         }
      }

   case MONGOC_TOPOLOGY_SHARDED:
      switch ((int)desc_type) {
      case MONGOC_SERVER_MONGOS:
         return true;
      default:
         return false;
      }
   default:
      return false;
   }
}

typedef struct _mongoc_suitable_data_t
{
   mongoc_read_mode_t                 read_mode;
   mongoc_topology_description_type_t topology_type;
   mongoc_server_description_t       *primary; /* OUT */
   mongoc_server_description_t      **candidates; /* OUT */
   size_t                             candidates_len; /* OUT */
   bool                               has_secondary; /* OUT */
} mongoc_suitable_data_t;

static bool
_mongoc_replica_set_read_suitable_cb (void *item,
                                      void *ctx)
{
   mongoc_server_description_t *server = item;
   mongoc_suitable_data_t *data = ctx;

   if (_mongoc_topology_description_server_is_candidate (server->type,
                                                         data->read_mode,
                                                         data->topology_type)) {

      if (server->type == MONGOC_SERVER_RS_PRIMARY) {
         data->primary = server;

         if (data->read_mode == MONGOC_READ_PRIMARY ||
             data->read_mode == MONGOC_READ_PRIMARY_PREFERRED) {
            /* we want a primary and we have one, done! */
            return false;
         }
      }

      if (server->type == MONGOC_SERVER_RS_SECONDARY) {
         data->has_secondary = true;
      }

      /* add to our candidates */
      data->candidates[data->candidates_len++] = server;
   }
   return true;
}

/* if any mongos are candidates, add them to the candidates array */
static bool
_mongoc_find_suitable_mongos_cb (void *item,
                                 void *ctx)
{
   mongoc_server_description_t *server = item;
   mongoc_suitable_data_t *data = ctx;

   if (_mongoc_topology_description_server_is_candidate (server->type,
                                                         data->read_mode,
                                                         data->topology_type)) {
      data->candidates[data->candidates_len++] = server;
   }
   return true;
}

/*
 *-------------------------------------------------------------------------
 *
 * mongoc_topology_description_suitable_servers --
 *
 *       Return an array of suitable server descriptions for this
 *       operation and read preference.
 *
 *       NOTE: this method should only be called while holding the mutex on
 *       the owning topology object.
 *
 * Returns:
 *       Array of server descriptions, or NULL upon failure.
 *
 * Side effects:
 *       None.
 *
 *-------------------------------------------------------------------------
 */

void
mongoc_topology_description_suitable_servers (
   mongoc_array_t                *set, /* OUT */
   mongoc_ss_optype_t             optype,
   mongoc_topology_description_t *topology,
   const mongoc_read_prefs_t     *read_pref,
   size_t                         local_threshold_ms)
{
   mongoc_suitable_data_t data;
   mongoc_server_description_t **candidates;
   mongoc_server_description_t *server;
   int64_t nearest = -1;
   int i;
   mongoc_read_mode_t read_mode = mongoc_read_prefs_get_mode(read_pref);

   candidates = bson_malloc0(sizeof(*candidates) * topology->servers->items_len);

   data.read_mode = read_mode;
   data.topology_type = topology->type;
   data.primary = NULL;
   data.candidates = candidates;
   data.candidates_len = 0;
   data.has_secondary = false;

   /* Single server --
    * Either it is suitable or it isn't */
   if (topology->type == MONGOC_TOPOLOGY_SINGLE) {
      server = topology->servers->items[0].item;
      if (_mongoc_topology_description_server_is_candidate (server->type, read_mode, topology->type)) {
         _mongoc_array_append_val (set, server);
      }
      goto DONE;
   }

   /* Replica sets --
    * Find suitable servers based on read mode */
   if (topology->type == MONGOC_TOPOLOGY_RS_NO_PRIMARY ||
       topology->type == MONGOC_TOPOLOGY_RS_WITH_PRIMARY) {

      if (optype == MONGOC_SS_READ) {

         mongoc_set_for_each(topology->servers, _mongoc_replica_set_read_suitable_cb, &data);

         /* if we have a primary, it's a candidate, for some read modes we are done */
         if (read_mode == MONGOC_READ_PRIMARY || read_mode == MONGOC_READ_PRIMARY_PREFERRED) {
            if (data.primary) {
               _mongoc_array_append_val (set, data.primary);
               goto DONE;
            }
         }

         if (! mongoc_server_description_filter_eligible (data.candidates, data.candidates_len, read_pref)) {
            if (read_mode == MONGOC_READ_NEAREST) {
               goto DONE;
            } else {
               data.has_secondary = false;
            }
         }

         if (data.has_secondary &&
             (read_mode == MONGOC_READ_SECONDARY || read_mode == MONGOC_READ_SECONDARY_PREFERRED)) {
            /* secondary or secondary preferred and we have one. */

            for (i = 0; i < data.candidates_len; i++) {
               if (candidates[i] && candidates[i]->type == MONGOC_SERVER_RS_PRIMARY) {
                  candidates[i] = NULL;
               }
            }
         } else if (read_mode == MONGOC_READ_SECONDARY_PREFERRED && data.primary) {
            /* secondary preferred, but only the one primary is a candidate */
            _mongoc_array_append_val (set, data.primary);
            goto DONE;
         }

      } else if (topology->type == MONGOC_TOPOLOGY_RS_WITH_PRIMARY) {
         /* includes optype == MONGOC_SS_WRITE as the exclusion of the above if */
         mongoc_set_for_each(topology->servers, _mongoc_topology_description_has_primary_cb,
                             &data.primary);
         if (data.primary) {
            _mongoc_array_append_val (set, data.primary);
            goto DONE;
         }
      }
   }

   /* Sharded clusters --
    * All candidates in the latency window are suitable */
   if (topology->type == MONGOC_TOPOLOGY_SHARDED) {
      mongoc_set_for_each (topology->servers, _mongoc_find_suitable_mongos_cb, &data);
   }

   /* Ways to get here:
    *   - secondary read
    *   - secondary preferred read
    *   - primary_preferred and no primary read
    *   - sharded anything
    * Find the nearest, then select within the window */

   for (i = 0; i < data.candidates_len; i++) {
      if (candidates[i] && (nearest == -1 || nearest > candidates[i]->round_trip_time)) {
         nearest = candidates[i]->round_trip_time;
      }
   }

   for (i = 0; i < data.candidates_len; i++) {
      if (candidates[i] && (candidates[i]->round_trip_time <= nearest + local_threshold_ms)) {
         _mongoc_array_append_val (set, candidates[i]);
      }
   }

DONE:

   bson_free (candidates);

   return;
}


/*
 *-------------------------------------------------------------------------
 *
 * mongoc_topology_description_select --
 *
 *      Return a server description of a node that is appropriate for
 *      the given read preference and operation type.
 *
 *      NOTE: this method simply attempts to select a server from the
 *      current topology, it does not retry or trigger topology checks.
 *
 *      NOTE: this method should only be called while holding the mutex on
 *      the owning topology object.
 *
 * Returns:
 *      Selected server description, or NULL upon failure.
 *
 * Side effects:
 *      None.
 *
 *-------------------------------------------------------------------------
 */

mongoc_server_description_t *
mongoc_topology_description_select (mongoc_topology_description_t *topology,
                                    mongoc_ss_optype_t             optype,
                                    const mongoc_read_prefs_t     *read_pref,
                                    int64_t                        local_threshold_ms,
                                    bson_error_t                  *error) /* OUT */
{
   mongoc_array_t suitable_servers;
   mongoc_server_description_t *sd = NULL;

   ENTRY;

   if (!topology->compatible) {
      /* TODO, should we return an error object here,
         or just treat as a case where there are no suitable servers? */
      RETURN(NULL);
   }

   _mongoc_array_init(&suitable_servers, sizeof(mongoc_server_description_t *));

   mongoc_topology_description_suitable_servers(&suitable_servers, optype,
                                                 topology, read_pref, local_threshold_ms);
   if (suitable_servers.len != 0) {
      sd = _mongoc_array_index(&suitable_servers, mongoc_server_description_t*,
                               rand() % suitable_servers.len);
   }

   _mongoc_array_destroy (&suitable_servers);

   RETURN(sd);
}

/*
 *--------------------------------------------------------------------------
 *
 * mongoc_topology_description_server_by_id --
 *
 *       Get the server description for @id, if that server is present
 *       in @description. Otherwise, return NULL.
 *
 *       NOTE: In most cases, caller should create a duplicate of the
 *       returned server description. Caller should hold the mutex on the
 *       owning topology object while calling this method and while using
 *       the returned reference.
 *
 * Returns:
 *       A mongoc_server_description_t *, or NULL.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

mongoc_server_description_t *
mongoc_topology_description_server_by_id (mongoc_topology_description_t *description,
                                          uint32_t                       id)
{
   bson_return_val_if_fail (description, NULL);

   return mongoc_set_get(description->servers, id);
}

/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_topology_description_remove_server --
 *
 *       If present, remove this server from this topology description.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       Removes server from topology's list of servers.
 *
 *--------------------------------------------------------------------------
 */
static void
_mongoc_topology_description_remove_server (mongoc_topology_description_t *description,
                                            mongoc_server_description_t   *server)
{
   bson_return_if_fail (description);
   bson_return_if_fail (server);

   if (description->cb.rm) {
      description->cb.rm(server);
   }

   mongoc_set_rm(description->servers, server->id);
}

typedef struct _mongoc_address_and_id_t {
   const char *address; /* IN */
   bool found; /* OUT */
   uint32_t id; /* OUT */
} mongoc_address_and_id_t;

/* find the given server and stop iterating */
static bool
_mongoc_topology_description_has_server_cb (void *item,
                                            void *ctx /* IN - OUT */)
{
   mongoc_server_description_t *server = item;
   mongoc_address_and_id_t *data = ctx;

   if (strcmp (data->address, server->connection_address) == 0) {
      data->found = true;
      data->id = server->id;
      return false;
   }
   return true;
}

/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_topology_description_topology_has_server --
 *
 *       Return true if @server is in @topology. If so, place its id in
 *       @id if given.
 *
 * Returns:
 *       True if server is in topology, false otherwise.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */
static bool
_mongoc_topology_description_has_server (mongoc_topology_description_t *description,
                                         const char                    *address,
                                         uint32_t                      *id /* OUT */)
{
   mongoc_address_and_id_t data;

   bson_return_val_if_fail (description, 0);
   bson_return_val_if_fail (address, 0);

   data.address = address;
   data.found = false;
   mongoc_set_for_each (description->servers, _mongoc_topology_description_has_server_cb, &data);

   if (data.found && id) {
      *id = data.id;
   }

   return data.found;
}

typedef struct _mongoc_address_and_type_t
{
   const char *address;
   mongoc_server_description_type_t type;
} mongoc_address_and_type_t;

static bool
_mongoc_label_unknown_member_cb (void *item,
                                 void *ctx)
{
   mongoc_server_description_t *server = item;
   mongoc_address_and_type_t *data = ctx;

   if (strcmp (server->connection_address, data->address) == 0 &&
       server->type == MONGOC_SERVER_UNKNOWN) {
      mongoc_server_description_set_state(server, data->type);
      return false;
   }
   return true;
}

/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_topology_description_label_unknown_member --
 *
 *       Find the server description with the given @address and if its
 *       type is UNKNOWN, set its type to @type.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */
static void
_mongoc_topology_description_label_unknown_member (mongoc_topology_description_t *description,
                                                   const char *address,
                                                   mongoc_server_description_type_t type)
{
   mongoc_address_and_type_t data;

   bson_return_if_fail (description);
   bson_return_if_fail (address);

   data.type = type;
   data.address = address;

   mongoc_set_for_each(description->servers, _mongoc_label_unknown_member_cb, &data);
}

/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_topology_description_set_state --
 *
 *       Change the state of this cluster and unblock things waiting
 *       on a change of topology type.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       Unblocks anything waiting on this description to change states.
 *
 *--------------------------------------------------------------------------
 */
static void
_mongoc_topology_description_set_state (mongoc_topology_description_t *description,
                                        mongoc_topology_description_type_t type)
{
   description->type = type;
}

/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_topology_description_check_if_has_primary --
 *
 *       If there is a primary in topology, set topology
 *       type to RS_WITH_PRIMARY, otherwise set it to
 *       RS_NO_PRIMARY.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       Changes the topology type.
 *
 *--------------------------------------------------------------------------
 */
static void
_mongoc_topology_description_check_if_has_primary (mongoc_topology_description_t *topology,
                                                   mongoc_server_description_t   *server)
{
   if (_mongoc_topology_description_has_primary(topology)) {
      _mongoc_topology_description_set_state(topology, MONGOC_TOPOLOGY_RS_WITH_PRIMARY);
   }
   else {
      _mongoc_topology_description_set_state(topology, MONGOC_TOPOLOGY_RS_NO_PRIMARY);
   }
}

/*
 *--------------------------------------------------------------------------
 *
 * mongoc_topology_description_invalidate_server --
 *
 *      Invalidate a server if a network error occurred while using it in
 *      another part of the client. Server description is set to type
 *      UNKNOWN and other parameters are reset to defaults.
 *
 *      NOTE: this method should only be called while holding the mutex on
 *      the owning topology object.
 *
 *--------------------------------------------------------------------------
 */
void
mongoc_topology_description_invalidate_server (mongoc_topology_description_t *topology,
                                               uint32_t                       id)
{
   mongoc_server_description_t *sd;
   bson_error_t error;

   sd = mongoc_topology_description_server_by_id (topology, id);
   mongoc_topology_description_handle_ismaster(topology, sd, NULL, 0, &error);

   return;
}

/*
 *--------------------------------------------------------------------------
 *
 * mongoc_topology_description_add_server --
 *
 *       Add the specified server to the cluster topology if it is not
 *       already a member. If @id, place its id in @id.
 *
 *       NOTE: this method should only be called while holding the mutex on
 *       the owning topology object.
 *
 * Return:
 *       True if the server was added or already existed in the topology,
 *       false if an error occurred.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */
bool
mongoc_topology_description_add_server (mongoc_topology_description_t *topology,
                                        const char                    *server,
                                        uint32_t                      *id /* OUT */)
{
   uint32_t server_id;
   mongoc_server_description_t *description;

   bson_return_val_if_fail (topology, false);
   bson_return_val_if_fail (server, false);

   if (!_mongoc_topology_description_has_server(topology, server, &server_id)){

      /* TODO this might not be an accurate count in all cases */
      server_id = ++topology->max_server_id;

      description = bson_malloc0(sizeof *description);
      mongoc_server_description_init(description, server, server_id);

      mongoc_set_add(topology->servers, server_id, description);

      if (topology->cb.add) {
         topology->cb.add(description);
      }
   }

   if (id) {
      *id = server_id;
   }

   return true;
}


static void
_mongoc_topology_description_monitor_new_servers (mongoc_topology_description_t *topology,
                                                  mongoc_server_description_t *server)
{
   bson_iter_t member_iter;
   const bson_t *rs_members[3];
   int i;

   rs_members[0] = &server->hosts;
   rs_members[1] = &server->arbiters;
   rs_members[2] = &server->passives;

   for (i = 0; i < 3; i++) {
      bson_iter_init (&member_iter, rs_members[i]);

      while (bson_iter_next (&member_iter)) {
         mongoc_topology_description_add_server(topology, bson_iter_utf8(&member_iter, NULL), NULL);
      }
   }
}

typedef struct _mongoc_primary_and_topology_t {
   mongoc_topology_description_t *topology;
   mongoc_server_description_t *primary;
} mongoc_primary_and_topology_t;

/* invalidate old primaries */
static bool
_mongoc_topology_description_invalidate_primaries_cb (void *item,
                                                      void *ctx)
{
   mongoc_server_description_t *server = item;
   mongoc_primary_and_topology_t *data = ctx;

   if (server->id != data->primary->id &&
       server->type == MONGOC_SERVER_RS_PRIMARY) {
      mongoc_server_description_set_state(server, MONGOC_SERVER_UNKNOWN);
   }
   return true;
}

/* Stop monitoring any servers primary doesn't know about */
static bool
_mongoc_topology_description_remove_missing_hosts_cb (void *item,
                                                      void *ctx)
{
   mongoc_server_description_t *server = item;
   mongoc_primary_and_topology_t *data = ctx;

   if (strcmp(data->primary->connection_address, server->connection_address) != 0 &&
       !mongoc_server_description_has_rs_member(data->primary, server->connection_address)) {
      _mongoc_topology_description_remove_server(data->topology, server);
   }
   return true;
}

/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_update_rs_from_primary --
 *
 *       First, determine that this is really the primary:
 *          -If this node isn't in the cluster, do nothing.
 *          -If the cluster's set name is null, set it to node's set name.
 *           Otherwise if the cluster's set name is different from node's,
 *           we found a rogue primary, so remove it from the cluster and
 *           check the cluster for a primary, then return.
 *          -If any of the members of cluster reports an address different
 *           from node's, node cannot be the primary.
 *       Now that we know this is the primary:
 *          -If any hosts, passives, or arbiters in node's description aren't
 *           in the cluster, add them as UNKNOWN servers and begin monitoring.
 *          -If the cluster has any servers that aren't in node's description
 *           remove them and stop monitoring.
 *       Finally, check the cluster for the new primary.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       Changes to the cluster, possible removal of cluster nodes.
 *
 *--------------------------------------------------------------------------
 */
static void
_mongoc_topology_description_update_rs_from_primary (mongoc_topology_description_t *topology,
                                                     mongoc_server_description_t   *server)
{
   mongoc_primary_and_topology_t data;

   bson_return_if_fail (topology);
   bson_return_if_fail (server);

   if (!_mongoc_topology_description_has_server(topology, server->connection_address, NULL)) return;

   /*
    * 'Server' can only be the primary if it has the right rs name.
    */

   if (!topology->set_name && server->set_name) {
      topology->set_name = bson_strdup (server->set_name);
   }
   else if (strcmp(topology->set_name, server->set_name) != 0) {
      _mongoc_topology_description_remove_server(topology, server);
      _mongoc_topology_description_check_if_has_primary(topology, server);
      return;
   }

   /* 'Server' is the primary! Invalidate other primaries if found */
   data.primary = server;
   data.topology = topology;
   mongoc_set_for_each(topology->servers, _mongoc_topology_description_invalidate_primaries_cb, &data);

   /* Begin monitoring any new servers primary knows about */
   _mongoc_topology_description_monitor_new_servers(topology, server);

   /* Stop monitoring any servers primary doesn't know about */
   mongoc_set_for_each(topology->servers, _mongoc_topology_description_remove_missing_hosts_cb, &data);

   /* Finally, set topology type */
   topology->type = MONGOC_TOPOLOGY_RS_WITH_PRIMARY;
}

/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_topology_description_update_rs_without_primary --
 *
 *       Update cluster's information when there is no primary.
 *
 * Returns:
 *       None.
 *
 * Side Effects:
 *       Alters cluster state, may remove node from cluster.
 *
 *--------------------------------------------------------------------------
 */
static void
_mongoc_topology_description_update_rs_without_primary (mongoc_topology_description_t *topology,
                                                        mongoc_server_description_t   *server)
{
   bson_return_if_fail (topology);
   bson_return_if_fail (server);

   if (!_mongoc_topology_description_has_server(topology, server->connection_address, NULL)) {
      return;
   }

   /* make sure we're talking about the same replica set */
   if (server->set_name) {
      if (!topology->set_name) {
         topology->set_name = bson_strdup(server->set_name);
      }
      else if (strcmp(topology->set_name, server->set_name)!= 0) {
         _mongoc_topology_description_remove_server(topology, server);
         return;
      }
   }

   /* Begin monitoring any new servers that this server knows about */
   _mongoc_topology_description_monitor_new_servers(topology, server);

   /* If this server thinks there is a primary, label it POSSIBLE_PRIMARY */
   if (server->current_primary) {
      _mongoc_topology_description_label_unknown_member(topology,
                                                        server->current_primary,
                                                        MONGOC_SERVER_POSSIBLE_PRIMARY);
   }
}

/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_topology_description_update_rs_with_primary_from_member --
 *
 *       Update cluster's information when there is a primary, but the
 *       update is coming from another replica set member.
 *
 * Returns:
 *       None.
 *
 * Side Effects:
 *       Alters cluster state.
 *
 *--------------------------------------------------------------------------
 */
static void
_mongoc_topology_description_update_rs_with_primary_from_member (mongoc_topology_description_t *topology,
                                                                 mongoc_server_description_t   *server)
{
   bson_return_if_fail (topology);
   bson_return_if_fail (server);

   if (!_mongoc_topology_description_has_server(topology, server->connection_address, NULL)) {
      return;
   }

   /* set_name should never be null here */
   if (strcmp(topology->set_name, server->set_name) != 0) {
      _mongoc_topology_description_remove_server(topology, server);
   }

   /* If there is no primary, label server's current_primary as the POSSIBLE_PRIMARY */
   if (!_mongoc_topology_description_has_primary(topology) && server->current_primary) {
      _mongoc_topology_description_set_state(topology, MONGOC_TOPOLOGY_RS_NO_PRIMARY);
      _mongoc_topology_description_label_unknown_member(topology,
                                                        server->current_primary,
                                                        MONGOC_SERVER_POSSIBLE_PRIMARY);
   }
}

/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_topology_description_set_topology_type_to_sharded --
 *
 *       Sets topology's type to SHARDED.
 *
 * Returns:
 *       None
 *
 * Side effects:
 *       Alter's topology's type
 *
 *--------------------------------------------------------------------------
 */
static void
_mongoc_topology_description_set_topology_type_to_sharded (mongoc_topology_description_t *topology,
                                                           mongoc_server_description_t   *server)
{
   _mongoc_topology_description_set_state(topology, MONGOC_TOPOLOGY_SHARDED);
}

/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_topology_description_transition_unknown_to_rs_no_primary --
 *
 *       Encapsulates transition from cluster state UNKNOWN to
 *       RS_NO_PRIMARY. Sets the type to RS_NO_PRIMARY,
 *       then updates the replica set accordingly.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       Changes topology state.
 *
 *--------------------------------------------------------------------------
 */
static void
_mongoc_topology_description_transition_unknown_to_rs_no_primary (mongoc_topology_description_t *topology,
                                                                  mongoc_server_description_t   *server)
{
   _mongoc_topology_description_set_state(topology, MONGOC_TOPOLOGY_RS_NO_PRIMARY);
   _mongoc_topology_description_update_rs_without_primary(topology, server);
}

/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_topology_description_remove_and_check_primary --
 *
 *       Remove this server from being monitored, then check that the
 *       current topology has a primary.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       Stop monitoring server.
 *
 *--------------------------------------------------------------------------
 */
static void
_mongoc_topology_description_remove_and_check_primary (mongoc_topology_description_t *topology,
                                                       mongoc_server_description_t   *server)
{
   _mongoc_topology_description_remove_server(topology, server);
   _mongoc_topology_description_check_if_has_primary(topology, server);
}

/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_topology_description_update_unknown_with_standalone --
 *
 *       If the cluster doesn't contain this server, do nothing.
 *       Otherwise, if the topology only has one seed, change its
 *       type to SINGLE. If the topology has multiple seeds, it does not
 *       include us, so remove this server and stop monitoring us.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       Changes the topology type, might remove server from monitor.
 *
 *--------------------------------------------------------------------------
 */
static void
_mongoc_topology_description_update_unknown_with_standalone (mongoc_topology_description_t *topology,
                                                             mongoc_server_description_t   *server)
{
   bson_return_if_fail (topology);
   bson_return_if_fail (server);

   if (!_mongoc_topology_description_has_server(topology, server->connection_address, NULL)) return;

   if (topology->servers->items_len > 1) {
      /* This cluster contains other servers, it cannot be a standalone. */
      _mongoc_topology_description_remove_server(topology, server);
   } else {
      _mongoc_topology_description_set_state(topology, MONGOC_TOPOLOGY_SINGLE);
   }
}

/*
 *--------------------------------------------------------------------------
 *
 *  This table implements the 'ToplogyType' table outlined in the Server
 *  Discovery and Monitoring spec. Each row represents a server type,
 *  and each column represents the topology type. Given a current topology
 *  type T and a newly-observed server type S, use the function at
 *  state_transions[S][T] to transition to a new state.
 *
 *  Rows should be read like so:
 *  { server type for this row
 *     UNKNOWN,
 *     SHARDED,
 *     RS_NO_PRIMARY,
 *     RS_WITH_PRIMARY
 *  }
 *
 *--------------------------------------------------------------------------
 */

typedef void (*transition_t)(mongoc_topology_description_t *topology,
                             mongoc_server_description_t   *server);

transition_t
gSDAMTransitionTable[MONGOC_SERVER_DESCRIPTION_TYPES][MONGOC_TOPOLOGY_DESCRIPTION_TYPES] = {
   { /* UNKNOWN */
      NULL, /* MONGOC_TOPOLOGY_UNKNOWN */
      NULL, /* MONGOC_TOPOLOGY_SHARDED */
      NULL, /* MONGOC_TOPOLOGY_RS_NO_PRIMARY */
      _mongoc_topology_description_check_if_has_primary /* MONGOC_TOPOLOGY_RS_WITH_PRIMARY */
   },
   { /* STANDALONE */
      _mongoc_topology_description_update_unknown_with_standalone,
      _mongoc_topology_description_remove_server,
      _mongoc_topology_description_remove_server,
      _mongoc_topology_description_remove_and_check_primary
   },
   { /* MONGOS */
      _mongoc_topology_description_set_topology_type_to_sharded,
      NULL,
      _mongoc_topology_description_remove_server,
      _mongoc_topology_description_remove_and_check_primary
   },
   { /* POSSIBLE_PRIMARY */
      NULL,
      NULL,
      NULL,
      NULL
   },
   { /* PRIMARY */
      _mongoc_topology_description_update_rs_from_primary,
      _mongoc_topology_description_remove_server,
      _mongoc_topology_description_update_rs_from_primary,
      _mongoc_topology_description_update_rs_from_primary
   },
   { /* SECONDARY */
      _mongoc_topology_description_transition_unknown_to_rs_no_primary,
      _mongoc_topology_description_remove_server,
      _mongoc_topology_description_update_rs_without_primary,
      _mongoc_topology_description_update_rs_with_primary_from_member
   },
   { /* ARBITER */
      _mongoc_topology_description_transition_unknown_to_rs_no_primary,
      _mongoc_topology_description_remove_server,
      _mongoc_topology_description_update_rs_without_primary,
      _mongoc_topology_description_update_rs_with_primary_from_member
   },
   { /* RS_OTHER */
      _mongoc_topology_description_transition_unknown_to_rs_no_primary,
      _mongoc_topology_description_remove_server,
      _mongoc_topology_description_update_rs_without_primary,
      _mongoc_topology_description_update_rs_with_primary_from_member
   },
   { /* RS_GHOST */
      NULL,
      _mongoc_topology_description_remove_server,
      NULL,
      _mongoc_topology_description_check_if_has_primary
   }
};

/*
 *--------------------------------------------------------------------------
 *
 * mongoc_topology_description_handle_ismaster --
 *
 *      Handle an ismaster. This is called by the background SDAM process,
 *      and by client when invalidating servers.
 *
 *      NOTE: this method should only be called while holding the mutex on
 *      the owning topology object.
 *
 *--------------------------------------------------------------------------
 */

bool
mongoc_topology_description_handle_ismaster (
   mongoc_topology_description_t *topology,
   mongoc_server_description_t   *sd,
   const bson_t                  *ismaster_response,
   int64_t                        rtt_msec,
   bson_error_t                  *error)
{
   bson_return_val_if_fail (topology, false);
   bson_return_val_if_fail (sd, false);

   if (!_mongoc_topology_description_has_server(topology, sd->connection_address, NULL)) {
      return false;
   }

   mongoc_server_description_handle_ismaster (sd, ismaster_response, rtt_msec, error);

   if (gSDAMTransitionTable[sd->type][topology->type]) {
      gSDAMTransitionTable[sd->type][topology->type](topology, sd);
   }

   return true;
}