/*  =========================================================================
    actor_commands - actor commands

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
    actor_commands - actor commands
@discuss
@end
*/

#include "fty_metric_composite_classes.h"

int
actor_commands (
        c_metric_conf_t *cfg,
        data_t **data_p,
        zmsg_t **message_p)
{
    assert (message_p && *message_p);
    assert (cfg);
    assert (data_p && *data_p);

    zmsg_t *message = *message_p;

    char *cmd = zmsg_popstr (message);
    if (!cmd) {
        log_error (
                "Given `which == pipe` function `zmsg_popstr (msg)` returned NULL. "
                "Message received is most probably empty (has no frames).");
        zmsg_destroy (message_p);
        return 0;
    }

    int ret = 0;
    log_debug ("actor command = '%s'", cmd);
    if (streq (cmd, "$TERM")) {
        log_info ("Got $TERM");
        ret = 1;
    }
    else
    if (streq (cmd, "CONNECT")) {
        char *endpoint = zmsg_popstr (message);
        if (!endpoint) {
            log_error (
                    "Expected multipart string format: CONNECT/endpoint. "
                    "Received CONNECT/nullptr");
            zstr_free (&cmd);
            zmsg_destroy (message_p);
            return 0;
        }
        int rv = mlm_client_connect (c_metric_conf_client (cfg), endpoint, 1000, c_metric_conf_name (cfg));
        if (rv == -1) {
            log_error (
                    "mlm_client_connect (endpoint = '%s', timeout = '%d', address = '%s') failed",
                    endpoint, 1000, c_metric_conf_name (cfg));
        }
        zstr_free (&endpoint);
    }
    else
    if (streq (cmd, "PRODUCER")) {
        char *stream = zmsg_popstr (message);
        if (!stream) {
            log_error (
                    "Expected multipart string format: PRODUCER/stream. "
                    "Received PRODUCER/nullptr");
            zstr_free (&stream);
            zstr_free (&cmd);
            zmsg_destroy (message_p);
            return 0;
        }
        int rv = mlm_client_set_producer (c_metric_conf_client (cfg), stream);
        if (rv == -1) {
            log_error ("mlm_client_set_producer (stream = '%s') failed", stream);
        }
        zstr_free (&stream);
    }
    else
    if (streq (cmd, "CONSUMER")) {
        char *stream = zmsg_popstr (message);
        if (!stream) {
            log_error (
                    "Expected multipart string format: CONSUMER/stream/pattern. "
                    "Received CONSUMER/nullptr");
            zstr_free (&cmd);
            zmsg_destroy (message_p);
            return 0;
        }
        char *pattern = zmsg_popstr (message);
        if (!pattern) {
            log_error (
                    "Expected multipart string format: CONSUMER/stream/pattern. "
                    "Received CONSUMER/stream/nullptr");
            zstr_free (&stream);
            zstr_free (&cmd);
            zmsg_destroy (message_p);
            return 0;
        }
        int rv = mlm_client_set_consumer (c_metric_conf_client (cfg), stream, pattern);
        if (rv == -1) {
            log_error (
                    "mlm_client_set_consumer (stream = '%s', pattern = '%s') failed",
                    stream, pattern);
        }
        zstr_free (&pattern);
        zstr_free (&stream);
    }
    else
    if (streq (cmd, "STATE_FILE")) {
        char *state_file = zmsg_popstr (message);
        if (!state_file) {
            log_error (
                    "Expected multipart string format: STATE_FILE/state_file."
                    "Received STATE_FILE/nullptr");
            zstr_free (&cmd);
            zmsg_destroy (message_p);
            return 0;
        }
        c_metric_conf_set_statefile (cfg, state_file);
        zstr_free (&state_file);
    } else
    if (streq (cmd, "IS_PROPAGATION_NEEDED")) {
        char *answer = zmsg_popstr (message);
        if (!answer) {
            log_error (
                    "Expected multipart string format: IS_PROPAGATION_NEEDED/answer."
                    "Received IS_PROPAGATION_NEEDED/nullptr");
            zstr_free (&cmd);
            zmsg_destroy (message_p);
            return 0;
        }
        if ( streq (answer, "true") ) {
            c_metric_conf_set_propagation (cfg, true);
        } else {
            c_metric_conf_set_propagation (cfg, false);
        }
        zstr_free (&answer);
    }
    else
    if (streq (cmd, "LOAD")) {
        if (streq (c_metric_conf_statefile (cfg), "")) {
            log_error (
                    "State file: '' not loaded (name of statefile is not specified yet).");
            zstr_free (&cmd);
            zmsg_destroy (message_p);
            return 0;
        }
        data_t *new_data = data_load (c_metric_conf_statefile (cfg));
        if (new_data == NULL) {
            log_error (
                    "State file: '%s' not loaded (error during load).", c_metric_conf_statefile (cfg));
            zstr_free (&cmd);
            zmsg_destroy (message_p);
            return 0;
        }
        data_destroy (data_p);
        *data_p = new_data;
        log_info ("State file: '%s' loaded successfully", c_metric_conf_statefile (cfg));
    }
    else
    if (streq (cmd, "CFG_DIRECTORY")) {
        char *cfgdir = zmsg_popstr (message);
        if (!cfgdir) {
            log_error (
                    "Expected multipart string format: CFG_DIRECTORY/cfg_directory."
                    "Received CFG_DIRECTORY/nullptr");
            zstr_free (&cmd);
            zmsg_destroy (message_p);
            return 0;
        }
        c_metric_conf_set_cfgdir (cfg, cfgdir);
        zstr_free (&cfgdir);
    }
    else {
        log_warning ("Command '%s' is unknown or not implemented", cmd);
    }

    zstr_free (&cmd);
    zmsg_destroy (message_p);
    return ret;
}


//  --------------------------------------------------------------------------
//  Self test of this class

void
actor_commands_test (bool verbose)
{
    ManageFtyLog::setInstanceFtylog ("actor-commands-test", "");
    if ( verbose )
        ManageFtyLog::getInstanceFtylog()->setVeboseMode();
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

    //  @selftest
    static const char* endpoint = "ipc://bios-actor-commands-test";
    // malamute broker
    zactor_t *malamute = zactor_new (mlm_server, (void*) "Malamute");
    assert (malamute);
    if (verbose)
        zstr_send (malamute, "VERBOSE");
    zstr_sendx (malamute, "BIND", endpoint, NULL);

    c_metric_conf_t *cfg = c_metric_conf_new ("mlm_name_is_I_AM_REACH");
    assert (cfg);
    data_t *data = data_new ();

    zmsg_t *message = NULL;
    // empty message - expected fail
    message = zmsg_new ();
    assert (message);
    int rv = actor_commands (cfg, &data, &message);
    assert (rv == 0);
    assert (message == NULL);

    assert (streq (c_metric_conf_statefile (cfg), ""));
    assert (streq (c_metric_conf_cfgdir (cfg), ""));
    // --------------------------------------------------------------
    // empty string - expected fail
    message = zmsg_new ();
    assert (message);
    zmsg_addstr (message, "");
    rv = actor_commands (cfg, &data, &message);
    assert (rv == 0);
    assert (message == NULL);
    assert (streq (c_metric_conf_statefile (cfg), ""));
    assert (streq (c_metric_conf_cfgdir (cfg), ""));

    // --------------------------------------------------------------
    // unknown command - expected fail
    message = zmsg_new ();
    assert (message);
    zmsg_addstr (message, "MAGIC!");
    rv = actor_commands (cfg, &data, &message);
    assert (rv == 0);
    assert (message == NULL);
    assert (streq (c_metric_conf_statefile (cfg), ""));
    assert (streq (c_metric_conf_cfgdir (cfg), ""));

    // --------------------------------------------------------------
    // STATE_FILE - expected fail
    message = zmsg_new ();
    assert (message);
    zmsg_addstr (message, "STATE_FILE");
    // missing config_file here
    rv = actor_commands (cfg, &data, &message);
    assert (rv == 0);
    assert (message == NULL);
    assert (streq (c_metric_conf_statefile (cfg), ""));
    assert (streq (c_metric_conf_cfgdir (cfg), ""));

    // --------------------------------------------------------------
    // STATE_FILE - expected fail
    message = zmsg_new ();
    assert (message);
    zmsg_addstr (message, "STATE_FILE");
    zmsg_addstr (message, "."); // supplied path is a directory
    rv = actor_commands (cfg, &data, &message);
    assert (rv == 0);
    assert (message == NULL);
    assert (streq (c_metric_conf_statefile (cfg), ""));
    assert (streq (c_metric_conf_cfgdir (cfg), ""));

    // --------------------------------------------------------------
    // CFG_DIRECTORY - expected fail
    message = zmsg_new ();
    assert (message);
    zmsg_addstr (message, "CFG_DIRECTORY");
    // missing config_file here
    rv = actor_commands (cfg, &data, &message);
    assert (rv == 0);
    assert (message == NULL);
    assert (streq (c_metric_conf_statefile (cfg), ""));
    assert (streq (c_metric_conf_cfgdir (cfg), ""));

    // --------------------------------------------------------------
    // CFG_DIRECTORY - expected fail
    message = zmsg_new ();
    assert (message);
    zmsg_addstr (message, "CFG_DIRECTORY");
    zmsg_addstr (message, "/etc/passwd"); // supplied path is not a directory
    rv = actor_commands (cfg, &data, &message);
    assert (rv == 0);
    assert (message == NULL);
    assert (streq (c_metric_conf_statefile (cfg), ""));
    assert (streq (c_metric_conf_cfgdir (cfg), ""));

    // --------------------------------------------------------------
    // CFG_DIRECTORY - expected fail
    message = zmsg_new ();
    assert (message);
    zmsg_addstr (message, "CFG_DIRECTORY");
    zmsg_addstr (message, "/sdssdf/sfef//sdfe"); // non-existing path
    rv = actor_commands (cfg, &data, &message);
    assert (rv == 0);
    assert (message == NULL);
    assert (streq (c_metric_conf_statefile (cfg), ""));
    assert (streq (c_metric_conf_cfgdir (cfg), ""));

    // --------------------------------------------------------------
    // CONNECT - expected fail
    message = zmsg_new ();
    assert (message);
    zmsg_addstr (message, "CONNECT");
    // missing endpoint here
    // missing name here
    rv = actor_commands (cfg, &data, &message);
    assert (rv == 0);
    assert (message == NULL);

    // --------------------------------------------------------------
    // CONNECT - expected fail; bad endpoint
    message = zmsg_new ();
    assert (message);
    zmsg_addstr (message, "CONNECT");
    zmsg_addstr (message, "ipc://bios-smtp-server-BAD");
    rv = actor_commands (cfg, &data, &message);
    assert (rv == 0);
    assert (message == NULL);

    // --------------------------------------------------------------
    // CONSUMER - expected fail
    message = zmsg_new ();
    assert (message);
    zmsg_addstr (message, "CONSUMER");
    zmsg_addstr (message, "some-stream");
    // missing pattern here
    rv = actor_commands (cfg, &data, &message);
    assert (rv == 0);
    assert (message == NULL);

    // --------------------------------------------------------------
    // CONSUMER - expected fail
    message = zmsg_new ();
    assert (message);
    zmsg_addstr (message, "CONSUMER");
    // missing stream here
    // missing pattern here
    rv = actor_commands (cfg, &data, &message);
    assert (rv == 0);
    assert (message == NULL);

    // --------------------------------------------------------------
    // PRODUCER - expected fail
    message = zmsg_new ();
    assert (message);
    zmsg_addstr (message, "PRODUCER");
    // missing stream here
    rv = actor_commands (cfg, &data, &message);
    assert (rv == 0);
    assert (message == NULL);

    // --------------------------------------------------------------
    // $TERM
    message = zmsg_new ();
    assert (message);
    zmsg_addstr (message, "$TERM");
    rv = actor_commands (cfg, &data, &message);
    assert (rv == 1);
    assert (message == NULL);
    assert (streq (c_metric_conf_statefile (cfg), ""));
    assert (streq (c_metric_conf_cfgdir (cfg), ""));

    // CONNECT
    message = zmsg_new ();
    assert (message);
    zmsg_addstr (message, "CONNECT");
    zmsg_addstr (message, endpoint);
    rv = actor_commands (cfg, &data, &message);
    assert (rv == 0);
    assert (message == NULL);
    assert (streq (c_metric_conf_statefile (cfg), ""));
    assert (streq (c_metric_conf_cfgdir (cfg), ""));

    // CONSUMER
    message = zmsg_new ();
    assert (message);
    zmsg_addstr (message, "CONSUMER");
    zmsg_addstr (message, "some-stream");
    zmsg_addstr (message, ".+@.+");
    rv = actor_commands (cfg, &data, &message);
    assert (rv == 0);
    assert (message == NULL);
    assert (streq (c_metric_conf_statefile (cfg), ""));
    assert (streq (c_metric_conf_cfgdir (cfg), ""));

    // PRODUCER
    message = zmsg_new ();
    assert (message);
    zmsg_addstr (message, "PRODUCER");
    zmsg_addstr (message, "some-stream");
    rv = actor_commands (cfg, &data, &message);
    assert (rv == 0);
    assert (message == NULL);
    assert (streq (c_metric_conf_statefile (cfg), ""));
    assert (streq (c_metric_conf_cfgdir (cfg), ""));

    // STATE_FILE
    message = zmsg_new ();
    assert (message);
    zmsg_addstr (message, "STATE_FILE");
    zmsg_addstr (message, test_state_file);
    rv = actor_commands (cfg, &data, &message);
    assert (rv == 0);
    assert (message == NULL);
    assert (streq (c_metric_conf_statefile (cfg), test_state_file));
    assert (streq (c_metric_conf_cfgdir (cfg), ""));

    // CFG_DIRECTORY, expected writable (current builddir of course is, right?)
    message = zmsg_new ();
    assert (message);
    zmsg_addstr (message, "CFG_DIRECTORY");
    zmsg_addstr (message, "./");
    rv = actor_commands (cfg, &data, &message);
    assert (rv == 0);
    assert (message == NULL);
    assert (streq (c_metric_conf_statefile (cfg), test_state_file));
    assert (streq (c_metric_conf_cfgdir (cfg), "./"));

    // CFG_DIRECTORY, expected writable
    // Note: the (originally non-absolute) config path is still stored
    // relative to process CWD at least currently, not sure this is a
    // good thing - but it is expected according to original tests.
    message = zmsg_new ();
    assert (message);
    zmsg_addstr (message, "CFG_DIRECTORY");
    zmsg_addstr (message, SELFTEST_DIR_RW);
    rv = actor_commands (cfg, &data, &message);
    assert (rv == 0);
    assert (message == NULL);
    assert (streq (c_metric_conf_statefile (cfg), test_state_file));
    assert (streq (c_metric_conf_cfgdir (cfg), SELFTEST_DIR_RW));

    zmsg_destroy (&message);
    c_metric_conf_destroy (&cfg);
    data_destroy (&data);
    zsys_file_delete (test_state_file);
    zstr_free (&test_state_file);
    zactor_destroy (&malamute);
    //  @end
    log_info ("actor-commands-test OK\n");
}
