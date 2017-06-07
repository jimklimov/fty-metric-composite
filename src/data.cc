/*  =========================================================================
    data - composite metrics data structure

    Copyright (C) 2014 - 2017 Eaton

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
    =========================================================================
*/

/*
@header
    data - composite metrics data structure, holds information about known
           assets, configures sensors, stores information about metrics
           that would be produced by the actual configuration.
@discuss
@end
*/

#include "fty_metric_composite_classes.h"
#include <set>
#include <set>
#include <string>

//  Structure of our class
struct _data_t {
    // Information about all interesting assets for this agent
    zhashx_t *all_assets; // asset_name -> its message definition. Owns messages
    // Structure of sensors
    zhashx_t *last_configuration; // asset_name -> list of sensors (each sensor is represented as message). Doesn't own messages
    bool is_reconfig_needed; // indicates, if recently added asset can change configuration
    std::set<std::string> produced_metrics; // list of metrics, that are now produced by composite_metric
    std::map <std::string, std::string> devmap;
    char* ipc_name;
};

//  --------------------------------------------------------------------------
//  Create a new data

data_t *
data_new (void)
{
    data_t *self = (data_t *) zmalloc (sizeof (data_t));
    if ( self ) {
        self->produced_metrics = {};
        self->last_configuration = zhashx_new ();
        if ( self->last_configuration ) {
            self->all_assets = zhashx_new ();
        }
        if ( self->all_assets ) {
            zhashx_set_destructor (self->last_configuration, (zhashx_destructor_fn *) zlistx_destroy);
            zhashx_set_destructor (self->all_assets, (zhashx_destructor_fn *) fty_proto_destroy);
            self->is_reconfig_needed = false;
        }
        else {
            data_destroy (&self);
        }
    }

    // copy and paste from fty-sensor-env
    // maps symbolic name to real device name
    self->devmap = {
        {"/dev/ttySTH1", "/dev/ttyS9"},
        {"/dev/ttySTH2", "/dev/ttyS10"},
        {"/dev/ttySTH3", "/dev/ttyS11"},
        {"/dev/ttySTH4", "/dev/ttyS12"}
    };

    for (auto &it: self->devmap) {
        char *patha = realpath (it.first.c_str (), NULL);
        if (!patha) {
            zsys_warning ("Can't get realpath of %s, using %s: %s", it.first.c_str (), it.second.c_str (), strerror (errno));
            zstr_free (&patha);
            continue;
        }
        std::string npath {patha};
        self->devmap [it.first] = npath;
        zstr_free (&patha);
    }

    return self;
}

void
data_set_ipc (data_t *self, const std::string& name) {
    zstr_free (&self->ipc_name);
    self->ipc_name = strdup (name.c_str ());
}

const char*
data_get_ipc (data_t *self) {
    return (const char*) self->ipc_name;
}


//  --------------------------------------------------------------------------
//  Get a list of sensors assigned to the specified asset, if for this asset
//  there is no any record, creates empty one and returns it.

static zlistx_t*
s_get_assigned_sensors (data_t *self, const char *asset_name)
{
    zlistx_t *already_assigned_sensors = (zlistx_t *) zhashx_lookup (self->last_configuration, asset_name);
    // if no sensors were assigned yet, create empty list
    if ( already_assigned_sensors == NULL ) {
        already_assigned_sensors = zlistx_new ();
        // TODO: handle NULL-pointer
        zhashx_insert (self->last_configuration, asset_name, (void *) already_assigned_sensors);
        // ATTENTION: zhashx_set_destructor is not called intentionally, as here we have only "links" on
        // sensors, because "all_assets" owns this information!
    }
    return already_assigned_sensors;
}

//  --------------------------------------------------------------------------
//  According known information about assets, decide, where sensors logically belong to

void
data_reassign_sensors (data_t *self, bool is_propagation_needed)
{
    assert (self);
    // delete old configuration first
    zhashx_purge (self->last_configuration);
    // explicitly say, that we suppose, that no further reconfiguration is neened
    self->is_reconfig_needed = false;

    // go through every known sensor (if it is not sensor, skip it)
    zlistx_t *asset_names = data_asset_names (self);

    char *one_sensor_name = (char *) zlistx_first (asset_names);
    while (one_sensor_name) {
        // get an asset
        fty_proto_t *one_sensor = (fty_proto_t *) zhashx_lookup (self->all_assets, one_sensor_name);
        // discover the sub-type of the asset
        const char *subtype = fty_proto_aux_string (one_sensor, "subtype", "");
        // check if it is sensor or not
        if ( !streq (subtype, "sensor") ) {
            // if it is NOT sensor -> do nothing!
            // and we can move to next one
            one_sensor_name = (char *) zlistx_next (asset_names);
            continue;
        }
        // if it is sensor, do CONFIGURATION

        // first of all, take its logical asset
        const char *logical_asset_name = fty_proto_ext_string (one_sensor, "logical_asset", NULL);
        if ( logical_asset_name == NULL ) {
            log_warning ("Sensor '%s' has no logical asset assigned -> skip it", one_sensor_name);
            one_sensor_name = (char *) zlistx_next (asset_names);
            continue;
        }

        // find detailed information about logical asset
        fty_proto_t *logical_asset = (fty_proto_t *) zhashx_lookup (self->all_assets, logical_asset_name);
        if ( logical_asset == NULL ) {
            log_warning ("Inconsistecy (yet): detailes about logical asset '%s' are not known -> skip sensor '%s'", logical_asset_name, one_sensor_name);
            // Detailed information about logical asset was not found
            // It can happen if:
            //  * reconfiguration started before detailed "logical_asset" message arrived
            //  * something is really wrong!
            self->is_reconfig_needed = true;
            one_sensor_name = (char *) zlistx_next (asset_names);
            continue;
        }
        // We do not allow sensors to be logically assigned to any device or group
        const char *logical_asset_type = fty_proto_aux_string (logical_asset, "type", "");
        if ( streq (logical_asset_type, "device") ||
             streq (logical_asset_type, "group" ) )
        {
            log_error ("Sensor '%s' is logically assigned to '%s' -> skip it", one_sensor_name, logical_asset_type);
            one_sensor_name = (char *) zlistx_next (asset_names);
            continue;
        }
        if ( is_propagation_needed ) {
            // BIOS-2484: start - ignore sensors assigned to the NON-RACK asset
            if ( !streq (logical_asset_type, "rack") ) {
                log_warning ("Sensor '%s' is logically assigned to non-'rack' -> skip it (BIOS-2484)", one_sensor_name);
                one_sensor_name = (char *) zlistx_next (asset_names);
                continue;
            }
        }

        // So, now let us put our sensor to the right place

        // Find already assigned sensors
        zlistx_t *already_assigned_sensors = s_get_assigned_sensors (self, logical_asset_name);
        // add sensor to the list
        zlistx_add_end (already_assigned_sensors, (void *) one_sensor);

        if ( is_propagation_needed ) {
            // BIOS-2484: start - propagate sensor in physical topology
            // (need to add sensor to all "parents" of the logical asset)
            // Actually, the chain is: dc-room-row-rack-device-device -> max 5 level parents can be
            // But here, we start from rack -> only 3 level is available at maximum
            const char *l_parent_name = fty_proto_aux_string (logical_asset, "parent_name.1", NULL);
            if ( l_parent_name ) {
                already_assigned_sensors = s_get_assigned_sensors (self, l_parent_name);
                zlistx_add_end (already_assigned_sensors, (void *) one_sensor);
            }

            l_parent_name = fty_proto_aux_string (logical_asset, "parent_name.2", NULL);
            if ( l_parent_name ) {
                already_assigned_sensors = s_get_assigned_sensors (self, l_parent_name);
                zlistx_add_end (already_assigned_sensors, (void *) one_sensor);
            }

            l_parent_name = fty_proto_aux_string (logical_asset, "parent_name.3", NULL);
            if ( l_parent_name ) {
                already_assigned_sensors = s_get_assigned_sensors (self, l_parent_name);
                zlistx_add_end (already_assigned_sensors, (void *) one_sensor);
            }
        }

        // at this point configuration of this sensor is done
        // and we can move to next one
        one_sensor_name = (char *) zlistx_next (asset_names);
    }
    zlistx_destroy (&asset_names);
}

//  --------------------------------------------------------------------------
//  Before using this functionality, sensors should be assigned to the right positions
//  by calling 'data_reassign_sensors' function.
//  Get list of sensors assigned to the asset
//  You can limit the list of sensors returned to a certain 'sensor_function',
//  NULL returns all sensors.
//  Returns NULL when for 'asset_name' sensors are not known or asset_name is not known at all
//  or in case of memory issues
//  The caller is responsible for destroying the return value when finished with it

zlistx_t *
data_get_assigned_sensors (
        data_t *self,
        const char *asset_name,
        const char *sensor_function)
{
    assert (self);
    assert (asset_name);

    zlistx_t *sensors = (zlistx_t *) zhashx_lookup (self->last_configuration, asset_name);
    if (!sensors) {
        log_info (
                "Asset '%s' has no sensors assigned (function='%s')",
                asset_name, ( sensor_function == NULL ) ? "(null)": sensor_function);
        return NULL;
    }
    zlistx_t *return_sensor_list = zlistx_new ();
    if ( !return_sensor_list ) {
        log_error ("Memory allocation error");
        return NULL;
    }
    zlistx_set_destructor (return_sensor_list, (czmq_destructor *) fty_proto_destroy);
    zlistx_set_duplicator (return_sensor_list, (czmq_duplicator *) fty_proto_dup);

    fty_proto_t *one_sensor = (fty_proto_t *) zlistx_first (sensors);
    while (one_sensor) {
        if (!sensor_function) {
            zlistx_add_end (return_sensor_list, (void *) one_sensor);
        }
        else if (streq (sensor_function, fty_proto_ext_string (one_sensor, "sensor_function", ""))) {
            zlistx_add_end (return_sensor_list, (void *) one_sensor);
        }
        one_sensor = (fty_proto_t *) zlistx_next (sensors);
    }
    if ( zlistx_first (return_sensor_list) == NULL ) { // in other words if list is empty
        zlistx_destroy (&return_sensor_list);
        return NULL;
    }
    return return_sensor_list;
}

//  --------------------------------------------------------------------------
//  Checks if sensors attributes are ok.
//  Writes to log detailed information "what is wrong"

static void
s_check_sensor_correctness (data_t *self, fty_proto_t *sensor)
{
    assert (self);
    assert (sensor);
    const char *logical_asset = fty_proto_ext_string (sensor, "logical_asset", NULL);
    const char *port = fty_proto_ext_string (sensor, "port", NULL);
    const char *parent_name = fty_proto_aux_string (sensor, "parent_name.1", NULL);

    if (!port) {
        log_error (
                "Sensor='%s':Attribute '%s' is missing from '%s' field in the message.",
                fty_proto_name (sensor), "port", "ext");
		return;
    }
    if (!parent_name) {
        log_error (
                "Sensor='%s':Attribute '%s' is missing from '%s' field in the message.",
                fty_proto_name (sensor), "parent_name.1", "aux");
        return;
    }

    if (!logical_asset) {
        log_error (
                "Sensor='%s':Attribute '%s' is missing from '%s' field in the message.",
                fty_proto_name (sensor), "logical_asset", "ext");
        return;
    }

    // BIOS-3136: for sensors when parent_name is IPC / rack controller subtybe ...
    if (data_get_ipc (self) && streq (parent_name, data_get_ipc (self))) {
        // ... if port is TH1-TH4 ...
        if (port [0] == 'T' && port [1] == 'H' && \
                (port [2] >= 49 && port [2] <= 52)) {
            std::string key = "/dev/ttyS";
            key.append (port);
            port = self->devmap [key].c_str ();
            // ... port is /dev/ttySX - according devmap
            fty_proto_ext_insert (sensor, "port", "%s", port);
        }
        else
        // ... or port is a number ...
        if (isdigit (port [0])) {
            // ... then use /dev/ttyS$port
            std::string foo = "/dev/ttyS";
            foo.append (port);
            port = foo.c_str ();
            fty_proto_ext_insert (sensor, "port", "%s", port);
        }
    }

}

static bool
s_check_container_info_changed (fty_proto_t *asset_old, fty_proto_t *asset_new)
{
    // BIOS-2484: start
    // update of the asset should trigger reconfiguration if topology changed
    // if asset is known we need to check, if physical topology had changed
    // Actually, the chain is: dc-room-row-rack-device-device -> max 5 level parents can be
    // But here, we start from rack -> only 3 level is available at maximum
    if ( !streq (fty_proto_aux_string (asset_old,   "parent_name.1", ""),
                 fty_proto_aux_string (asset_new, "parent_name.1", ""))
        )
    {
        return true;
    }
    if ( !streq (fty_proto_aux_string (asset_old,   "parent_name.2", ""),
                 fty_proto_aux_string (asset_new, "parent_name.2", ""))
        )
    {
        return true;
    }
    if ( !streq (fty_proto_aux_string (asset_old,   "parent_name.3", ""),
                 fty_proto_aux_string (asset_new, "parent_name.3", ""))
        )
    {
        return true;
    }
    return false;
    // BIOS-2484: end
}

static bool
s_check_sensor_info_changed (fty_proto_t *asset_old, fty_proto_t *asset_new)
{
    if ( !streq (fty_proto_ext_string (asset_old, "logical_asset", ""),
                 fty_proto_ext_string (asset_new, "logical_asset", ""))
        )
    {
        return true;
    }
    if ( !streq (fty_proto_ext_string (asset_old, "port", ""),
                 fty_proto_ext_string (asset_new, "port", ""))
        )
    {
        return true;
    }
    if ( !streq (fty_proto_ext_string (asset_old, "calibration_offset_t", ""),
                 fty_proto_ext_string (asset_new, "calibration_offset_t", ""))
        )
    {
        return true;
    }
    if ( !streq (fty_proto_ext_string (asset_old, "calibration_offset_h", ""),
                 fty_proto_ext_string (asset_new, "calibration_offset_h", ""))
        )
    {
        return true;
    }
    if ( !streq (fty_proto_ext_string (asset_old, "sensor_function", ""),
                 fty_proto_ext_string (asset_new, "sensor_function", ""))
        )
    {
        return true;
    }
    // TODO add sensor driver when it would be needed
    return false;
}

//  --------------------------------------------------------------------------
//  Store asset, takes ownership of the message

bool
data_asset_store (data_t *self, fty_proto_t **message_p)
{
    assert (self);
    assert (message_p);
    if (!*message_p)
        return false;
    fty_proto_t *message = *message_p;

    const char *name = fty_proto_name (message);
    const char *operation = fty_proto_operation (message);
    log_debug ("Process message: op='%s', asset_name='%s'", operation, fty_proto_name (message));
    const char *type = fty_proto_aux_string (message, "type", "");

    const char *subtype = fty_proto_aux_string (message, "subtype", "");

    if (streq (subtype, "rack controller")) {
        if (!data_get_ipc (self) || !streq (data_get_ipc (self), name))
            self->is_reconfig_needed = true;
        data_set_ipc (self, name);
    }

    if (streq (operation, FTY_PROTO_ASSET_OP_CREATE) ) {
        if ( streq (type, "datacenter") ||
             streq (type, "room") ||
             streq (type, "row") ||
             streq (type, "rack") )
        {
            // always do reconfiguration
            self->is_reconfig_needed = true;
        } else
        if ( streq (subtype, "sensor") ) {
            // lets check, that sensor has all necessary information
            // So, we have "device" and it is "sensor"!
            s_check_sensor_correctness (self, message);
            // store it in any case, because we cannot ignore message on UPDATE operation,
            // and in order to be consistent do not ignore it ere on CREATE operation
            self->is_reconfig_needed = true;
        } else {
            // intentionally left empty
            // here we are if message is for "group" or any "device" other than "sensor"
            // because they can not impact configuration
        }
        zhashx_update (self->all_assets, fty_proto_name (message), (void *) message);
        *message_p = NULL;
        return true;
    } else
    if ( streq (operation, FTY_PROTO_ASSET_OP_UPDATE) ) {
        if ( streq (type, "datacenter") ||
             streq (type, "room") ||
             streq (type, "row") ||
             streq (type, "rack") )
        {
            // So, if NOT "device" is UPDATED -> do reconfiguration only if TOPOLGY had changed
            // Look for the asset
            fty_proto_t *asset = (fty_proto_t*) zhashx_lookup (self->all_assets, fty_proto_name (message));
            if ( asset == NULL ) { // if the asset was not known (for any reason)
                self->is_reconfig_needed = true;
            }
            else {
                if ( s_check_container_info_changed (asset, message) ) {
                    self->is_reconfig_needed = true;
                }
            }
        } else
        if ( streq (subtype, "sensor") ) {
            // So, we have "device" and it is "sensor"!
            // lets check, that sensor has all necessary information
            s_check_sensor_correctness (self, message);
            // store it in any case, because we cannot ignore message on UPDATE operation,
            // Because if we ignore this message -> everything would be configured
            // with old information which is already obsolete

            // do reconfiguration only if important info changes
            // Look for the asset
            fty_proto_t *asset = (fty_proto_t*) zhashx_lookup (self->all_assets, fty_proto_name (message));
            if ( asset == NULL ) { // if it not known (for any reason)
                self->is_reconfig_needed = true;
            }
            else {
                if ( s_check_sensor_info_changed (asset, message) ) {
                    self->is_reconfig_needed = true;
                }
            }
        } else {
            // intentionally left empty
            // here we are if message is for "group" or any "device" other than "sensor"
            // because they can not impact configuration
        }
        zhashx_update (self->all_assets, fty_proto_name (message), (void *) message);
        *message_p = NULL;
        return true;
    } else
    if (streq (operation, FTY_PROTO_ASSET_OP_DELETE) ||
        streq (operation, FTY_PROTO_ASSET_OP_RETIRE))
    {
        void *exists = zhashx_lookup (self->all_assets, fty_proto_name (message));
        if (exists)
            self->is_reconfig_needed = true;
        zhashx_delete (self->all_assets, fty_proto_name (message));
        fty_proto_destroy (message_p);
        *message_p = NULL;
        return true;
    }
    else {
        log_debug ("Msg: op='%s', asset_name='%s' is not interesting", operation, fty_proto_name (message));
        fty_proto_destroy (message_p);
        *message_p = NULL;
        return false;
    }
}

//  --------------------------------------------------------------------------
//  Update list of metrics produced by composite_metrics

void
data_set_produced_metrics (data_t *self,const std::set <std::string> &metrics)
{
    assert (self);
    self->produced_metrics.clear();
    self->produced_metrics = metrics;
}

//  --------------------------------------------------------------------------
//  Get list of metrics produced by composite_metrics

std::set <std::string>
data_get_produced_metrics (data_t *self)
{
    assert (self);
    return self->produced_metrics;
}

//  --------------------------------------------------------------------------
//  Returns 'true' if some of recently added asset requires the reconfiguration
//                 or if reconfiguration was done in 'inconsistent' state
//                 and we MUST reconfigure one more time

bool
data_is_reconfig_needed (data_t *self)
{
    assert (self);
    return self->is_reconfig_needed;
}

//  --------------------------------------------------------------------------
//  Get asset names
//  The caller is responsible for destroying the return value when finished with it.

zlistx_t *
data_asset_names (data_t *self)
{
    assert (self);
    zlistx_t *list = zhashx_keys (self->all_assets);
    zlistx_set_comparator (list, (czmq_comparator *) strcmp);
    return list;
}

//  --------------------------------------------------------------------------
//  Get information for any given asset name if it is known or NULL otherwise
//  Ownership is NOT transferred

fty_proto_t *
data_asset (data_t *self, const char *name)
{
    assert (self);
    assert (name);
    return (fty_proto_t *) zhashx_lookup (self->all_assets, name);
}

//  --------------------------------------------------------------------------
//  Save data to disk
//  0 - success, -1 - error

int
data_save (data_t *self, const char * filename)
{
    assert (self);
    zconfig_t *root = zconfig_new ("nobody_cares", NULL);
    if (!root) {
        log_error ("root=zconfig_new() failed");
        return -1;
    }
    zconfig_t *assets = zconfig_new ("assets", root);
    if (!assets) {
        log_error ("assets=zconfig_new() failed");
        zconfig_destroy (&root);
        return -1;
    }
    int i = 1;
    for (fty_proto_t *bmsg = (fty_proto_t*) zhashx_first (self->all_assets);
                       bmsg != NULL;
                       bmsg = (fty_proto_t*) zhashx_next (self->all_assets))
    {
        zconfig_t *item = zconfig_new (std::to_string (i).c_str(), assets);
        i++;
        zconfig_put (item, "name", fty_proto_name (bmsg));
        zconfig_put (item, "operation", fty_proto_operation (bmsg));

        zhash_t *aux = fty_proto_aux (bmsg);
        if ( aux ) {
            for (const char *aux_value = (const char*) zhash_first (aux);
                    aux_value != NULL;
                    aux_value = (const char*) zhash_next (aux))
            {
                const char *aux_key = (const char*) zhash_cursor (aux);
                char *item_key;
                int r = asprintf (&item_key, "aux.%s", aux_key);
                assert (r != -1);   // make gcc @ rhel happy
                zconfig_put (item, item_key, aux_value);
                zstr_free (&item_key);
            }
        }
        zhash_t *ext = fty_proto_ext (bmsg);
        if ( ext ) {
            for (const char *value = (const char*) zhash_first (ext);
                    value != NULL;
                    value = (const char*) zhash_next (ext))
            {
                const char *key = (const char*) zhash_cursor (ext);
                char *item_key;
                int r = asprintf (&item_key, "ext.%s", key);
                assert (r != -1);   // make gcc @ rhel happy
                zconfig_put (item, item_key, value);
                zstr_free (&item_key);
            }
        }
    }
    zconfig_t *metrics = zconfig_new ("produced_metrics", root);
    int j = 1;
    for ( const auto &metric_topic : self->produced_metrics ) {
        zconfig_put (metrics, std::to_string (j).c_str(), metric_topic.c_str() );
        j++;
    }
    if ( self->is_reconfig_needed ) {
        zconfig_t *reconfig = zconfig_new ("is_reconfig_needed", root);
        assert (reconfig); // make compiler happy!!
    }
    if (data_get_ipc (self)) {
        zconfig_t *ipc_name = zconfig_new ("ipc_name", root);
        zconfig_set_value (ipc_name, "%s", data_get_ipc (self));
    }

    int r = zconfig_save (root, filename);
    zconfig_destroy (&root);
    return r;
}

//  --------------------------------------------------------------------------
//  Load data from disk
//  0 - success, -1 - error

data_t *
data_load (const char *filename)
{
    if ( !filename )
        return NULL;

    zconfig_t *root = zconfig_load (filename);
    if (!root)
        return NULL;

    data_t *self = data_new ();
    if (!self) {
        zconfig_destroy (&root);
        return NULL;
    }
    zconfig_t *sub_config = zconfig_child (root); // first child
    for ( ; sub_config != NULL; sub_config = zconfig_next (sub_config) ) { // next child
        const char *sub_key = zconfig_name (sub_config);
        if ( strncmp (sub_key, "assets", 6) == 0 ) {
            zconfig_t *key_config = zconfig_child (sub_config); // actually represents one asset
            for ( ; key_config != NULL; key_config = zconfig_next (key_config))
            {
                // 1. create bmsg
                fty_proto_t *bmsg = fty_proto_new (FTY_PROTO_ASSET);
                fty_proto_set_name (bmsg, "%s", zconfig_get (key_config, "name", ""));
                fty_proto_set_operation (bmsg, "%s", zconfig_get (key_config, "operation", ""));

                // 2. put aux things
                zconfig_t *bmsg_config = zconfig_child (key_config);
                for (; bmsg_config != NULL; bmsg_config = zconfig_next (bmsg_config))
                {
                    const char *bmsg_key = zconfig_name (bmsg_config);
                    if (strncmp (bmsg_key, "aux.", 4) == 0)
                        fty_proto_aux_insert (bmsg, (bmsg_key+4), "%s", zconfig_value (bmsg_config));
                    if (strncmp (bmsg_key, "ext.", 4) == 0)
                        fty_proto_ext_insert (bmsg, (bmsg_key+4), "%s", zconfig_value (bmsg_config));
                }
                zhashx_update (self->all_assets, zconfig_get (key_config, "name", ""), bmsg);
            }
            continue;
        }
        if ( strncmp (sub_key, "produced_metrics", 16) == 0 ) {
            zconfig_t *key_config = zconfig_child (sub_config); // actually represents one metric
            for ( ; key_config != NULL; key_config = zconfig_next (key_config))
            {
                self->produced_metrics.insert (zconfig_value (key_config));
            }
            continue;
        }
        if ( strncmp (sub_key, "is_reconfig_needed", 18) == 0 ) {
            self->is_reconfig_needed = true;
            continue;
        }
        if ( strncmp (sub_key, "ipc_name", 8) == 0 ) {
            data_set_ipc (self, zconfig_value (sub_config));
            continue;
        }
        // if we are here, then unexpected config subtree found
        log_info ("key '%s' is not supported", sub_key);
    }
    zconfig_destroy (&root);
    return self;
}

//  --------------------------------------------------------------------------
//  Destroy the data

void
data_destroy (data_t **self_p)
{
    if (!self_p)
        return;
    if (*self_p) {
        data_t *self = *self_p;
        //  Free class properties here
        zhashx_destroy (&self->all_assets);
        zhashx_destroy (&self->last_configuration);
        self->produced_metrics.clear();
        zstr_free (&self->ipc_name);
        //  Free object itself
        self->devmap.clear ();
		free (self);
        *self_p = NULL;
    }
}

//  --------------------------------------------------------------------------
//  Self test of this class

//  Helper test function
//  create new ASSET message of type fty_proto_t
static fty_proto_t *
test_asset_new (const char *name, const char *operation)
{
    assert (name);
    assert (operation);

    fty_proto_t *asset = fty_proto_new (FTY_PROTO_ASSET);
    fty_proto_set_name (asset, "%s", name);
    fty_proto_set_operation (asset, "%s", operation);
    return asset;
}

//  Helper test function
//  0 - same, 1 - different
static int
test_zlistx_compare (zlistx_t *expected, zlistx_t **received_p, bool verbose = false)
{
    assert (expected);
    assert (received_p && *received_p);
    zlistx_t *received = *received_p;

    const char *cursor = (const char *) zlistx_first (expected);
    while (cursor) {
        void *handle = zlistx_find (received, (void *) cursor);
        if (!handle) {
            if ( verbose )
                log_debug ("expected but not found: %s", cursor);
            zlistx_destroy (received_p);
            *received_p = NULL;
            return 1;
        }
        zlistx_delete (received, handle);
        cursor = (const char *) zlistx_next (expected);
    }
    int rv = 1;
    if (zlistx_size (received) == 0) {
        rv = 0;
    } else {
        if ( verbose ) {
            const char *element = (const char *) zlistx_first (received);
            log_debug ("received but not expected:");
            while (element) {
                log_debug ("\t%s", element);
                element = (const char *) zlistx_next (received);
            }
        }
    }
    zlistx_destroy (received_p);
    *received_p = NULL;
    return rv;
}

static void
data_compare (data_t *source, data_t *target, bool verbose) {

    if ( source == NULL )
        assert ( target == NULL );
    else {
        assert ( target != NULL );

        zsys_debug ("data_get_ipc (source)=%s <%p>", data_get_ipc (source), (void*) data_get_ipc (source));
        zsys_debug ("data_get_ipc (target)=%s <%p>", data_get_ipc (target), (void*) data_get_ipc (target));

        if (data_get_ipc (source) != NULL) {
            assert (data_get_ipc (target));
            assert (streq (data_get_ipc (source), data_get_ipc (target)));
        }
        else    // XXX FIXME TODO: why is there "(null)" sometime
            assert (data_get_ipc (target) == NULL);

        // test all_assets
        assert ( source-> all_assets != NULL ); // by design, it should be not NULL!
        assert ( target-> all_assets != NULL ); // by design, it should be not NULL!
        for ( fty_proto_t *source_asset = (fty_proto_t *) zhashx_first (source->all_assets);
              source_asset != NULL;
              source_asset = (fty_proto_t *) zhashx_next (source->all_assets)
            )
        {
            void *handle = zhashx_lookup (target->all_assets, fty_proto_name (source_asset));
            if ( handle == NULL ) {
                if ( verbose )
                    log_debug ("asset='%s' is NOT in target, but expected", fty_proto_name (source_asset));
                assert ( false );
            }
        }
        for ( fty_proto_t *target_asset = (fty_proto_t *) zhashx_first (target->all_assets);
              target_asset != NULL;
              target_asset = (fty_proto_t *) zhashx_next (target->all_assets)
            )
        {
            void *handle = zhashx_lookup (source->all_assets, fty_proto_name (target_asset));
            if ( handle == NULL ) {
                if ( verbose )
                    log_debug ("asset='%s' is in target, but NOT expected", fty_proto_name (target_asset));
                assert ( false );
            }
        }
        // test is_reconfig_needed
        assert ( source->is_reconfig_needed == target->is_reconfig_needed );
        // test last_configuration
        assert ( zhashx_size (target->last_configuration) == 0 );
        // test produced_metrics
        for ( const auto &source_metric : source->produced_metrics) {
            if ( target->produced_metrics.count (source_metric) != 1 ) {
                if ( verbose )
                    log_debug ("produced_topic='%s' is NOT in target, but expected", source_metric.c_str());
                assert ( false );
            }
        }
        for ( const auto &target_metric : target->produced_metrics) {
            if ( source->produced_metrics.count (target_metric) != 1 ) {
                if ( verbose )
                    log_debug ("produced_topic='%s' is in target, but NOT expected", target_metric.c_str());
                assert ( false );
            }
        }
    }
}

static void
test4 (bool verbose)
{
    if ( verbose )
        log_debug ("Test4: save/load test");

    // Note: If your selftest reads SCMed fixture data, please keep it in
    // src/selftest-ro; if your test creates filesystem objects, please
    // do so under src/selftest-rw. They are defined below along with a
    // usecase (asert) to make compilers happy.
    const char *SELFTEST_DIR_RO = "src/selftest-ro";
    const char *SELFTEST_DIR_RW = "src/selftest-rw";
    assert (SELFTEST_DIR_RO);
    assert (SELFTEST_DIR_RW);
    // std::string str_SELFTEST_DIR_RO = std::string(SELFTEST_DIR_RO);
    // std::string str_SELFTEST_DIR_RW = std::string(SELFTEST_DIR_RW);

    char *test_state_file = zsys_sprintf ("%s/state_file_test4", SELFTEST_DIR_RW);
    assert (test_state_file != NULL);
    char *test_state_file1 = zsys_sprintf ("%s/state_file_test4-1", SELFTEST_DIR_RW);
    assert (test_state_file1 != NULL);

    data_t *self = NULL;
    data_t *self_load = NULL;
    fty_proto_t *asset = NULL;

    // just ipc_name
    self = data_new ();
    data_set_ipc (self, "IPC");
    data_save (self, test_state_file);

    self_load = data_load (test_state_file);

    data_compare (self, self_load, verbose);
    data_destroy (&self);
    data_destroy (&self_load);

    // one asset without AUX without EXT
    asset = test_asset_new ("some_asset", FTY_PROTO_ASSET_OP_CREATE);
    self = data_new ();
    data_asset_store (self, &asset);

    data_save (self, test_state_file);

    self_load = data_load (test_state_file);

    data_compare (self, self_load, verbose);
    data_destroy (&self);
    data_destroy (&self_load);

    // one asset without EXT
    asset = test_asset_new ("some_asset_without_ext", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "type", "%s", "datacenter");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    self = data_new ();

    data_asset_store (self, &asset);

    data_save (self, test_state_file);

    self_load = data_load (test_state_file);

    data_compare (self, self_load, verbose);
    data_destroy (&self);
    data_destroy (&self_load);

    // one asset without AUX
    asset = test_asset_new ("some_asset_without_aux", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_ext_insert (asset, "type", "%s", "datacenter");
    fty_proto_ext_insert (asset, "subtype", "%s", "unknown");
    self = data_new ();

    data_asset_store (self, &asset);

    data_save (self, test_state_file);

    self_load = data_load (test_state_file);

    data_compare (self, self_load, verbose);
    data_destroy (&self);
    data_destroy (&self_load);

    // three assets + reassign + metrics
    self = data_new ();

    asset = test_asset_new ("TEST4_DC", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_ext_insert (asset, "type", "%s", "datacenter");
    fty_proto_ext_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);

    asset = test_asset_new ("TEST4_RACK", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST4_DC");
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);

    asset = test_asset_new ("TEST4_SENSOR", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST4_UPS");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH2");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "2");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "20");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST4_RACK");
    data_asset_store (self, &asset);

    data_reassign_sensors (self, true);
    std::set <std::string> metrics {"topic1", "topic2"};
    data_set_produced_metrics (self, metrics);

    data_save (self, test_state_file);

    self_load = data_load (test_state_file);

    data_compare (self, self_load, verbose);

    data_save (self_load, test_state_file1);
    data_t *self_load_load = data_load (test_state_file1);
    data_compare (self, self_load_load, verbose);

    data_destroy (&self);
    data_destroy (&self_load);
    data_destroy (&self_load_load);
    zstr_free (&test_state_file);
    zstr_free (&test_state_file1);
}

static void
test5 (bool verbose)
{
    if ( verbose )
        log_debug ("Test5: Sensor arrived before logical asset for CREATE operation");

    data_t *self = data_new();
    fty_proto_t *asset = NULL;
    zlistx_t *sensors = NULL;

    zlistx_t *assets_expected = zlistx_new ();
    zlistx_set_destructor (assets_expected, (czmq_destructor *) zstr_free);
    zlistx_set_duplicator (assets_expected, (czmq_duplicator *) strdup);
    zlistx_set_comparator (assets_expected, (czmq_comparator *) strcmp);

    // TOPOLOGY:
    // DC->ROOM->ROW->RACK->UPS->SENSOR
    if ( verbose )
        log_debug ("\tCREATE 'TEST5_DC' as datacenter");
    asset = test_asset_new ("TEST5_DC", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "type", "%s", "datacenter");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, true); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );
    zlistx_add_end (assets_expected, (void *) "TEST5_DC");

    if ( verbose )
        log_debug ("\tCREATE 'TEST5_ROOM' as room");
    asset = test_asset_new ("TEST5_ROOM", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST5_DC");
    fty_proto_aux_insert (asset, "type", "%s", "room");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, true); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );
    zlistx_add_end (assets_expected, (void *) "TEST5_ROOM");

    if ( verbose )
        log_debug ("\tCREATE 'TEST5_ROW' as row");
    asset = test_asset_new ("TEST5_ROW", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST5_ROOM");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST5_DC");
    fty_proto_aux_insert (asset, "type", "%s", "row");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, true); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );
    zlistx_add_end (assets_expected, (void *) "TEST5_ROW");

    if ( verbose ) {
        log_debug ("\tCREATE 'TEST5_SENSOR' as sensor");
        log_debug ("\t\tSituation: sensor asset message arrives before asset specified in logical_asset");
    }
    asset = test_asset_new ("TEST5_SENSOR", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST5_UPS");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST5_RACK");
    fty_proto_aux_insert (asset, "parent_name.3", "%s", "TEST5_ROW");
    fty_proto_aux_insert (asset, "parent_name.4", "%s", "TEST5_ROOM");
    fty_proto_aux_insert (asset, "parent_name.5", "%s", "TEST5_DC");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH1");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "10");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST5_RACK");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );

    // test assigned sensors: propagated
    data_reassign_sensors (self, true);
    assert ( data_is_reconfig_needed (self) == true );
    sensors = data_get_assigned_sensors (self, "TEST5_DC", NULL);
    assert ( sensors == NULL );
    sensors = data_get_assigned_sensors (self, "TEST5_ROOM", NULL);
    assert ( sensors == NULL );
    sensors = data_get_assigned_sensors (self, "TEST5_ROW", NULL);
    assert ( sensors == NULL );
    sensors = data_get_assigned_sensors (self, "TEST5_RACK", NULL);
    assert ( sensors == NULL );

    // test assigned sensors: NOT propagated
    data_reassign_sensors (self, false);
    assert ( data_is_reconfig_needed (self) == true );
    sensors = data_get_assigned_sensors (self, "TEST5_DC", NULL);
    assert ( sensors == NULL );
    sensors = data_get_assigned_sensors (self, "TEST5_ROOM", NULL);
    assert ( sensors == NULL );
    sensors = data_get_assigned_sensors (self, "TEST5_ROW", NULL);
    assert ( sensors == NULL );
    sensors = data_get_assigned_sensors (self, "TEST5_RACK", NULL);
    assert ( sensors == NULL );

    // test that all messages expected to be store are really stores
    zlistx_add_end (assets_expected, (void *) "TEST5_SENSOR");
    {
        zlistx_t *received = data_asset_names (self);
        assert ( received );

        int rv = test_zlistx_compare (assets_expected, &received, verbose);
        assert ( rv == 0 );
    }

    if ( verbose ) {
        log_debug ("\tCREATE 'TEST5_RACK' as rack");
        log_debug ("\t\tSituation: finally message about logical_asset arrived");
    }
    asset = test_asset_new ("TEST5_RACK", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST5_ROW");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST5_ROOM");
    fty_proto_aux_insert (asset, "parent_name.3", "%s", "TEST5_DC");
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );

    // test that all messages expected to be store are really stores
    zlistx_add_end (assets_expected, (void *) "TEST5_RACK");
    {
        zlistx_t *received = data_asset_names (self);
        assert (received);

        int rv = test_zlistx_compare (assets_expected, &received, verbose);
        assert (rv == 0);
    }

    // test assigned sensors: propagated
    data_reassign_sensors (self, true);
    assert ( data_is_reconfig_needed (self) == false );
    sensors = data_get_assigned_sensors (self, "TEST5_DC", NULL);
    assert ( sensors != NULL );
    assert ( zlistx_size (sensors) == 1 );
    zlistx_destroy (&sensors);
    sensors = data_get_assigned_sensors (self, "TEST5_ROOM", NULL);
    assert ( sensors != NULL );
    assert ( zlistx_size (sensors) == 1 );
    zlistx_destroy (&sensors);
    sensors = data_get_assigned_sensors (self, "TEST5_ROW", NULL);
    assert ( sensors != NULL );
    assert ( zlistx_size (sensors) == 1 );
    zlistx_destroy (&sensors);
    sensors = data_get_assigned_sensors (self, "TEST5_RACK", NULL);
    assert ( sensors != NULL );
    assert ( zlistx_size (sensors) == 1 );
    zlistx_destroy (&sensors);

    // test assigned sensors: NOT propagated
    data_reassign_sensors (self, false);
    assert ( data_is_reconfig_needed (self) == false );
    sensors = data_get_assigned_sensors (self, "TEST5_DC", NULL);
    assert ( sensors == NULL );
    sensors = data_get_assigned_sensors (self, "TEST5_ROOM", NULL);
    assert ( sensors == NULL );
    sensors = data_get_assigned_sensors (self, "TEST5_ROW", NULL);
    assert ( sensors == NULL );
    sensors = data_get_assigned_sensors (self, "TEST5_RACK", NULL);
    assert ( sensors != NULL );
    assert ( zlistx_size (sensors) == 1 );
    zlistx_destroy (&assets_expected);
    zlistx_destroy (&sensors);
    data_destroy (&self);
}

static void
test6 (bool verbose)
{
    if ( verbose )
        log_debug ("Test6: Sensor important info is missing for CREATE operation");

    data_t *self = data_new();
    fty_proto_t *asset = NULL;

    if ( verbose )
        log_debug ("\tCREATE 'TEST6_RACK' as rack");
    asset = test_asset_new ("TEST6_RACK", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, true);
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tCREATE 'TEST6_SENSOR01' as sensor");
        log_debug ("\t\tlogical_asset is missing");
    }
    asset = test_asset_new ("TEST6_SENSOR01", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST6_UPS");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH3");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    // logical_asset missing
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);

    if ( verbose ) {
        log_debug ("\tCREATE 'TEST6_SENSOR02' as sensor");
        log_debug ("\t\tparent_name.1 is missing");
    }
    asset = test_asset_new ("TEST6_SENSOR02", FTY_PROTO_ASSET_OP_CREATE);
    // parent_name.1 missing
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH3");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST6_RACK");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);

    if ( verbose ) {
        log_debug ("\tCREATE 'TEST6_SENSOR03' as sensor");
        log_debug ("\t\tport is missing");
    }
    asset = test_asset_new ("TEST6_SENSOR03", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST6_UPS");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    // port missing
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST6_RACK");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);

    data_destroy (&self);
}


static void
test7 (bool verbose)
{
    if ( verbose )
        log_debug ("Test7: Sensor important info is missing for UPDATE operation");

    data_t *self = data_new();
    fty_proto_t *asset = NULL;
    zlistx_t *sensors = NULL;

    // SETUP
    if ( verbose )
        log_debug ("\tCREATE 'TEST7_DC' as datacenter");
    asset = test_asset_new ("TEST7_DC", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "type", "%s", "datacenter");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, true); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose )
        log_debug ("\tCREATE 'TEST7_RACK' as rack");
    asset = test_asset_new ("TEST7_RACK", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST7_DC");
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, true); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose )
        log_debug ("\tCREATE 'TEST7_SENSOR01' as sensor");
    asset = test_asset_new ("TEST7_SENSOR01", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST7_UPS");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH1");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST7_RACK");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, true); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose )
        log_debug ("\tCREATE 'TEST7_SENSOR02' as sensor");
    asset = test_asset_new ("TEST7_SENSOR02", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST7_UPS");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH2");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST7_RACK");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, true); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose )
        log_debug ("\tCREATE 'TEST7_SENSOR03' as sensor");
    asset = test_asset_new ("TEST7_SENSOR03", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST7_UPS");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH3");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST7_RACK");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, true); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    // actual test
    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST7_SENSOR01' as sensor");
        log_debug ("\t\tlogical_asset is missing");
    }
    asset = test_asset_new ("TEST7_SENSOR01", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST7_UPS");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH3");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    // logical_asset missing
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, true); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST7_SENSOR02' as sensor");
        log_debug ("\t\tparent_name.1 is missing");
    }
    asset = test_asset_new ("TEST7_SENSOR02", FTY_PROTO_ASSET_OP_UPDATE);
    // parent_name.1 missing
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH3");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST7_DC");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, true); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tCREATE 'TEST7_SENSOR03' as sensor");
        log_debug ("\t\tport is missing");
    }
    asset = test_asset_new ("TEST7_SENSOR03", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST7_UPS");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    // port missing
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST7_RACK");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );

    // test assigned sensors: propagated
    data_reassign_sensors (self, true);
    assert ( data_is_reconfig_needed (self) == false );
    sensors = data_get_assigned_sensors (self, "TEST7_DC", NULL);
    assert ( sensors != NULL ); // rack sensor was propagated!
    assert ( zlistx_size (sensors) == 1 );
    zlistx_destroy (&sensors);
    sensors = data_get_assigned_sensors (self, "TEST7_RACK", NULL);
    assert ( sensors != NULL );
    assert ( zlistx_size (sensors) == 1 );
    zlistx_destroy (&sensors);

    // test assigned sensors: NOT propagated
    data_reassign_sensors (self, false);
    assert ( data_is_reconfig_needed (self) == false );
    sensors = data_get_assigned_sensors (self, "TEST7_DC", NULL);
    assert ( sensors != NULL ); // dc sensor was assigned!
    assert ( zlistx_size (sensors) == 1 );
    zlistx_destroy (&sensors);
    sensors = data_get_assigned_sensors (self, "TEST7_RACK", NULL);
    assert ( sensors != NULL );
    assert ( zlistx_size (sensors) == 1 );
    zlistx_destroy (&sensors);

    data_destroy (&self);
}

static void
test8 (bool verbose)
{
    if ( verbose )
        log_debug ("Test8: data_get_assigned_sensors");

    data_t *self = data_new();
    fty_proto_t *asset = NULL;
    zlistx_t *sensors = NULL;
    fty_proto_t *item = NULL;

    if ( verbose )
        log_debug ("\tCREATE 'TEST8_DC' as datacenter");
    asset = test_asset_new ("TEST8_DC", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "type", "%s", "datacenter");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);

    if ( verbose )
        log_debug ("\tCREATE 'TEST8_RACK' as rack");
    asset = test_asset_new ("TEST8_RACK", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST8_DC");
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    data_asset_store (self, &asset);

    if ( verbose )
        log_debug ("\tCREATE 'TEST8_SENSOR01' as sensor");
    asset = test_asset_new ("TEST8_SENSOR01", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST8_UPS");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH1");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "10");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST8_RACK");
    data_asset_store (self, &asset);

    if ( verbose )
        log_debug ("\tCREATE 'TEST8_SENSOR02' as sensor");
    asset = test_asset_new ("TEST8_SENSOR02", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST8_UPS");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH2");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "2");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "20");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST8_RACK");
    data_asset_store (self, &asset);

    if ( verbose )
        log_debug ("\tCREATE 'TEST8_SENSOR03' as sensor");
    asset = test_asset_new ("TEST8_SENSOR03", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST8_UPS");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH3");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "output");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST8_RACK");
    data_asset_store (self, &asset);

    if ( verbose )
        log_debug ("\tCREATE 'TEST8_SENSOR04' as sensor");
    asset = test_asset_new ("TEST8_SENSOR04", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST8_UPS");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH4");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "1");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST8_RACK");
    data_asset_store (self, &asset);

    data_reassign_sensors (self, false);

    sensors = data_get_assigned_sensors (self, "Non-existing-asset", NULL);
    assert (sensors == NULL);
    sensors = data_get_assigned_sensors (self, "Non-existing-asset", "input");
    assert (sensors == NULL);

    sensors = data_get_assigned_sensors (self, "TEST8_DC", NULL);
    assert (sensors == NULL);
    sensors = data_get_assigned_sensors (self, "TEST8_DC", "ambient");
    assert (sensors == NULL);

    sensors = data_get_assigned_sensors (self, "TEST8_RACK", NULL);
    assert (sensors != NULL);
    assert (zlistx_size (sensors) == 4);
    zlistx_destroy (&sensors);

    sensors = data_get_assigned_sensors (self, "TEST8_RACK", "input");
    assert (sensors != NULL);
    assert (zlistx_size (sensors) == 2);
    item = (fty_proto_t *) zlistx_first (sensors);
    const char *name1 = fty_proto_name (item);
    item = (fty_proto_t *) zlistx_next (sensors);
    const char *name2 = fty_proto_name (item);
    assert
        (   (streq(name1, "TEST8_SENSOR01") || (streq(name2, "TEST8_SENSOR02")) )
            ||
            (streq(name1, "TEST8_SENSOR02") || (streq(name2, "TEST8_SENSOR01")) )
        );
    zlistx_destroy (&sensors);

    sensors = data_get_assigned_sensors (self, "TEST8_RACK", "notknownfunction");
    assert (sensors == NULL);

    sensors = data_get_assigned_sensors (self, "TEST8_RACK", "output");
    assert (sensors != NULL);
    assert (zlistx_size (sensors) == 1);
    item = (fty_proto_t *) zlistx_first (sensors);
    assert ( streq(fty_proto_name (item), "TEST8_SENSOR03") );
    zlistx_destroy (&sensors);

    sensors = data_get_assigned_sensors (self, "TEST8_RACK", "");
    assert (sensors != NULL);
    assert (zlistx_size (sensors) == 1);
    item = (fty_proto_t *) zlistx_first (sensors);
    assert ( streq(fty_proto_name (item), "TEST8_SENSOR04") );
    zlistx_destroy (&sensors);

    sensors = data_get_assigned_sensors (self, "TEST8_SENSOR03", NULL);
    assert (sensors == NULL);

    data_destroy (&self);
}


static void
test9 (bool verbose)
{
    if ( verbose )
        log_debug ("Test9: Check that sensor logically assigned to device/group is skipped");

    data_t *self = data_new();
    fty_proto_t *asset = NULL;
    zlistx_t *sensors = NULL;

    // Physical topology:
    // DC->RACK->UPS->SENSOR01
    //         ->RC ->SENSOR02
    //   ->GROUP
    if ( verbose )
        log_debug ("\tCREATE 'TEST9_DC' as datacenter");
    asset = test_asset_new ("TEST9_DC", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "type", "%s", "datacenter");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);

    if ( verbose )
        log_debug ("\tCREATE 'TEST9_GROUP' as group");
    asset = test_asset_new ("TEST9_GROUP", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST9_DC");
    fty_proto_aux_insert (asset, "type", "%s", "group");
    fty_proto_aux_insert (asset, "subtype", "%s", "power_input");
    data_asset_store (self, &asset);

    if ( verbose )
        log_debug ("\tCREATE 'TEST9_RACK' as rack");
    asset = test_asset_new ("TEST9_RACK", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST9_DC");
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    data_asset_store (self, &asset);

    if ( verbose )
        log_debug ("\tCREATE 'TEST9_UPS' as ups");
    asset = test_asset_new ("TEST9_UPS", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST9_RACK");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST9_DC");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "ups");
    data_asset_store (self, &asset);

    if ( verbose )
        log_debug ("\tCREATE 'TEST9_RC' as rack controller");
    asset = test_asset_new ("TEST9_RC", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST9_RACK");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST9_DC");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "rack controller");
    data_asset_store (self, &asset);

    if ( verbose )
        log_debug ("\tCREATE 'TEST9_SENSOR01' as sensor");
    asset = test_asset_new ("TEST9_SENSOR01", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST9_UPS");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST9_RACK");
    fty_proto_aux_insert (asset, "parent_name.3", "%s", "TEST9_DC");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH1");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "10");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST9_RACK");
    data_asset_store (self, &asset);

    if ( verbose ) {
        log_debug ("\tCREATE 'TEST9_SENSOR02' as sensor");
        log_debug ("\t\tSituation: sensor logically assigned to device");
    }
    asset = test_asset_new ("TEST9_SENSOR02", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST9_RC");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST9_RACK");
    fty_proto_aux_insert (asset, "parent_name.3", "%s", "TEST9_DC");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH2");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "10");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST9_UPS");
    data_asset_store (self, &asset);

    if ( verbose ) {
        log_debug ("\tCREATE 'TEST9_SENSOR03' as sensor");
        log_debug ("\t\tSituation: sensor logically assigned to group");
    }
    asset = test_asset_new ("TEST9_SENSOR03", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST9_DC");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH3");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "10");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST9_GROUP");
    data_asset_store (self, &asset);

    // test assigned sensors: propagated
    data_reassign_sensors (self, true);
    assert ( data_is_reconfig_needed (self) == false );

    sensors = data_get_assigned_sensors (self, "TEST9_DC", NULL);
    assert ( sensors != NULL );
    assert (zlistx_size (sensors) == 1);
    zlistx_destroy (&sensors);

    sensors = data_get_assigned_sensors (self, "TEST9_RACK", NULL);
    assert ( sensors != NULL );
    assert (zlistx_size (sensors) == 1);
    zlistx_destroy (&sensors);

    sensors = data_get_assigned_sensors (self, "TEST9_GROUP", NULL);
    assert ( sensors == NULL );

    sensors = data_get_assigned_sensors (self, "TEST9_UPS", NULL);
    assert ( sensors == NULL );

    // test assigned sensors: NOT propagated
    data_reassign_sensors (self, false);
    assert ( data_is_reconfig_needed (self) == false );

    sensors = data_get_assigned_sensors (self, "TEST9_DC", NULL);
    assert ( sensors == NULL );

    sensors = data_get_assigned_sensors (self, "TEST9_RACK", NULL);
    assert ( sensors != NULL );
    assert (zlistx_size (sensors) == 1);
    zlistx_destroy (&sensors);

    sensors = data_get_assigned_sensors (self, "TEST9_GROUP", NULL);
    assert ( sensors == NULL );

    sensors = data_get_assigned_sensors (self, "TEST9_UPS", NULL);
    assert ( sensors == NULL );

    data_destroy (&self);
}

static void
test10 (bool verbose)
{
    if ( verbose )
        log_debug ("Test10: Check that UPDATE sensor will trigger reconfig only if needed");

    data_t *self = data_new();
    fty_proto_t *asset = NULL;

    if ( verbose )
        log_debug ("\tCREATE 'TEST10_RACK01' as rack");
    asset = test_asset_new ("TEST10_RACK01", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    data_asset_store (self, &asset);

    if ( verbose )
        log_debug ("\tCREATE 'TEST10_RACK02' as rack");
    asset = test_asset_new ("TEST10_RACK02", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    data_asset_store (self, &asset);
    data_reassign_sensors (self, false); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST10_SENSOR01' as sensor");
        log_debug ("\t\tSituation: UPDATE message came, but CREATE message was missing");
    }
    asset = test_asset_new ("TEST10_SENSOR01", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST10_UPS");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST10_DC");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH1");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "10");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST10_RACK01");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, false); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST10_SENSOR01' as sensor");
        log_debug ("\t\tSituation: UPDATE message has the same important info but other different EXT attributes");
    }
    asset = test_asset_new ("TEST10_SENSOR01", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST10_UPS");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST10_DC");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH1");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "10");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST10_RACK01");
    fty_proto_ext_insert (asset, "notimportant_ext_attribute", "%s", "WOW");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST10_SENSOR01' as sensor");
        log_debug ("\t\tSituation: UPDATE message has the same important info but other different AUX attributes");
    }
    asset = test_asset_new ("TEST10_SENSOR01", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST10_UPS");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST10_DC");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_aux_insert (asset, "notimportant_aux_attribute", "%s", "WOW");
    fty_proto_ext_insert (asset, "port", "%s", "TH1");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "10");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST10_RACK01");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST10_SENSOR01' as sensor");
        log_debug ("\t\tSituation: UPDATE message important info ('port') changed");
    }
    asset = test_asset_new ("TEST10_SENSOR01", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST10_UPS");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST10_DC");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH2");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "10");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST10_RACK01");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, false); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST10_SENSOR01' as sensor");
        log_debug ("\t\tSituation: UPDATE message important info ('logical_asset') changed");
    }
    asset = test_asset_new ("TEST10_SENSOR01", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST10_UPS");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST10_DC");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH2");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "10");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST10_RACK02");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, false); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST10_SENSOR01' as sensor");
        log_debug ("\t\tSituation: UPDATE message important info ('calibration_offset_h') changed");
    }
    asset = test_asset_new ("TEST10_SENSOR01", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST10_UPS");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST10_DC");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH2");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "100");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST10_RACK02");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, false); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST10_SENSOR01' as sensor");
        log_debug ("\t\tSituation: UPDATE message important info ('calibration_offset_t') changed");
    }
    asset = test_asset_new ("TEST10_SENSOR01", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST10_UPS");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST10_DC");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH2");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "10");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "100");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST10_RACK02");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, false); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST10_SENSOR01' as sensor");
        log_debug ("\t\tSituation: UPDATE message important info ('sensor_function') changed");
    }
    asset = test_asset_new ("TEST10_SENSOR01", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST10_UPS");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST10_DC");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH2");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "10");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "100");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "output");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST10_RACK02");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, false); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST10_SENSOR01' as sensor");
        log_debug ("\t\tSituation: UPDATE message not important info ('parent_name.1') changed");
    }
    asset = test_asset_new ("TEST10_SENSOR01", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST10_DC");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH2");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "10");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "100");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "output");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST10_RACK02");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == false );

    data_destroy (&self);
}

static void
test11 (bool verbose)
{
    if ( verbose )
        log_debug ("Test11: Check that UPDATE container will trigger reconfig only if needed");

    data_t *self = data_new();
    fty_proto_t *asset = NULL;

    // this one is used in UPDATE operations
    if ( verbose )
        log_debug ("\tCREATE 'TEST11_DC_NEW' as datacenter");
    asset = test_asset_new ("TEST11_DC_NEW", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "type", "%s", "datacenter");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);

    if ( verbose )
        log_debug ("\tCREATE 'TEST11_ROOM_NEW' as room");
    asset = test_asset_new ("TEST11_ROOM_NEW", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST11_DC_NEW");
    fty_proto_aux_insert (asset, "type", "%s", "room");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);

    if ( verbose )
        log_debug ("\tCREATE 'TEST11_ROW_NEW' as row");
    asset = test_asset_new ("TEST11_ROW_NEW", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST11_ROOM_NEW");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST11_DC_NEW");
    fty_proto_aux_insert (asset, "type", "%s", "row");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);

    data_reassign_sensors (self, false); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    // DC
    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST11_DC' as datacenter");
        log_debug ("\t\tSituation: UPDATE message came, but CREATE message was missing");
    }
    asset = test_asset_new ("TEST11_DC", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "type", "%s", "datacenter");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, false); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST11_DC' as datacenter");
        log_debug ("\t\tSituation: UPDATE message has the same important info but other different EXT attributes");
    }
    asset = test_asset_new ("TEST11_DC", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "type", "%s", "datacenter");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    fty_proto_ext_insert (asset, "notimportant_ext_attribute", "%s", "WOW");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST11_DC' as datacenter");
        log_debug ("\t\tSituation: UPDATE message has the same important info but other different AUX attributes");
    }
    asset = test_asset_new ("TEST11_DC", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "type", "%s", "datacenter");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    fty_proto_ext_insert (asset, "notimportant_ext_attribute", "%s", "WOW"); // as it was in previous
    fty_proto_aux_insert (asset, "notimportant_aux_attribute", "%s", "WOW");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == false );

    // ROOM
    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST11_ROOM' as room");
        log_debug ("\t\tSituation: UPDATE message came, but CREATE message was missing");
    }
    asset = test_asset_new ("TEST11_ROOM", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST11_DC");
    fty_proto_aux_insert (asset, "type", "%s", "room");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, false); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST11_ROOM' as room");
        log_debug ("\t\tSituation: UPDATE message has the same important info but other different EXT attributes");
    }
    asset = test_asset_new ("TEST11_ROOM", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST11_DC");
    fty_proto_aux_insert (asset, "type", "%s", "room");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    fty_proto_ext_insert (asset, "notimportant_ext_attribute", "%s", "WOW");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST11_ROOM' as room");
        log_debug ("\t\tSituation: UPDATE message has the same important info but other different AUX attributes");
    }
    asset = test_asset_new ("TEST11_ROOM", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST11_DC");
    fty_proto_aux_insert (asset, "type", "%s", "room");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    fty_proto_ext_insert (asset, "notimportant_ext_attribute", "%s", "WOW"); // as it was in previous
    fty_proto_aux_insert (asset, "notimportant_aux_attribute", "%s", "WOW");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST11_ROOM' as room");
        log_debug ("\t\tSituation: UPDATE message not important info ('parent_name.1') changed");
    }
    asset = test_asset_new ("TEST11_ROOM", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST11_DC_NEW");
    fty_proto_aux_insert (asset, "type", "%s", "room");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    fty_proto_ext_insert (asset, "notimportant_ext_attribute", "%s", "WOW"); // as it was in previous
    fty_proto_aux_insert (asset, "notimportant_aux_attribute", "%s", "WOW"); // as it was in previous
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, false); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    // ROW
    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST11_ROW' as row");
        log_debug ("\t\tSituation: UPDATE message came, but CREATE message was missing");
    }
    asset = test_asset_new ("TEST11_ROW", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST11_ROOM");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST11_DC");
    fty_proto_aux_insert (asset, "type", "%s", "row");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, false); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST11_ROW' as row");
        log_debug ("\t\tSituation: UPDATE message has the same important info but other different EXT attributes");
    }
    asset = test_asset_new ("TEST11_ROW", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST11_ROOM");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST11_DC");
    fty_proto_aux_insert (asset, "type", "%s", "row");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    fty_proto_ext_insert (asset, "notimportant_ext_attribute", "%s", "WOW");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST11_ROW' as row");
        log_debug ("\t\tSituation: UPDATE message has the same important info but other different AUX attributes");
    }
    asset = test_asset_new ("TEST11_ROW", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST11_ROOM");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST11_DC");
    fty_proto_aux_insert (asset, "type", "%s", "row");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    fty_proto_ext_insert (asset, "notimportant_ext_attribute", "%s", "WOW"); // as it was in previous
    fty_proto_aux_insert (asset, "notimportant_aux_attribute", "%s", "WOW");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST11_ROW' as row");
        log_debug ("\t\tSituation: UPDATE message not important info ('parent_name.1') changed");
    }
    asset = test_asset_new ("TEST11_ROW", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST11_ROOM_NEW");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST11_DC");
    fty_proto_aux_insert (asset, "type", "%s", "row");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    fty_proto_ext_insert (asset, "notimportant_ext_attribute", "%s", "WOW"); // as it was in previous
    fty_proto_aux_insert (asset, "notimportant_aux_attribute", "%s", "WOW"); // as it was in previous
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, false); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST11_ROW' as row");
        log_debug ("\t\tSituation: UPDATE message not important info ('parent_name.2') changed");
    }
    asset = test_asset_new ("TEST11_ROW", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST11_ROOM_NEW");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST11_DC_NEW");
    fty_proto_aux_insert (asset, "type", "%s", "row");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    fty_proto_ext_insert (asset, "notimportant_ext_attribute", "%s", "WOW"); // as it was in previous
    fty_proto_aux_insert (asset, "notimportant_aux_attribute", "%s", "WOW"); // as it was in previous
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, false); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    // RACK
    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST11_RACK' as rack");
        log_debug ("\t\tSituation: UPDATE message came, but CREATE message was missing");
    }
    asset = test_asset_new ("TEST11_RACK", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST11_ROW");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST11_ROOM");
    fty_proto_aux_insert (asset, "parent_name.3", "%s", "TEST11_DC");
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, false); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST11_RACK' as rack");
        log_debug ("\t\tSituation: UPDATE message has the same important info but other different EXT attributes");
    }
    asset = test_asset_new ("TEST11_RACK", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST11_ROW");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST11_ROOM");
    fty_proto_aux_insert (asset, "parent_name.3", "%s", "TEST11_DC");
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    fty_proto_ext_insert (asset, "notimportant_ext_attribute", "%s", "WOW");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST11_RACK' as rack");
        log_debug ("\t\tSituation: UPDATE message has the same important info but other different AUX attributes");
    }
    asset = test_asset_new ("TEST11_RACK", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST11_ROW");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST11_ROOM");
    fty_proto_aux_insert (asset, "parent_name.3", "%s", "TEST11_DC");
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    fty_proto_ext_insert (asset, "notimportant_ext_attribute", "%s", "WOW"); // as it was in previous
    fty_proto_aux_insert (asset, "notimportant_aux_attribute", "%s", "WOW");
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST11_RACK' as rack");
        log_debug ("\t\tSituation: UPDATE message not important info ('parent_name.1') changed");
    }
    asset = test_asset_new ("TEST11_RACK", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST11_ROW_NEW");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST11_ROOM");
    fty_proto_aux_insert (asset, "parent_name.3", "%s", "TEST11_DC");
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    fty_proto_ext_insert (asset, "notimportant_ext_attribute", "%s", "WOW"); // as it was in previous
    fty_proto_aux_insert (asset, "notimportant_aux_attribute", "%s", "WOW"); // as it was in previous
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, false); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST11_RACK' as rack");
        log_debug ("\t\tSituation: UPDATE message not important info ('parent_name.2') changed");
    }
    asset = test_asset_new ("TEST11_RACK", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST11_ROW_NEW");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST11_ROOM_NEW");
    fty_proto_aux_insert (asset, "parent_name.3", "%s", "TEST11_DC");
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    fty_proto_ext_insert (asset, "notimportant_ext_attribute", "%s", "WOW"); // as it was in previous
    fty_proto_aux_insert (asset, "notimportant_aux_attribute", "%s", "WOW"); // as it was in previous
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, false); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    if ( verbose ) {
        log_debug ("\tUPDATE 'TEST11_RACK' as rack");
        log_debug ("\t\tSituation: UPDATE message not important info ('parent_name.3') changed");
    }
    asset = test_asset_new ("TEST11_RACK", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST11_ROW_NEW");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST11_ROOM_NEW");
    fty_proto_aux_insert (asset, "parent_name.3", "%s", "TEST11_DC_NEW");
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    fty_proto_ext_insert (asset, "notimportant_ext_attribute", "%s", "WOW"); // as it was in previous
    fty_proto_aux_insert (asset, "notimportant_aux_attribute", "%s", "WOW"); // as it was in previous
    data_asset_store (self, &asset);
    assert ( data_is_reconfig_needed (self) == true );
    data_reassign_sensors (self, false); // drop the flag "is reconfiguration needed"
    assert ( data_is_reconfig_needed (self) == false );

    data_destroy (&self);
}

void
data_test (bool verbose)
{
    if ( verbose )
        log_set_level (LOG_DEBUG);

    printf (" * data: \n");
    //  @selftest
    //  =================================================================
    if ( verbose )
        log_debug ("Test1: Simple create/destroy test");
    data_t *self = data_new ();
    assert (self);
    zlistx_t *asset_names = data_asset_names (self);
    assert (asset_names != NULL);
    assert (zlistx_size (asset_names) == 0);
    zlistx_destroy (&asset_names);
    data_destroy (&self);
    assert (self == NULL);

    data_destroy (&self);
    assert (self == NULL);

    self = data_new ();

    //  =================================================================
    if ( verbose )
        log_debug ("Test2: data_asset_store()/data_is_reconfig_needed()/data_is_reconfig_needed() for CREATE operation");
    // asset
    zlistx_t *assets_expected = zlistx_new ();
    zlistx_set_destructor (assets_expected, (czmq_destructor *) zstr_free);
    zlistx_set_duplicator (assets_expected, (czmq_duplicator *) strdup);

    zlistx_set_comparator (assets_expected, (czmq_comparator *) strcmp);
    fty_proto_t *item = NULL;
    fty_proto_t *asset = NULL;

    if ( verbose )
        log_debug ("\tCREATE 'TEST1_DC' as datacenter");
    asset = test_asset_new ("TEST1_DC", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "type", "%s", "datacenter");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "TEST1_DC");

    if ( verbose )
        log_debug ("\tCREATE 'TEST1_ROOM01' as room");
    asset = test_asset_new ("TEST1_ROOM01", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_DC");
    fty_proto_aux_insert (asset, "type", "%s", "room");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "TEST1_ROOM01");

    if ( verbose )
        log_debug ("\tCREATE 'TEST1_ROOM02 with spaces' as room");
    asset = test_asset_new ("TEST1_ROOM02 with spaces", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_DC");
    fty_proto_aux_insert (asset, "type", "%s", "room");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "TEST1_ROOM02 with spaces");

    if ( verbose )
        log_debug ("\tCREATE 'TEST1_ROW01' as row");
    asset = test_asset_new ("TEST1_ROW01", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_ROOM01");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST1_DC");
    fty_proto_aux_insert (asset, "type", "%s", "row");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "TEST1_ROW01");

    if ( verbose )
        log_debug ("\tCREATE 'TEST1_RACK01' as rack");
    asset = test_asset_new ("TEST1_RACK01", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_ROW01");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST1_ROOM01");
    fty_proto_aux_insert (asset, "parent_name.3", "%s", "TEST1_DC");
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "TEST1_RACK01");

    if ( verbose )
        log_debug ("\tCREATE 'TEST1_RACK02' as rack");
    asset = test_asset_new ("TEST1_RACK02", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_ROW01");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST1_ROOM01");
    fty_proto_aux_insert (asset, "parent_name.3", "%s", "TEST1_DC");
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "TEST1_RACK02");

    if ( verbose )
        log_debug ("\tCREATE 'TEST1_ROW02' as row");
    asset = test_asset_new ("TEST1_ROW02", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_ROOM02");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST1_DC");
    fty_proto_aux_insert (asset, "type", "%s", "row");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "TEST1_ROW02");

    if ( verbose )
        log_debug ("\tCREATE 'TEST1_ROW03' as row");
    asset = test_asset_new ("TEST1_ROW03", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_ROOM02");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST1_DC");
    fty_proto_aux_insert (asset, "type", "%s", "row");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "TEST1_ROW03");

    if ( verbose )
        log_debug ("\tCREATE 'TEST1_RACK03' as rack");
    asset = test_asset_new ("TEST1_RACK03", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_ROW02");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST1_ROOM02");
    fty_proto_aux_insert (asset, "parent_name.3", "%s", "TEST1_DC");
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "TEST1_RACK03");

    if ( verbose )
        log_debug ("\tCREATE 'TEST1_RACK04' as rack");
    asset = test_asset_new ("TEST1_RACK04", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_ROW03");
    fty_proto_aux_insert (asset, "parent_name.2", "%s", "TEST1_ROOM02");
    fty_proto_aux_insert (asset, "parent_name.3", "%s", "TEST1_DC");
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "TEST1_RACK04");

    if ( verbose )
        log_debug ("\tCREATE 'TEST1_RACK01.ups1' as ups");
    asset = test_asset_new ("TEST1_RACK01.ups1", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "ups");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == false);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "TEST1_RACK01.ups1");

    if ( verbose )
        log_debug ("\tCREATE 'TEST1_GROUP' as group");
    asset = test_asset_new ("TEST1_GROUP", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "type", "%s", "group");
    fty_proto_aux_insert (asset, "subtype", "%s", "power_input");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == false);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "TEST1_GROUP");

    if ( verbose )
        log_debug ("\tCREATE 'Sensor01' as sensor");
    asset = test_asset_new ("Sensor01", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH1");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "10");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_RACK01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor01");

    if ( verbose )
        log_debug ("\tCREATE 'Sensor02' as sensor");
    asset = test_asset_new ("Sensor02", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH2");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "2");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "20");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_RACK01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor02");

    if ( verbose )
        log_debug ("\tCREATE 'Sensor03' as sensor");
    asset = test_asset_new ("Sensor03", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH3");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_RACK01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor03");

    if ( verbose )
        log_debug ("\tCREATE 'Sensor08' as sensor");
    asset = test_asset_new ("Sensor08", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH4");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "1");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_RACK02");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor08");

    if ( verbose )
        log_debug ("\tCREATE 'Sensor09' as sensor");
    asset = test_asset_new ("Sensor09", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH5");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "2.0");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "2.0");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "output");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_RACK02");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor09");

    if ( verbose )
        log_debug ("\tCREATE 'Sensor10' as sensor");
    asset = test_asset_new ("Sensor10", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH6");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "output");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_RACK01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor10");

    if ( verbose )
        log_debug ("\tCREATE 'Sensor11' as sensor");
    asset = test_asset_new ("Sensor11", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH7");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "15.5");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "20.7");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "output");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_RACK01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor11");

    if ( verbose )
        log_debug ("\tCREATE 'Sensor12' as sensor");
    asset = test_asset_new ("Sensor12", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH8");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "0");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "0");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "neuvedeno");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_RACK01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor12");

    if ( verbose )
        log_debug ("\tCREATE 'Sensor13' as sensor");
    asset = test_asset_new ("Sensor13", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH9");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "-1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "1");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_ROW03");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor13");

    if ( verbose )
        log_debug ("\tCREATE 'Sensor14' as sensor");
    asset = test_asset_new ("Sensor14", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH10");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_ROOM02 with spaces");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor14");

    if ( verbose )
        log_debug ("\tCREATE 'Sensor15' as sensor");
    asset = test_asset_new ("Sensor15", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH11");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1.4");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "-1");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "output");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_ROW03");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, false);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor15");

    printf ("TRACE ---===### (Test block -1-) ###===---\n");
    {
        // check, that list of assets stored is the expected one
        zlistx_t *received = data_asset_names (self);
        assert (received);
        int rv = test_zlistx_compare (assets_expected, &received, verbose);
        assert (rv == 0);

        // check functionality of the data_asset 
        asset = data_asset (self, "TEST1_DC");
        assert (asset);
        assert (streq (fty_proto_aux_string (asset, "type", ""), "datacenter"));
        assert (streq (fty_proto_name (asset), "TEST1_DC"));

        asset = data_asset (self, "TEST1_ROOM01");
        assert (asset);
        assert (streq (fty_proto_aux_string (asset, "type", ""), "room"));
        assert (streq (fty_proto_name (asset), "TEST1_ROOM01"));

        asset = data_asset (self, "TEST1_ROOM02 with spaces");
        assert (asset);
        assert (streq (fty_proto_aux_string (asset, "type", ""), "room"));
        assert (streq (fty_proto_name (asset), "TEST1_ROOM02 with spaces"));

        asset = data_asset (self, "TEST1_ROW01");
        assert (asset);
        assert (streq (fty_proto_aux_string (asset, "type", ""), "row"));
        assert (streq (fty_proto_name (asset), "TEST1_ROW01"));

        asset = data_asset (self, "TEST1_RACK01");
        assert (asset);
        asset = data_asset (self, "TEST1_RACK02");
        assert (asset);
        asset = data_asset (self, "TEST1_RACK01.ups1");
        assert (asset);
        asset = data_asset (self, "non-existing-sensor");
        assert (asset == NULL);
        asset = data_asset (self, "TEST1_GROUP");
        assert (asset);

        // check, that sensors where assigned correctly
        zlistx_t *sensors = data_get_assigned_sensors (self, "TEST1_DC", NULL);
        assert (sensors == NULL);

        sensors = data_get_assigned_sensors (self, "TEST1_ROOM01", NULL);
        assert (sensors == NULL);

        sensors = data_get_assigned_sensors (self, "TEST1_ROOM02 with spaces", NULL);
        assert (sensors != NULL);
        assert (zlistx_size (sensors) == 1);
        item = (fty_proto_t *) zlistx_first (sensors);
        assert (streq (fty_proto_name (item), "Sensor14"));
        zlistx_destroy (&sensors);

        sensors = data_get_assigned_sensors (self, "TEST1_ROW01", NULL);
        assert (sensors == NULL);

        sensors = data_get_assigned_sensors (self, "TEST1_ROW02", NULL);
        assert (sensors == NULL);

        sensors = data_get_assigned_sensors (self, "TEST1_ROW03", NULL);
        assert (sensors != NULL);
        assert (zlistx_size (sensors) == 2);
        item = (fty_proto_t *) zlistx_first (sensors);
        const char *name1 = fty_proto_name (item);
        item = (fty_proto_t *) zlistx_next (sensors);
        const char *name2 = fty_proto_name (item);
        assert
            (   (streq(name1, "Sensor13") || (streq(name2, "Sensor15")) )
              ||
                (streq(name1, "Sensor15") || (streq(name2, "Sensor13")) )
            );
        zlistx_destroy (&sensors);

/*
        sensors = data_get_assigned_sensors (self, "TEST1_RACK02", NULL);
        assert (zlistx_size (sensors) == 2);
        {
            fty_proto_t *item = (fty_proto_t *) zlistx_first (sensors);
            assert (streq (fty_proto_name (item), "Sensor08"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH4"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), "bottom"));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), "input"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), "1"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), "1"));
            item = (fty_proto_t *) zlistx_next (sensors);

            assert (streq (fty_proto_name (item), "Sensor09"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH5"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), "top"));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), "output"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), "2.0"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), "2.0"));
            item = (fty_proto_t *) zlistx_next (sensors);
        }
        zlistx_destroy (&sensors);

        sensors = data_get_assigned_sensors (self, "TEST1_RACK03", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_get_assigned_sensors (self, "TEST1_RACK04", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_get_assigned_sensors (self, "TEST1_RACK01", NULL);
        assert (zlistx_size (sensors) == 6);
        {
            fty_proto_t *item = (fty_proto_t *) zlistx_first (sensors);
            assert (streq (fty_proto_name (item), "Sensor01"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH1"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), "bottom"));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), "input"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), "1"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), "10"));

            item = (fty_proto_t *) zlistx_next (sensors);
            assert (streq (fty_proto_name (item), "Sensor02"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH2"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), "bottom"));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), "input"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), "2"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), "20"));

            item = (fty_proto_t *) zlistx_next (sensors);
            assert (streq (fty_proto_name (item), "Sensor03"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH3"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), "middle"));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), "input"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), "3"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), "30"));

            item = (fty_proto_t *) zlistx_next (sensors);
            assert (streq (fty_proto_name (item), "Sensor10"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH6"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), "top"));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), "output"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), ""));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), ""));

            item = (fty_proto_t *) zlistx_next (sensors);
            assert (streq (fty_proto_name (item), "Sensor11"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH7"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), "top"));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), "output"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), "15.5"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), "20.7"));

            item = (fty_proto_t *) zlistx_next (sensors);
            assert (streq (fty_proto_name (item), "Sensor12"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH8"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), ""));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), "neuvedeno"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), "0"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), "0"));

        }
        zlistx_destroy (&sensors);
*/
    }

    if ( verbose )
        log_debug ("\tCREATE 'ups2' as ups");
    asset = test_asset_new ("ups2", FTY_PROTO_ASSET_OP_CREATE); // 12
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "ups");
    fty_proto_aux_insert (asset, "parent", "%s", "10");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == false);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "ups2");

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor01'");
    asset = test_asset_new ("Sensor01", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH1");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "-5.2");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_RACK01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor02'");
    asset = test_asset_new ("Sensor02", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent", "%s", "12");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "ups2");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH1");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "-7");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "-4.14");
    fty_proto_ext_insert (asset, "vertical_position", "%s", "middle");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_RACK01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor03'");
    asset = test_asset_new ("Sensor03", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH3");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "output");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_RACK01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor10'");
    asset = test_asset_new ("Sensor10", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH2");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "-0.16");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "output");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_RACK01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);

    if ( verbose )
        log_debug ("\tRETIRE 'Sensor11'");
    asset = test_asset_new ("Sensor11", FTY_PROTO_ASSET_OP_RETIRE);
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    void *handle = zlistx_find (assets_expected, (void *) "Sensor11");
    zlistx_delete (assets_expected, handle);

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor08'");
    asset = test_asset_new ("Sensor08", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH4");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "2.0");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "12");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_DC");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor09'");
    asset = test_asset_new ("Sensor09", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH5");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "5");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "50");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_ROW03");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor04'");
    asset = test_asset_new ("Sensor04", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH12");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "1");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_ROOM01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor04");

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor05'");
    asset = test_asset_new ("Sensor05", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH13");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "4");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "-6");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "output");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_DC");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor05");

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor06'");
    asset = test_asset_new ("Sensor06", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH14");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "-1.2");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "-1.4");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "output");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_ROOM01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor06");

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor07'");
    asset = test_asset_new ("Sensor07", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH15");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "4");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "-25");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "ambient");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_ROW03");

    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor07");

    if ( verbose )
        log_debug ("\tDELETE 'Sensor12'");
    asset = test_asset_new ("Sensor12", FTY_PROTO_ASSET_OP_DELETE);
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == false);
    handle = zlistx_find (assets_expected, (void *) "Sensor12");
    zlistx_delete (assets_expected, handle);

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor14'");
    asset = test_asset_new ("Sensor14", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "ups2");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH10");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "This-asset-does-not-exist");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == true);

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor15'");
    asset = test_asset_new ("Sensor15", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01.ups1");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH11");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "2");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "-3");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_ROOM02 with spaces");
    data_asset_store (self, &asset);
    data_reassign_sensors (self, true);
    assert (data_is_reconfig_needed (self) == true);

    if ( verbose )
        log_debug ("\tDELETE 'Sensor13'");
    asset = test_asset_new ("Sensor13", FTY_PROTO_ASSET_OP_DELETE);
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    data_asset_store (self, &asset);
    data_reassign_sensors (self, true);
    handle = zlistx_find (assets_expected, (void *) "Sensor13");
    zlistx_delete (assets_expected, handle);

    printf ("TRACE ---===### (Test block -2-) ###===---\n");
    {
        zlistx_t *received = data_asset_names (self);
        assert (received);
        int rv = test_zlistx_compare (assets_expected, &received, verbose);
        assert (rv == 0);

        asset = data_asset (self, "ups2");
        assert (asset);

        asset = data_asset (self, "This-asset-does-not-exist");
        assert (asset == NULL);
/*
        zlistx_t *sensors = data_get_assigned_sensors (self, "This-asset-does-not-exist", NULL);
        assert (sensors == NULL);

        sensors = data_get_assigned_sensors (self, "TEST1_DC", NULL);
        assert (zlistx_size (sensors) == 2);
        {
            fty_proto_t *item = (fty_proto_t *) zlistx_first (sensors);
            assert (streq (fty_proto_name (item), "Sensor08"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH4"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), "middle"));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), "input"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), "2.0"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), "12"));
            item = (fty_proto_t *) zlistx_next (sensors);

            assert (streq (fty_proto_name (item), "Sensor05"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH13"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), "top"));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), "output"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), "4"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), "-6"));
            item = (fty_proto_t *) zlistx_next (sensors);
        }
        zlistx_destroy (&sensors);

        sensors = data_get_assigned_sensors (self, "TEST1_ROOM01", NULL);
        assert (zlistx_size (sensors) == 2);
        {
            fty_proto_t *item = (fty_proto_t *) zlistx_first (sensors);
            assert (streq (fty_proto_name (item), "Sensor04"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH12"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), ""));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), ""));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), "1"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), "1"));
            item = (fty_proto_t *) zlistx_next (sensors);

            assert (streq (fty_proto_name (item), "Sensor06"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH14"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), ""));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), "output"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), "-1.2"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), "-1.4"));
            item = (fty_proto_t *) zlistx_next (sensors);
        }
        zlistx_destroy (&sensors);

        sensors = data_get_assigned_sensors (self, "TEST1_ROOM02 with spaces", NULL);
        assert (zlistx_size (sensors) == 1);
        {
            fty_proto_t *item = (fty_proto_t *) zlistx_first (sensors);
            assert (streq (fty_proto_name (item), "Sensor15"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH11"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), ""));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), ""));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), "2"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), "-3"));
            item = (fty_proto_t *) zlistx_next (sensors);

        }
        zlistx_destroy (&sensors);

        sensors = data_get_assigned_sensors (self, "TEST1_ROW01", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_get_assigned_sensors (self, "TEST1_ROW02", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_get_assigned_sensors (self, "TEST1_ROW03", NULL);
        assert (zlistx_size (sensors) == 2);
        {
            fty_proto_t *item = (fty_proto_t *) zlistx_first (sensors);
            assert (streq (fty_proto_name (item), "Sensor09"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH5"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), ""));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), ""));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), "5"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), "50"));
            item = (fty_proto_t *) zlistx_next (sensors);

            assert (streq (fty_proto_name (item), "Sensor07"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH15"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), ""));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), "ambient"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), "4"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), "-25"));
            item = (fty_proto_t *) zlistx_next (sensors);
        }
        zlistx_destroy (&sensors);

        sensors = data_get_assigned_sensors (self, "TEST1_RACK02", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_get_assigned_sensors (self, "TEST1_RACK03", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_get_assigned_sensors (self, "TEST1_RACK04", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_get_assigned_sensors (self, "TEST1_RACK01", NULL);
        assert (zlistx_size (sensors) == 4);
        {
            fty_proto_t *item = (fty_proto_t *) zlistx_first (sensors);
            assert (streq (fty_proto_name (item), "Sensor01"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH1"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), "bottom"));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), "input"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), "-5.2"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), ""));

            item = (fty_proto_t *) zlistx_next (sensors);
            assert (streq (fty_proto_name (item), "Sensor02"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "ups2"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH1"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), "middle"));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), "input"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), "-7"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), "-4.14"));

            item = (fty_proto_t *) zlistx_next (sensors);
            assert (streq (fty_proto_name (item), "Sensor03"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH3"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), ""));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), "output"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), ""));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), ""));

            item = (fty_proto_t *) zlistx_next (sensors);
            assert (streq (fty_proto_name (item), "Sensor10"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH2"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), "top"));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), "output"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), ""));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), "-0.16"));
        }
        zlistx_destroy (&sensors);
        */
    }

    if ( verbose )
        log_debug ("\tDELETE 'Sensor15'");
    asset = test_asset_new ("Sensor15", FTY_PROTO_ASSET_OP_DELETE);
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    data_asset_store (self, &asset);
    data_reassign_sensors (self, true);
    handle = zlistx_find (assets_expected, (void *) "Sensor15");
    zlistx_delete (assets_expected, handle);
    assert (data_is_reconfig_needed (self) == true);

    if ( verbose )
        log_debug ("\tDELETE 'TEST1_ROW03'");
    asset = test_asset_new ("TEST1_ROW03", FTY_PROTO_ASSET_OP_DELETE);
    fty_proto_aux_insert (asset, "type", "%s", "row");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    data_reassign_sensors (self, true);

    void *to_delete = zlistx_find  (assets_expected, (void *) "TEST1_ROW03");
    assert (to_delete);
    int rv = zlistx_delete (assets_expected, to_delete);
    assert (rv == 0);
    assert (data_is_reconfig_needed (self) == true);

    if ( verbose )
        log_debug ("\tCREATE 'Sensor16' as sensor");
    asset = test_asset_new ("Sensor16", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "nas rack controller");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH2");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "-3.51");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "TEST1_DC");
    data_asset_store (self, &asset);
    data_reassign_sensors (self, true);
    zlistx_add_end (assets_expected, (void *) "Sensor16");
    assert (data_is_reconfig_needed (self) == true);

    if ( verbose )
        log_debug ("\tCREATE 'nas rack constroller' as rack controller");
    asset = test_asset_new ("nas rack controller", FTY_PROTO_ASSET_OP_CREATE); // 12
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "rack controller");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "TEST1_RACK01");
    data_asset_store (self, &asset);
    data_reassign_sensors (self, true);
    // "true" is expected, because data_reassign_sensors also changes the "is_reconfig_needed"
    // when detailed information about the asset is not known
    assert (data_is_reconfig_needed (self) == true);
    zlistx_add_end (assets_expected, (void *) "nas rack controller");

    printf ("TRACE ---===### (Test block -3-) ###===---\n");
    {
        zlistx_t *received = data_asset_names (self);
        assert (received);
        int rv = test_zlistx_compare (assets_expected, &received, verbose);
        assert (rv == 0);
/*
        zlistx_t *sensors = data_get_assigned_sensors (self, "TEST1_DC", NULL);
        assert (zlistx_size (sensors) == 3);
        {
            fty_proto_t *item = (fty_proto_t *) zlistx_first (sensors);
            assert (streq (fty_proto_name (item), "Sensor08"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH4"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), "middle"));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), "input"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), "2.0"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), "12"));
            item = (fty_proto_t *) zlistx_next (sensors);

            assert (streq (fty_proto_name (item), "Sensor05"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "TEST1_RACK01.ups1"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH13"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), "top"));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), "output"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), "4"));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), "-6"));
            item = (fty_proto_t *) zlistx_next (sensors);

            assert (streq (fty_proto_name (item), "Sensor16"));
            assert (streq (fty_proto_aux_string (item, "parent_name.1", ""), "nas rack controller"));
            assert (streq (fty_proto_ext_string (item, "port", ""), "TH2"));
            assert (streq (fty_proto_ext_string (item, "vertical_position", ""), ""));
            assert (streq (fty_proto_ext_string (item, "sensor_function", ""), ""));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_t", ""), ""));
            assert (streq (fty_proto_ext_string (item, "calibration_offset_h", ""), "-3.51"));
            item = (fty_proto_t *) zlistx_next (sensors);
        }
        zlistx_destroy (&sensors);

        asset = data_asset (self, "TEST1_ROOM02 with spaces");
        assert (asset);
        assert (streq (fty_proto_aux_string (asset, "type", ""), "room"));
        assert (streq (fty_proto_name (asset), "TEST1_ROOM02 with spaces"));
        assert (streq (fty_proto_aux_string (asset, "parent", ""), "1"));

        sensors = data_get_assigned_sensors (self, "TEST1_ROOM02 with spaces", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        asset = data_asset (self, "TEST1_ROW03");
        assert (asset == NULL);

        sensors = data_get_assigned_sensors (self, "TEST1_ROW03", NULL);
        assert (sensors == NULL);
        */
    }

    test4 (verbose);
    test5 (verbose);
    test6 (verbose);
    test7 (verbose);
    test8 (verbose);
    test9 (verbose);
    test10(verbose);
    test11(verbose);

    data_t *newdata = data_new();
    std::set <std::string> newset{"sdlkfj"};
    data_set_produced_metrics (newdata, newset);

    data_destroy (&newdata);

    zlistx_destroy (&assets_expected);
    data_destroy (&self);
    //  @end
    printf (" * data: OK\n");
}
