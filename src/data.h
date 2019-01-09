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

#ifndef DATA_H_INCLUDED
#define DATA_H_INCLUDED

#include <set>
#include <string>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _data_t data_t;

//  @interface
//  Create a new data
FTY_METRIC_COMPOSITE_EXPORT data_t *
    data_new (void);

FTY_METRIC_COMPOSITE_EXPORT void
data_set_ipc (data_t *self, const std::string& name);

FTY_METRIC_COMPOSITE_EXPORT const char*
data_get_ipc (data_t *self);

//  Store asset, takes ownership of the message
//  Return:
//      true - if metric was stores
//      false - if metric was ignored
FTY_METRIC_COMPOSITE_EXPORT bool
    data_asset_store (data_t *self, fty_proto_t **message_p);

//  According known information about assets, decide, where sensors logically belong to
FTY_METRIC_COMPOSITE_EXPORT void
    data_reassign_sensors (data_t *self, bool is_propagation_needed);

//  Before using this functionality, sensors should be assigned to the right positions
//  by calling 'data_reassign_sensors' function.
//  Get list of sensors assigned to the asset
//  You can limit the list of sensors returned to a certain 'sensor_function',
//  NULL returns all sensors.
//  Returns NULL when for 'asset_name' sensors are not known or asset_name is not known at all
//  or in case of memory issues
//  The caller is responsible for destroying the return value when finished with it
FTY_METRIC_COMPOSITE_EXPORT zlistx_t *
    data_get_assigned_sensors (
        data_t *self,
        const char *asset_name,
        const char *sensor_function);

//  Returns 'true' if some of recently added asset requires the reconfiguration
//                 or if reconfiguration was done in 'inconsistent' state
//                 and we MUST reconfigure one more time
FTY_METRIC_COMPOSITE_EXPORT bool
    data_is_reconfig_needed (data_t *self);

//  Update list of metrics produced by composite_metrics
FTY_METRIC_COMPOSITE_EXPORT void
    data_set_produced_metrics (data_t *self,const std::set <std::string> &metrics);

//  Get list of metrics produced by composite_metrics
FTY_METRIC_COMPOSITE_EXPORT std::set <std::string>
    data_get_produced_metrics (data_t *self);

//  Get asset names
//  The caller is responsible for destroying the return value when finished with it
FTY_METRIC_COMPOSITE_EXPORT zlistx_t *
    data_asset_names (data_t *self);

//  Get information for any given asset name if it is known or NULL otherwise
//  Ownership is NOT transferred
FTY_METRIC_COMPOSITE_EXPORT fty_proto_t *
    data_asset (data_t *self, const char *name);

//  Load ASSETS from fty-asset
//  data_t*- success or   NULL error
FTY_METRIC_COMPOSITE_EXPORT data_t *
    data_load (c_metric_conf_t *cfg);

//  Destroy the data
FTY_METRIC_COMPOSITE_EXPORT void
    data_destroy (data_t **self_p);

//  Self test of this class
FTY_METRIC_COMPOSITE_EXPORT void
    data_test (bool verbose);

//  @end

#ifdef __cplusplus
}
#endif

#endif
