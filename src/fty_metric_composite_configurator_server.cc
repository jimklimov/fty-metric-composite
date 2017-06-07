/*  =========================================================================
    fty_metric_composite_configurator_server - Composite metrics server configurator

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
    fty_metric_composite_configurator_server - Composite metrics server configurator
@discuss
@end
*/
#include <string>
#include <vector>
#include <regex>

#include "fty_metric_composite_classes.h"

// Copied from agent-nut
// -1 - error, subprocess code - success
static int
s_bits_systemctl (const char *operation, const char *service)
{
    assert (operation);
    assert (service);
    log_debug ("calling `sudo systemctl '%s' '%s'`", operation, service);

    std::vector <std::string> _argv = {"sudo", "systemctl", operation, service};

    SubProcess systemd (_argv);
    if (systemd.run()) {
        int result = systemd.wait (false);
        log_info ("sudo systemctl '%s' '%s' result  == %i (%s)",
                  operation, service, result, result == 0 ? "ok" : "failed");
        return result;
    }
    log_error ("can't run sudo systemctl '%s' '%s' command", operation, service);
    return -1;
}

// For each config file in top level of 'path_to_dir' do
//  * systemctl stop and disable of service that uses this file
//  * remove config file
// 0 - success, 1 - failure
static int
s_remove_and_stop (const char *path_to_dir)
{
    assert (path_to_dir);

    zdir_t *dir = zdir_new (path_to_dir, "-");
    if (!dir) {
        log_error ("zdir_new (path = '%s', parent = '-') failed.", path_to_dir);
        return 1;
    }

    zlist_t *files = zdir_list (dir);
    if (!files) {
        zdir_destroy (&dir);
        log_error ("zdir_list () failed.");
        return 1;
    }

    std::regex file_rex (".+\\.cfg");

    zfile_t *item = (zfile_t *) zlist_first (files);
    while (item) {
        if (std::regex_match (zfile_filename (item, path_to_dir), file_rex)) {
            std::string filename = zfile_filename (item, path_to_dir);
            filename.erase (filename.size () - 4);
            std::string service = "fty-metric-composite@";
            service += filename;
            s_bits_systemctl ("stop", service.c_str ());
            s_bits_systemctl ("disable", service.c_str ());
            zfile_remove (item);
            log_debug ("file removed");
        }
        item = (zfile_t *) zlist_next (files);
    }
    zlist_destroy (&files);
    zdir_destroy (&dir);
    return 0;
}

// Write contents to file
// 0 - success, 1 - failure
static int
s_write_file (const char *fullpath, const char *contents)
{
    assert (fullpath);
    assert (contents);

    zfile_t *file = zfile_new (NULL, fullpath);
    if (!file) {
        log_error ("zfile_new (path = NULL, file = '%s') failed.", fullpath);
        return 1;
    }
    if (zfile_output (file) == -1) {
        log_error ("zfile_output () failed; filename = '%s'", zfile_filename (file, NULL));
        zfile_close (file);
        zfile_destroy (&file);
        return 1;
    }
    zchunk_t *chunk = zchunk_new ((const void *) contents, strlen (contents));
    if (!chunk) {
        zfile_close (file);
        zfile_destroy (&file);
        log_error ("zchunk_new () failed");
        return 1;
    }
    int rv = zfile_write (file, chunk, 0);
    zchunk_destroy (&chunk);
    zfile_close (file);
    zfile_destroy (&file);
    if (rv == -1) {
        log_error ("zfile_write () failed");
        return 1;
    }
    return 0;
}

// Generate todo
// 0 - success, 1 - failure
static void
s_generate_and_start (const char *path_to_dir, const char *sensor_function, const char *asset_name, zlistx_t **sensors_p, std::set <std::string> &newMetricsGenerated)
{
    assert (path_to_dir);
    assert (asset_name);
    assert (sensors_p);

    zlistx_t *sensors = *sensors_p;

    if (!sensors) {
        log_error ("parameter '*sensors_p' is NULL");
        return;
    }

    if (zlistx_size (sensors) == 0) {
        zlistx_destroy (sensors_p);
        *sensors_p = NULL;
        return;
    }

    std::string temp_in = "[ ", hum_in = "[ ";

    std::string temp_offsets = "    offsets = {};\n";
    std::string hum_offsets = temp_offsets;
    bool first = true;

    fty_proto_t *item = (fty_proto_t *) zlistx_first (sensors);
    while (item) {
        if (first) {
            first = false;
        }
        else {
            temp_in += ", ";
            hum_in += ", ";
        }
        std::string temp_topic = std::string("temperature.") +
            fty_proto_ext_string (item, "port", "(unknown)") +
            "@" +
            fty_proto_aux_string (item, "parent_name.1", "(unknown)");
        
        temp_in += "\"" + temp_topic + "\"";
        
        
        std::string hum_topic = std::string("humidity.") +
            fty_proto_ext_string (item, "port", "(unknown)") +
            "@" +
            fty_proto_aux_string (item, "parent_name.1", "(unknown)");
        hum_in += "\"" + hum_topic + "\"";

        temp_offsets += "    offsets['" + temp_topic + "'] = " + fty_proto_ext_string (item, "calibration_offset_t", "0.0") + ";\n";
        hum_offsets += "    offsets['" + hum_topic + "'] = " + fty_proto_ext_string (item, "calibration_offset_h", "0.0") + ";\n";

        item = (fty_proto_t *) zlistx_next (sensors);
    }
    zlistx_destroy (sensors_p);
    *sensors_p = NULL;

    temp_in += " ]";
    hum_in += " ]";

    static const char *json_tmpl =
                           "{\n"
                           "\"in\" : ##IN##,\n"
                           "\"evaluation\": \"\n"
                           "##OFFSETS##\n"
                           "    sum = 0;\n"
                           "    num = 0;\n"
                           "    for key,value in pairs(mt) do\n"
                           "        sum = sum + value + offsets[key];\n"
                           "        num = num + 1;\n"
                           "    end;\n"
                           "    if num == 0 then error('all sensors lost'); end;\n"
                           "    tmp = sum / num;\n"
                           "    return '##RESULT_TOPIC##', tmp, '##UNITS##', 0;\"\n"
                           "}\n";
    std::string contents = json_tmpl;

    contents.replace (contents.find ("##IN##"), strlen ("##IN##"), temp_in);
    contents.replace (contents.find ("##OFFSETS##"), strlen ("##OFFSETS##"), temp_offsets);
    std::string qnty = "temperature";
    if (sensor_function) {
        qnty += "-";
        qnty += sensor_function;
    }
    std::string result_topic = "average.";
    result_topic += qnty;
    result_topic += "@";
    result_topic += asset_name;
    contents.replace (contents.find ("##RESULT_TOPIC##"), strlen ("##RESULT_TOPIC##"), result_topic);
    contents.replace (contents.find ("##UNITS##"), strlen ("##UNITS##"), "C");

    // name of the file (service) without extension
    std::string filename = asset_name;
    if (sensor_function) {
        filename += "-";
        filename += sensor_function;
    }
    filename += "-temperature";

    std::string fullpath = path_to_dir;
    fullpath += "/";
    fullpath += filename;
    fullpath += ".cfg";

    std::string service = "fty-metric-composite";
    service += "@";
    service += filename;

    if (s_write_file (fullpath.c_str (), contents.c_str ()) == 0) {
        s_bits_systemctl ("enable", service.c_str ());
        s_bits_systemctl ("start", service.c_str ());
        newMetricsGenerated.insert (result_topic);
    }
    else {
        log_error (
                "Creating config file '%s' failed. Service '%s' not started.",
                fullpath.c_str (), filename.c_str ());
    }

    contents = json_tmpl;
    contents.replace (contents.find ("##IN##"), strlen ("##IN##"), hum_in);
    contents.replace (contents.find ("##OFFSETS##"), strlen ("##OFFSETS##"), hum_offsets);
    qnty = "humidity";
    if (sensor_function) {
        qnty += "-";
        qnty += sensor_function;
    }
    result_topic = "average.";
    result_topic += qnty;
    result_topic += "@";
    result_topic += asset_name;
    contents.replace (contents.find ("##RESULT_TOPIC##"), strlen ("##RESULT_TOPIC##"), result_topic);
    contents.replace (contents.find ("##UNITS##"), strlen ("##UNITS##"), "%");

    // name of the file (service) without extension
    filename = asset_name;
    if (sensor_function) {
        filename += "-";
        filename += sensor_function;
    }
    filename += "-humidity";

    fullpath = path_to_dir;
    fullpath += "/";
    fullpath += filename;
    fullpath += ".cfg";

    service = "fty-metric-composite";
    service += "@";
    service += filename;

    if (s_write_file (fullpath.c_str (), contents.c_str ()) == 0) {
        s_bits_systemctl ("enable", service.c_str ());
        s_bits_systemctl ("start", service.c_str ());
        newMetricsGenerated.insert (result_topic);
    }
    else {
        log_error (
                "Creating config file '%s' failed. Service '%s' not started.",
                fullpath.c_str (), filename.c_str ());
    }
    return;
}

static void
s_regenerate (c_metric_conf_t *cfg, data_t *data, std::set <std::string> &metrics_unavailable)
{
    assert (cfg);
    assert (data);
    // potential unavailable metrics are those, what are now still available
    metrics_unavailable = data_get_produced_metrics (data);
    // 1. Delete all files in output dir and stop/disable services
    int rv = s_remove_and_stop (c_metric_conf_cfgdir (cfg));
    log_info ("Old configuration was removed");
    if (rv != 0) {
        log_error (
                "Error removing old config files from directory '%s'. New config "
                "files were NOT generated and services were NOT started.", c_metric_conf_cfgdir (cfg));
        return;
    }

    // 2. Generate new files and enable/start services
    zlistx_t *assets = data_asset_names (data);
    if (!assets) {
        log_error ("data_asset_names () failed");
        return;
    }
    log_debug ("propagation: %s",  c_metric_conf_propagation (cfg) ? "true": "false");
    data_reassign_sensors (data, c_metric_conf_propagation (cfg));
    log_info ("New configuration was deduced");
    const char *asset = (const char *) zlistx_first (assets);
    std::set <std::string> metricsAvailable;
    while (asset) {
        fty_proto_t *proto = data_asset (data, asset);
        if (streq (fty_proto_aux_string (proto, "type", ""), "rack")) {
            zlistx_t *sensors = NULL;
            // Ti, Hi
            sensors = data_get_assigned_sensors (data, asset, "input");
            if (sensors) {
                s_generate_and_start (c_metric_conf_cfgdir (cfg), "input", asset, &sensors, metricsAvailable);
            }

            // To, Ho
            sensors = data_get_assigned_sensors (data, asset, "output");
            if (sensors) {
                s_generate_and_start (c_metric_conf_cfgdir (cfg), "output", asset, &sensors, metricsAvailable);
            }
        }
        else {
            zlistx_t *sensors = NULL;
            // T, H
            sensors = data_get_assigned_sensors (data, asset, NULL);
            if (sensors) {
                s_generate_and_start (c_metric_conf_cfgdir (cfg), NULL, asset, &sensors, metricsAvailable);
            }
        }
        asset = (const char *) zlistx_next (assets);
    }
    for (const auto &one_metric: metricsAvailable) {
        metrics_unavailable.erase (one_metric);
    }
    data_set_produced_metrics (data, metricsAvailable);
    zlistx_destroy (&assets);
    log_info ("Sensors were reconfigured");
}


//  --------------------------------------------------------------------------
//  composite metrics configurator server

void
fty_metric_composite_configurator_server (zsock_t *pipe, void* args)
{
    assert (pipe);
    assert (args);

    c_metric_conf_t *cfg = c_metric_conf_new ((const char *) args);
    assert (cfg);

    data_t *data = data_new ();
    assert (data);

    zpoller_t *poller = zpoller_new (pipe, mlm_client_msgpipe ( c_metric_conf_client (cfg)), NULL);
    if (!poller) {
        log_error ("zpoller_new () failed");
        c_metric_conf_destroy (&cfg);
        data_destroy (&data);
        return;
    }

    zsock_signal (pipe, 0);

    uint64_t timestamp = (uint64_t) zclock_mono ();
    uint64_t timeout = (uint64_t) 30000;

    while (!zsys_interrupted) {
        void *which = zpoller_wait (poller, timeout);

        if (which == NULL) {
            if (zpoller_terminated (poller) || zsys_interrupted) {
                log_warning (
                    "zpoller_terminated () == '%s' or zsys_interrupted == '%s'",
                    zpoller_terminated (poller) ? "true" : "false", zsys_interrupted ? "true" : "false");
                break;
            }
            if (zpoller_expired (poller)) {
                if (data_is_reconfig_needed (data)) {
                    std::set <std::string> metrics_unavailable;
                    s_regenerate (cfg, data, metrics_unavailable);
                    data_save (data, c_metric_conf_statefile (cfg));
                    for (const auto &one_metric: metrics_unavailable) {
                        proto_metric_unavailable_send (c_metric_conf_client (cfg), one_metric.c_str ());
                    }
                }
            }
            timestamp = (uint64_t) zclock_mono ();
            continue;
        }

        if (which == pipe) {
            zmsg_t *message = zmsg_recv (pipe);
            if (!message) {
                log_error ("Given `which == pipe`, function `zmsg_recv (pipe)` returned NULL");
                continue;
            }
            bool old_is_propagation_needed = c_metric_conf_propagation (cfg);
            if (actor_commands (cfg, &data, &message) == 1) {
                break;
            }
            // This is UGLY hack, because there is a need to call s_regenerate from actor commands in some cases
            // but s_regenerate is satic function here!
            if (old_is_propagation_needed != c_metric_conf_propagation (cfg)) {
                // so, we need to regenerate configuration according new reality
                std::set <std::string> metrics_unavailable;
                s_regenerate (cfg, data, metrics_unavailable);
                data_save (data, c_metric_conf_statefile (cfg));
                for (const auto &one_metric: metrics_unavailable ) {
                    proto_metric_unavailable_send (c_metric_conf_client (cfg), one_metric.c_str ());
                }
            }
            continue;
        }

        uint64_t now = (uint64_t) zclock_mono ();
        if (now - timestamp >= timeout) {
             if (data_is_reconfig_needed (data)) {
                std::set <std::string> metrics_unavailable;
                s_regenerate (cfg, data, metrics_unavailable);
                data_save (data, c_metric_conf_statefile (cfg));
                for (const auto &one_metric: metrics_unavailable) {
                    proto_metric_unavailable_send (c_metric_conf_client (cfg), one_metric.c_str ());
                }
            }
            timestamp = (uint64_t) zclock_mono ();
        }

        if (which != mlm_client_msgpipe (c_metric_conf_client (cfg))) {
            log_error ("which was checked for NULL, pipe and now should have been `mlm_client_msgpipe ()` but is not.");
            continue;
        }

        zmsg_t *message = mlm_client_recv (c_metric_conf_client (cfg));
        if (!message) {
            log_error ("Given `which == mlm_client_msgpipe ()`, function `mlm_client_recv ()` returned NULL");
            continue;
        }

        const char *command = mlm_client_command (c_metric_conf_client (cfg));
        if (streq (command, "STREAM DELIVER")) {
            fty_proto_t *proto = fty_proto_decode (&message);
            if (!proto) {
                log_error (
                        "fty_proto_decode () failed; sender = '%s', subject = '%s'",
                        mlm_client_sender (c_metric_conf_client (cfg)), mlm_client_subject (c_metric_conf_client (cfg)));
                continue;
            }
            bool is_stored = data_asset_store (data, &proto);
            if (is_stored) {
                data_save (data, c_metric_conf_statefile (cfg));
            }
            assert (proto == NULL);
        }
        else
        if (streq (command, "MAILBOX DELIVER") ||
            streq (command, "SERVICE DELIVER")) {
            log_warning (
                    "Received a message from sender = '%s', command = '%s', subject = '%s'. Throwing away.",
                    mlm_client_sender (c_metric_conf_client (cfg)),
                    mlm_client_command (c_metric_conf_client (cfg)),
                    mlm_client_subject (c_metric_conf_client (cfg)));
            continue;
        }
        else {
            log_error ("Unrecognized mlm_client_command () = '%s'", command ? command : "(null)");
        }
        zmsg_destroy (&message);
    }
    data_save (data, c_metric_conf_statefile (cfg));
    zpoller_destroy (&poller);
    c_metric_conf_destroy (&cfg);
    data_destroy (&data);
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
//  Test directory contents for expected files
//  0 - ok, 1 - failure

static int
test_dir_contents (
        const std::string& directory,
        std::vector <std::string>& expected)
{
    printf ("test_dir_contents (): start\n");
    zdir_t *dir = zdir_new (directory.c_str (), "-");
    assert (dir);

    zlist_t *files = zdir_list (dir);
    assert (files);

    std::regex file_rex (".+\\.cfg");

    zfile_t *item = (zfile_t *) zlist_first (files);
    while (item) {
        if (std::regex_match (zfile_filename (item, directory.c_str ()), file_rex)) {
            bool found = false;
            for (auto it = expected.begin (); it != expected.end (); it++)
            {
                if (it->compare (zfile_filename (item, directory.c_str ())) == 0) {
                    expected.erase (it);
                    found = true;
                    break;
                }
            }
            if (!found) {
                printf ("Filename '%s' present in directory but not expected.\n", zfile_filename (item, directory.c_str ()));
                zlist_destroy (&files);
                zdir_destroy (&dir);
                return 1;
            }
        }
        item = (zfile_t *) zlist_next (files);
    }
    zlist_destroy (&files);
    zdir_destroy (&dir);
    if (expected.size () != 0) {
        for ( const auto &file_name : expected ) {
            log_error ("'%s' expected, but not found", file_name.c_str());
        }
        return 1;
    }
    return 0;
}


// Improvement memo: make expected_configs a string->string map and check the file contents
//                   as well (or parse json).

void
fty_metric_composite_configurator_server_test (bool verbose)
{
    if ( verbose ) 
        log_set_level (LOG_DEBUG);
    static const char* endpoint = "inproc://bios-composite-configurator-server-test";

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

    char *test_state_file = zsys_sprintf ("%s/test_state_file", SELFTEST_DIR_RW);
    assert (test_state_file != NULL);

    printf (" * fty_metric_composite_configurator_server: ");
    if (verbose)
        printf ("\n");

    //  @selftest

    zactor_t *server = zactor_new (mlm_server, (void*) "Malamute");
    zstr_sendx (server, "BIND", endpoint, NULL);
    zclock_sleep (100);

    mlm_client_t *producer = mlm_client_new ();
    mlm_client_connect (producer, endpoint, 1000, "producer");
    mlm_client_set_producer (producer, "ASSETS");
    zclock_sleep (100);

    mlm_client_t *alert_generator = mlm_client_new ();
    mlm_client_connect (alert_generator, endpoint, 1000, "alert_generator");
    mlm_client_set_consumer (alert_generator, "_METRICS_UNAVAILABLE", ".*");
    zclock_sleep (100);

    zactor_t *configurator = zactor_new (fty_metric_composite_configurator_server, (void*) "configurator");
    assert (configurator);
    zclock_sleep (100);
    // As directory MUST exist -> create in advance!
    int r = system ("mkdir -p ./test_dir");
    assert ( r != -1 ); // make debian g++ happy
    zstr_sendx (configurator, "CFG_DIRECTORY", "./test_dir", NULL);
    zstr_sendx (configurator, "STATE_FILE", test_state_file, NULL);
    zstr_sendx (configurator, "CONNECT", endpoint, NULL);
    zstr_sendx (configurator, "CONSUMER", "ASSETS", ".*", NULL);
    zstr_sendx (configurator, "PRODUCER", "_METRICS_UNAVAILABLE", ".*", NULL);
    zclock_sleep (500);

    fty_proto_t *asset = NULL;

    printf ("TRACE CREATE DC-Rozskoky\n");
    asset = test_asset_new ("DC-Rozskoky", FTY_PROTO_ASSET_OP_CREATE); // 1
    fty_proto_aux_insert (asset, "parent", "%s", "0");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "datacenter");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    fty_proto_ext_insert (asset, "max_power" , "%s",  "2");
    zmsg_t *zmessage = fty_proto_encode (&asset);
    int rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Lazer game\n");
    asset = test_asset_new ("Lazer game", FTY_PROTO_ASSET_OP_CREATE); // 2
    fty_proto_aux_insert (asset, "parent", "%s", "1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "room");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Curie\n");
    asset = test_asset_new ("Curie", FTY_PROTO_ASSET_OP_CREATE); // 3
    fty_proto_aux_insert (asset, "parent", "%s", "1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "room");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Lazer game.Row01\n");
    asset = test_asset_new ("Lazer game.Row01", FTY_PROTO_ASSET_OP_CREATE); // 4
    fty_proto_aux_insert (asset, "parent", "%s", "2");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "row");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    // testing situation when sensor asset message arrives before asset specified in logical_asset
    printf ("TRACE CREATE Sensor01\n");
    asset = test_asset_new ("Sensor01", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH1");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "10");
    fty_proto_ext_insert (asset, "vertical_position", "%s", "bottom");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Rack01\n");
    asset = test_asset_new ("Rack01", FTY_PROTO_ASSET_OP_CREATE); // 5
    fty_proto_aux_insert (asset, "parent", "%s", "4");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    fty_proto_ext_insert (asset, "u_size" , "%s",  "42");
    fty_proto_ext_insert (asset, "description" , "%s",  "Lorem ipsum asd asd asd asd asd asd asd");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Rack02\n");
    asset = test_asset_new ("Rack02", FTY_PROTO_ASSET_OP_CREATE); // 6
    fty_proto_aux_insert (asset, "parent", "%s", "4");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    fty_proto_ext_insert (asset, "u_size" , "%s",  "42");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Curie.Row01\n");
    asset = test_asset_new ("Curie.Row01", FTY_PROTO_ASSET_OP_CREATE); // 7
    fty_proto_aux_insert (asset, "parent", "%s", "3");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "row");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Curie.Row02\n");
    asset = test_asset_new ("Curie.Row02", FTY_PROTO_ASSET_OP_CREATE); // 8
    fty_proto_aux_insert (asset, "parent", "%s", "3");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "row");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Rack03\n");
    asset = test_asset_new ("Rack03", FTY_PROTO_ASSET_OP_CREATE); // 9
    fty_proto_aux_insert (asset, "parent", "%s", "7");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    fty_proto_ext_insert (asset, "u_size" , "%s",  "42");
    fty_proto_ext_insert (asset, "description" , "%s",  "Lorem ipsum asd asd asd asd asd asd asd");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (2000);

    printf ("TRACE CREATE Rack04\n");
    asset = test_asset_new ("Rack04", FTY_PROTO_ASSET_OP_CREATE); // 10
    fty_proto_aux_insert (asset, "parent", "%s", "8");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "rack");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    fty_proto_ext_insert (asset, "u_size" , "%s",  "42");
    fty_proto_ext_insert (asset, "description" , "%s",  "Lorem ipsum asd asd asd asd asd asd asd");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Rack01.ups1\n");
    asset = test_asset_new ("Rack01.ups1", FTY_PROTO_ASSET_OP_CREATE); // 11
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "ups");
    fty_proto_aux_insert (asset, "parent", "%s", "5");
    fty_proto_ext_insert (asset, "abc.d", "%s", " ups string 1");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Sensor02\n");
    asset = test_asset_new ("Sensor02", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH2");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "2");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "20");
    fty_proto_ext_insert (asset, "vertical_position", "%s", "bottom");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Sensor03\n");
    asset = test_asset_new ("Sensor03", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH3");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    fty_proto_ext_insert (asset, "vertical_position", "%s", "middle");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (100);

    // The following 4 sensors have important info missing
    printf ("TRACE CREATE Sensor04\n");
    asset = test_asset_new ("Sensor04", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH3");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    fty_proto_ext_insert (asset, "vertical_position", "%s", "middle");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    // logical_asset missing
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Sensor05\n");
    asset = test_asset_new ("Sensor05", FTY_PROTO_ASSET_OP_CREATE);
    // parent missing
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH3");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    fty_proto_ext_insert (asset, "vertical_position", "%s", "middle");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Sensor06\n");
    asset = test_asset_new ("Sensor06", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    // parent_name.1 missing
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH3");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    fty_proto_ext_insert (asset, "vertical_position", "%s", "middle");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Sensor07\n");
    asset = test_asset_new ("Sensor07", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    // port missing
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    fty_proto_ext_insert (asset, "vertical_position", "%s", "middle");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Sensor08\n");
    asset = test_asset_new ("Sensor08", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH4");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "1");
    fty_proto_ext_insert (asset, "vertical_position", "%s", "bottom");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Rack02");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Sensor09\n");
    asset = test_asset_new ("Sensor09", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH5");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "2.0");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "2.0");
    fty_proto_ext_insert (asset, "vertical_position", "%s", "top");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "output");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Rack02");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Sensor10\n");
    asset = test_asset_new ("Sensor10", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH6");
    fty_proto_ext_insert (asset, "vertical_position", "%s", "top");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "output");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Sensor11\n");
    asset = test_asset_new ("Sensor11", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH7");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "15.5");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "20");
    fty_proto_ext_insert (asset, "vertical_position", "%s", "top");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "output");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Sensor12\n");
    asset = test_asset_new ("Sensor12", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH8");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "0");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "0");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "neuvedeno");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Sensor13\n");
    asset = test_asset_new ("Sensor13", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH9");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "-1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "1");
    fty_proto_ext_insert (asset, "vertical_position", "%s", "top");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Curie.Row02");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Sensor14\n");
    asset = test_asset_new ("Sensor14", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH10");
    fty_proto_ext_insert (asset, "vertical_position", "%s", "bottom");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Curie");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Sensor15\n");
    asset = test_asset_new ("Sensor15", FTY_PROTO_ASSET_OP_CREATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH11");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1.4");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "-1");
    fty_proto_ext_insert (asset, "vertical_position", "%s", "middle");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "output");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Curie.Row02");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE ---===### (Test block -1-) ###===---\n");
    {
        printf ("Sleeping 1m for configurator kick in and finish\n");
        zclock_sleep (60000); // magical constant

        std::vector <std::string> expected_configs = {
            "Rack01-input-temperature.cfg",
            "Rack01-input-humidity.cfg",
            "Rack01-output-temperature.cfg",
            "Rack01-output-humidity.cfg",
            "Rack02-input-temperature.cfg",
            "Rack02-input-humidity.cfg",
            "Rack02-output-temperature.cfg",
            "Rack02-output-humidity.cfg" 
            // BIOS-2484: sensors assigned to non-racks are ignored
//,
//            "Curie.Row02-temperature.cfg",
//            "Curie.Row02-humidity.cfg",
//            "Curie-temperature.cfg",
//            "Curie-humidity.cfg"
        };

        int rv = test_dir_contents ("./test_dir", expected_configs);
        printf ("rv == %d\n", rv);
        assert (rv == 0);
        printf ("Test block -1- Ok\n");
    }

    printf ("TRACE CREATE ups2\n");
    asset = test_asset_new ("ups2", FTY_PROTO_ASSET_OP_CREATE); // 12
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "ups");
    fty_proto_aux_insert (asset, "parent", "%s", "10");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE UPDATE Sensor01\n");
    asset = test_asset_new ("Sensor01", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH1");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "-5.2");
    fty_proto_ext_insert (asset, "vertical_position", "%s", "bottom");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE UPDATE Sensor02\n");
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
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE UPDATE Sensor03\n");
    asset = test_asset_new ("Sensor03", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH3");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "output");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE UPDATE Sensor10\n");
    asset = test_asset_new ("Sensor10", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH2");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "-0.16");
    fty_proto_ext_insert (asset, "vertical_position", "%s", "top");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "output");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE RETIRE Sensor11\n");
    asset = test_asset_new ("Sensor11", FTY_PROTO_ASSET_OP_RETIRE);
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE UPDATE Sensor08\n");
    asset = test_asset_new ("Sensor08", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH4");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "2.0");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "12");
    fty_proto_ext_insert (asset, "vertical_position", "%s", "middle");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "input");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "DC-Rozskoky");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE UPDATE Sensor09\n");
    asset = test_asset_new ("Sensor09", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH5");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "5");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "50");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Curie.Row02");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE UPDATE Sensor04\n");
    asset = test_asset_new ("Sensor04", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH12");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "1");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Lazer game");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE UPDATE Sensor05\n");
    asset = test_asset_new ("Sensor05", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH13");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "4");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "-6");
    fty_proto_ext_insert (asset, "vertical_position", "%s", "top");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "output");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "DC-Rozskoky");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE UPDATE Sensor06\n");
    asset = test_asset_new ("Sensor06", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH14");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "-1.2");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "-1.4");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "output");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Lazer game");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE UPDATE Sensor07\n");
    asset = test_asset_new ("Sensor07", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH15");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "4");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "-25");
    fty_proto_ext_insert (asset, "sensor_function", "%s", "ambient");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Curie.Row02");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE DELETE Sensor12\n");
    asset = test_asset_new ("Sensor12", FTY_PROTO_ASSET_OP_DELETE);
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE UPDATE Sensor14\n");
    asset = test_asset_new ("Sensor14", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent", "%s", "12");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "ups2");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH10");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "This-asset-does-not-exist");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE UPDATE Sensor15\n");
    asset = test_asset_new ("Sensor15", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent", "%s", "11");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH11");
    fty_proto_ext_insert (asset, "calibration_offset_t", "%s", "2.1");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "-3.3");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "Curie");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE DELETE Sensor13\n");
    asset = test_asset_new ("Sensor13", FTY_PROTO_ASSET_OP_DELETE);
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE ---===### (Test block -2-) ###===---\n");
    {
        printf ("Sleeping 1m for configurator kick in and finish\n");
        zclock_sleep (60000);

        std::vector <std::string> expected_configs = {
            "Rack01-input-temperature.cfg",
            "Rack01-input-humidity.cfg",
            "Rack01-output-temperature.cfg",
            "Rack01-output-humidity.cfg"
            // BIOS-2484: sensors assigned to non-racks are ignored
//,
//            "DC-Rozskoky-temperature.cfg",
//            "DC-Rozskoky-humidity.cfg",
//            "Lazer game-temperature.cfg",
//            "Lazer game-humidity.cfg"
//            "Curie.Row02-temperature.cfg",
//            "Curie.Row02-humidity.cfg",
//            "Curie-temperature.cfg",
//            "Curie-humidity.cfg"
        };

        int rv = test_dir_contents ("./test_dir", expected_configs);
        printf ("rv == %d\n", rv);
        assert (rv == 0);

        zlistx_t *expected_unavailable = zlistx_new ();
        zlistx_set_duplicator (expected_unavailable, (czmq_duplicator *) strdup);
        zlistx_set_destructor (expected_unavailable, (czmq_destructor *) zstr_free);
        zlistx_set_comparator (expected_unavailable, (czmq_comparator *) strcmp);


        zlistx_add_end (expected_unavailable, (void *) "average.humidity-input@Rack02");
        zlistx_add_end (expected_unavailable, (void *) "average.temperature-input@Rack02");
        zlistx_add_end (expected_unavailable, (void *) "average.humidity-output@Rack02");
        zlistx_add_end (expected_unavailable, (void *) "average.temperature-output@Rack02");

        while (zlistx_size (expected_unavailable) != 0) {
            zmsg_t *message = mlm_client_recv (alert_generator);
            assert (message);
            assert (streq (mlm_client_subject (alert_generator), "metric_topic"));

            char *part = zmsg_popstr (message);
            assert (part);
            assert (streq (part, "METRICUNAVAILABLE"));
            zstr_free (&part);

            part = zmsg_popstr (message);
            assert (part);
            printf ("Got metric unavailable topic '%s' ... ", part);
            void *handle = zlistx_find (expected_unavailable, (void *) part);
            assert (handle);
            printf ("It's OK.\n");
            zlistx_delete (expected_unavailable, handle);

            zstr_free (&part);
            zmsg_destroy (&message);
        }


        zlistx_destroy (&expected_unavailable);

        printf ("Test block -2- Ok\n");
    }

    printf ("TRACE DELETE Sensor15\n");
    asset = test_asset_new ("Sensor15", FTY_PROTO_ASSET_OP_DELETE);
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE DELETE Curie.Row02\n");
    asset = test_asset_new ("Curie.Row02", FTY_PROTO_ASSET_OP_DELETE);
    fty_proto_aux_insert (asset, "type", "%s", "row");
    fty_proto_aux_insert (asset, "subtype", "%s", "unknown");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE Sensor16\n");
    asset = test_asset_new ("Sensor16", FTY_PROTO_ASSET_OP_UPDATE);
    fty_proto_aux_insert (asset, "parent", "%s", "13");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "nas rack controller");
    fty_proto_aux_insert (asset, "status", "%s", "active");
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "sensor");
    fty_proto_ext_insert (asset, "port", "%s", "TH2");
    fty_proto_ext_insert (asset, "calibration_offset_h", "%s", "-3.51");
    fty_proto_ext_insert (asset, "logical_asset", "%s", "DC-Rozskoky");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);

    printf ("TRACE CREATE nas rack controller\n");
    asset = test_asset_new ("nas rack controller", FTY_PROTO_ASSET_OP_CREATE); // 12
    fty_proto_aux_insert (asset, "type", "%s", "device");
    fty_proto_aux_insert (asset, "subtype", "%s", "rack controller");
    fty_proto_aux_insert (asset, "parent", "%s", "5");
    fty_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01");
    zmessage = fty_proto_encode (&asset);
    rv = mlm_client_send (producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (50);
/* BIOS-2484: sensors for NON racks are ignored -> this block is not relevant
    printf ("TRACE ---===### (Test block -3-) ###===---\n");
    {
        printf ("Sleeping 1m for configurator kick in and finish\n");
        zclock_sleep (60000);

        std::vector <std::string> expected_configs = {
            "Rack01-input-temperature.cfg",
            "Rack01-input-humidity.cfg",
            "Rack01-output-temperature.cfg",
            "Rack01-output-humidity.cfg",
            "DC-Rozskoky-temperature.cfg",
            "DC-Rozskoky-humidity.cfg",
            "Lazer game-temperature.cfg",
            "Lazer game-humidity.cfg"
        };

        int rv = test_dir_contents ("./test_dir", expected_configs);
        printf ("rv == %d\n", rv);
        assert (rv == 0);

        zlistx_t *expected_unavailable = zlistx_new ();
        zlistx_set_duplicator (expected_unavailable, (czmq_duplicator *) strdup);
        zlistx_set_destructor (expected_unavailable, (czmq_destructor *) zstr_free);
        zlistx_set_comparator (expected_unavailable, (czmq_comparator *) strcmp);

        zlistx_add_end (expected_unavailable, (void *) "average.temperature@Curie");
        zlistx_add_end (expected_unavailable, (void *) "average.humidity@Curie");
        zlistx_add_end (expected_unavailable, (void *) "average.temperature@Curie.Row02");
        zlistx_add_end (expected_unavailable, (void *) "average.humidity@Curie.Row02");

        while (zlistx_size (expected_unavailable) != 0) {
            zmsg_t *message = mlm_client_recv (alert_generator);
            assert (message);
            assert (streq (mlm_client_subject (alert_generator), "metric_topic"));

            char *part = zmsg_popstr (message);
            assert (part);
            assert (streq (part, "METRICUNAVAILABLE"));
            zstr_free (&part);

            part = zmsg_popstr (message);
            assert (part);
            printf ("Got metric unavailable topic '%s' ... ", part);
            void *handle = zlistx_find (expected_unavailable, (void *) part);
            assert (handle);
            printf ("It's OK.\n");
            zlistx_delete (expected_unavailable, handle);

            zstr_free (&part);
            zmsg_destroy (&message);
        }
        zlistx_destroy (&expected_unavailable);

        printf ("Test block -3- Ok\n");
    }*/

    zstr_free (&test_state_file);
    mlm_client_destroy (&producer);
    mlm_client_destroy (&alert_generator);
    zactor_destroy (&configurator);
    zactor_destroy (&server);
    //  @end
    printf ("OK\n");
}
