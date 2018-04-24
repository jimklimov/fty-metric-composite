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

/*
@header
    c_metric_conf - structure that represents current start of
            composite-metrics-configurator
@discuss
@end
*/

#include "fty_metric_composite_classes.h"
#include <unistd.h>
#include <sys/types.h>

struct _c_metric_conf_t {
    bool verbose;                   // is server verbose?
    char *name;                     // server name
//    data_t *asset_data;         // asset data
    mlm_client_t *client;           // malamute client
    char *statefile_name;           // state file name
    char *configuration_dir;        // configuration directory
    bool is_propagation_needed;     // should sensors be propagated in topology?
};

//  --------------------------------------------------------------------------
//  Create a new empty configuration

c_metric_conf_t*
c_metric_conf_new (const char *name)
{
    assert (name);

    c_metric_conf_t *self = (c_metric_conf_t*) zmalloc (sizeof (c_metric_conf_t));
    if (self) {
        self->name = strdup (name);
        if (self->name)
////            self->asset_data = data_new ();
////        if (self->asset_data)
            self->client = mlm_client_new ();
        if (self->client)
            self->statefile_name = strdup ("");
        if (self->statefile_name)
            self->configuration_dir = strdup ("");
        if (self->configuration_dir) {
            self->verbose = false;
            self->is_propagation_needed = true;
        }
        else
            c_metric_conf_destroy (&self);
    }
    return self;
}

//  --------------------------------------------------------------------------
//  Destroy the c_metric_conf

void
c_metric_conf_destroy (c_metric_conf_t **self_p)
{
    if (*self_p)
    {
        c_metric_conf_t *self = *self_p;
        // free structure items
        zstr_free (&self->name);
//        data_destroy (&self->asset_data);
        mlm_client_destroy (&self->client);
        zstr_free (&self->statefile_name);
        zstr_free (&self->configuration_dir);
        // free structure itself
        free (self);
        *self_p = NULL;
    }
}

//  --------------------------------------------------------------------------
//  Get server name

const char *
c_metric_conf_name (c_metric_conf_t *self)
{
    assert (self);
    return self->name;
}

//  --------------------------------------------------------------------------
//  Get client

mlm_client_t *
c_metric_conf_client (c_metric_conf_t *self)
{
    assert (self);
    return self->client; 
}

/*
//  --------------------------------------------------------------------------
//  Get data

data_t *
c_metric_conf_data (c_metric_conf_t *self)
{
    assert (self);
    return self->asset_data;
}

//  --------------------------------------------------------------------------
//  Get data and transfers ownership

data_t *
c_metric_conf_get_data (c_metric_conf_t *self)
{
    assert (self);
    data_t *data = self->asset_data;
    self->asset_data = NULL;
    return data;
}

//  --------------------------------------------------------------------------
//  Set data transfering ownership from caller

void
c_metric_conf_set_data (c_metric_conf_t *self, data_t **data_p)
{
    assert (self);
    assert (data_p);
    data_destroy (&self->asset_data);
    self->asset_data = *data_p;
    *data_p = NULL;    
}
*/

//  --------------------------------------------------------------------------
//  Get state file fullpath or empty string if not set

const char *
c_metric_conf_statefile (c_metric_conf_t *self)
{
    assert (self);
    return self->statefile_name;
}

//  --------------------------------------------------------------------------
//  Set state file fullpath
//  0 - success, -1 - error

int
c_metric_conf_set_statefile (c_metric_conf_t *self, const char *fullpath)
{
    assert (self);
    assert (fullpath);
    zfile_t *file = zfile_new (NULL, fullpath);
    if (!file) {
        log_error ("zfile_new (NULL, '%s') failed.", fullpath);
        return -1;
    }
    bool is_dir = zfile_is_directory (file);
    if (is_dir) {
        log_error ("Specified argument '%s' is a directory.", fullpath);
        zfile_destroy (&file);
        return -1;
    }
    zfile_destroy (&file);
    zstr_free (&self->statefile_name);
    self->statefile_name = strdup (fullpath);
    return 0;
}

//  --------------------------------------------------------------------------
//  Get propagation of sensors in topology

bool
c_metric_conf_propagation (c_metric_conf_t *self)
{
    assert (self);
    return self->is_propagation_needed;
}

//  --------------------------------------------------------------------------
//  Set propagation of sensors in topology

void
c_metric_conf_set_propagation (c_metric_conf_t *self, bool is_propagation_needed)
{
    assert (self);
    self->is_propagation_needed = is_propagation_needed;
}

//  --------------------------------------------------------------------------
//  Get path to configuration directory

const char *
c_metric_conf_cfgdir (c_metric_conf_t *self)
{
    assert (self);
    return self->configuration_dir;
}

//  --------------------------------------------------------------------------
//  Set configuration directory path
//  Directory (full path from FS root) MUST exist and be writable!
//  If directory doesn't exist -> error
//  0 - success, -1 - error

int
c_metric_conf_set_cfgdir (c_metric_conf_t *self, const char *path)
{
    assert (self);
    assert (path);
    zdir_t *dir = zdir_new (path, "-");
    if (!dir) {
        log_error ("zdir_new ('%s', \"-\") failed - full path does not exist.", path);
        return -1;
    }
    zdir_destroy (&dir);
    if(access(path, W_OK) != 0) {
        log_error ("access ('%s') failed - directory is not writable.", path);
        return -1;
    }
    zstr_free (&self->configuration_dir);
    self->configuration_dir = strdup (path);
    log_debug ("Configuration dir is set: '%s'", path);
    return 0;
}

void
c_metric_conf_test (bool verbose)
{
    if ( verbose )
        log_set_level (LOG_DEBUG);

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
    // Note: tests below also assume that "/tmp" exists and is available
    // for manipulation, and that "/lib" and "/root" dirs exist and are
    // not available to the testing user/program.

    printf (" * c_metric_conf: \n");
    //  @selftest
    //  =================================================================
    if ( verbose )
        log_debug ("Test1: Simple create/destroy test");
    c_metric_conf_t *self = c_metric_conf_new ("myname");
    assert (self);

    c_metric_conf_destroy (&self);
    assert (self == NULL);

    c_metric_conf_destroy (&self);
    assert (self == NULL);

    self = c_metric_conf_new ("myname");

    //  =================================================================
    if ( verbose )
        log_debug ("Test2: statefile set/get test");

    const char *state_file = c_metric_conf_statefile (self);
    assert (streq (state_file, ""));
    char *test_state_file = NULL;

    test_state_file = zsys_sprintf ("%s/state_file", SELFTEST_DIR_RW);
    assert (test_state_file != NULL);
    int rv = c_metric_conf_set_statefile (self, test_state_file);
    assert (rv == 0);
    state_file = c_metric_conf_statefile (self);
    assert (streq (state_file, test_state_file));
    zstr_free (&test_state_file);

    rv = c_metric_conf_set_statefile (self, "/tmp/composite-metrics/state_file");
    assert (rv == 0);
    state_file = c_metric_conf_statefile (self);
    assert (streq (state_file, "/tmp/composite-metrics/state_file"));

    test_state_file = zsys_sprintf ("%s/test_dir/state_file", SELFTEST_DIR_RW);
    assert (test_state_file != NULL);
    rv = c_metric_conf_set_statefile (self, test_state_file);
    assert (rv == 0);
    state_file = c_metric_conf_statefile (self);
    assert (streq (state_file, test_state_file));

    // directory (assumed to exist); state_file value should remain the same
    rv = c_metric_conf_set_statefile (self, "/lib");
    assert (rv == -1);
    state_file = c_metric_conf_statefile (self);
    assert (streq (state_file, test_state_file));
    zstr_free (&test_state_file);

    //  =================================================================
    if ( verbose )
        log_debug ("Test3: cfgdir set/get test");
    {
    const char *cfgdir = c_metric_conf_cfgdir (self);
    assert (streq (cfgdir, ""));

    int rv = c_metric_conf_set_cfgdir (self, "/tmp");
    assert (rv == 0);
    cfgdir = c_metric_conf_cfgdir (self);
    assert (streq (cfgdir, "/tmp"));

    if ( 0 == getuid() || 0 == geteuid() ) {
        if (verbose)
            log_debug("Skipping non-writable directory test for an apparently root caller");
    } else {
        if (verbose)
            log_debug("Non-writable directory test (assuming current user is not root)");
        rv = c_metric_conf_set_cfgdir (self, "/etc");
        assert (rv == -1);
        cfgdir = c_metric_conf_cfgdir (self);
        assert (streq (cfgdir, "/tmp"));
    } // if root
    } // scope

    c_metric_conf_destroy (&self);
    //  @end
    printf (" * c_metric_conf: OK\n");
}
