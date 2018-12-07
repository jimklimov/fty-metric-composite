/*  =========================================================================
    c_metric_conf - structure that represents current start of
            composite-metrics-configurator

    Copyright (C) 2014 - 2016 Eaton

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

#ifndef C_METRIC_CONF_H_INCLUDED
#define C_METRIC_CONF_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _c_metric_conf_t c_metric_conf_t;

//  @interface
//  Create a new empty configuration
FTY_METRIC_COMPOSITE_EXPORT c_metric_conf_t *
    c_metric_conf_new (const char *name);

//  Get server name
FTY_METRIC_COMPOSITE_EXPORT const char *
    c_metric_conf_name (c_metric_conf_t *self);

/*
//  Get data
FTY_METRIC_COMPOSITE_EXPORT data_t *
    c_metric_conf_data (c_metric_conf_t *self);

//  Get data and transfers ownership
FTY_METRIC_COMPOSITE_EXPORT data_t *
    c_metric_conf_get_data (c_metric_conf_t *self);

//  Set data transfering ownership from caller
FTY_METRIC_COMPOSITE_EXPORT void
    c_metric_conf_set_data (c_metric_conf_t *self, data_t **data_p);
*/

//  Get client
FTY_METRIC_COMPOSITE_EXPORT mlm_client_t *
    c_metric_conf_client (c_metric_conf_t *self);

//  Get propagation of sensors in topology
FTY_METRIC_COMPOSITE_EXPORT bool
    c_metric_conf_propagation (c_metric_conf_t *self);

//  Set propagation of sensors in topology
FTY_METRIC_COMPOSITE_EXPORT void
    c_metric_conf_set_propagation (c_metric_conf_t *self, bool is_propagation_needed);

//  Get path to confuration directory
FTY_METRIC_COMPOSITE_EXPORT const char *
    c_metric_conf_cfgdir (c_metric_conf_t *self);

//  Set configuration directory path
//  Directory MUST exist! If directory doesn't exist -> error
//  0 - success, -1 - error
FTY_METRIC_COMPOSITE_EXPORT int
    c_metric_conf_set_cfgdir (c_metric_conf_t *self, const char *path);

//  Destroy the c_metric_conf
FTY_METRIC_COMPOSITE_EXPORT void
    c_metric_conf_destroy (c_metric_conf_t **self_p);

//  Self test of this class
FTY_METRIC_COMPOSITE_EXPORT void
    c_metric_conf_test (bool verbose);

//  @end

#ifdef __cplusplus
}
#endif

#endif
