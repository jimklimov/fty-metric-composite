/*  =========================================================================
    fty_metric_composite_server - Composite metrics server

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
    fty_metric_composite_server - Composite metrics server
@discuss
@end
*/

#include "fty_metric_composite_classes.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <string.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <map>
#include <iostream>
#include <fstream>
#include <cxxtools/jsondeserializer.h>
#include <cxxtools/directory.h>
#include <fty_proto.h>

struct value {
  double value;
  time_t valid_till;
};

static std::string
escape_regex (const std::string &notregex)
{
    static const char *to_be_escaped = ".^$|()[]{}*+?\\";
    std::string result;

    for (char c: notregex) {
        if (strchr (to_be_escaped,c)) {
            result += '\\';
        }
        result += c;
    }
    return result;
}

void
fty_metric_composite_server (zsock_t *pipe, void* args) {
    static const uint64_t TTL = 5*60;
    std::map<std::string, value> cache;
    std::string lua_code;
    int phase = 0;

    char *name = strdup ((char*) args);

    mlm_client_t *client = mlm_client_new ();

    zpoller_t *poller = zpoller_new (pipe, mlm_client_msgpipe (client), NULL);

    zsock_signal (pipe, 0);

    while (!zsys_interrupted) {

        void *which = zpoller_wait (poller, -1);
        if (which == pipe) {
            zmsg_t *msg = zmsg_recv (pipe);
            char *cmd = zmsg_popstr (msg);
            log_trace ("actor command=%s", cmd);

            if (streq (cmd, "$TERM")) {
                log_info ("Got $TERM");
                zstr_free (&cmd);
                zmsg_destroy (&msg);
                goto exit;
            }
            else
            if (streq (cmd, "CONNECT")) {
                char* endpoint = zmsg_popstr (msg);
                mlm_client_connect(client, endpoint, 1000, name);
                int rv = mlm_client_set_producer(client, "METRICS");
                if (rv == -1) {
                    log_error ("mlm_client_set_producer () failed.");
                }
                zstr_free(&endpoint);
                phase = 1;
            }
            else
            if (streq (cmd, "CONFIG")) {
                if(phase < 1) {
                    log_error("CONFIG before CONNECT");
                    continue;
                }
                char* filename = zmsg_popstr (msg);
                log_trace ("%s:\tOpening '%s'", name, filename);
                std::ifstream f(filename);
                if ( !f.good() ) {
                    log_error ("%s:\tCannot open config file '%s' correctly", name, filename);
                    zstr_free (&filename);
                    zstr_free (&cmd);
                    zmsg_destroy (&msg);
                    break; // if we cannot open config file -> just exit!
                }
                try {
                    cxxtools::JsonDeserializer json(f);
                    json.deserialize();
                    const cxxtools::SerializationInfo *si = json.si();
                    si->getMember("evaluation") >>= lua_code;

                    // Subscribe to all streams, create expired values in cache
                    value expired;
                    expired.valid_till = 0;
                    for(auto it : si->getMember("in")) {
                        std::string buff;
                        it >>= buff;
                        cache[buff] = expired;
                        buff = "^" + escape_regex (buff) + "$";
                        mlm_client_set_consumer(client, "_METRICS_SENSOR", buff.c_str());
                        log_trace ("%s: Registered to receive '%s' from stream '%s'", name, buff.c_str(), "_METRICS_SENSOR");
                    }
                    zstr_free (&filename);
                    phase = 2;
                }
                catch ( const std::exception &e ) {
                    log_error ("Cannot deserialize cfg file! with '%s'", e.what());
                    zstr_free (&filename);
                    zstr_free (&cmd);
                    zmsg_destroy (&msg);
                    break; // if we cannot load config file -> just exit!
                }
            }
            zstr_free (&cmd);
            zmsg_destroy (&msg);
            continue;
        }

        if(phase < 2) {
            log_error("DATA before CONFIG");
            continue;
        }

        // Get message
        zmsg_t *msg = mlm_client_recv(client);
        if(msg == NULL)
            continue;
        fty_proto_t *yn = fty_proto_decode(&msg);
        if(yn == NULL)
            continue;

        // Update cache with updated values
        std::string topic = mlm_client_subject(client);
        value val;
        val.value = atof(fty_proto_value(yn));
        uint32_t ttl = fty_proto_ttl(yn);
        uint64_t timestamp = fty_proto_time (yn);
        val.valid_till = timestamp + ttl;
        log_trace ("%s: Got message '%s' with value %lf", name, topic.c_str(), val.value);
        auto f = cache.find(topic);
        if(f != cache.end()) {
            f->second = val;
        } else {
            cache.insert(std::make_pair(topic, val));
        }
        fty_proto_destroy(&yn);

        // Prepare data for computation
#if LUA_VERSION_NUM > 501
        lua_State *L = luaL_newstate();
#else
        lua_State *L = lua_open();
#endif
        luaL_openlibs(L);
        lua_newtable(L);
        time_t tme = time(NULL);
        int error;
        for(auto i : cache) {
            if(tme > i.second.valid_till) {
                // can't count average, missing measurements from sensor
                continue;
            }
            log_trace ("%s - %s, %f", name, i.first.c_str(), i.second.value);
            lua_pushstring(L, i.first.c_str());
            lua_pushnumber(L, i.second.value);
            lua_settable(L, -3);
        }
        lua_setglobal(L, "mt");

        // Do the real processing
        error = luaL_loadbuffer(L, lua_code.c_str(), lua_code.length(), "line") ||
            lua_pcall(L, 0, 3, 0);
        if(error) {
            log_error("%s", lua_tostring(L, -1));
            goto next_iter;
        }
        log_trace ("%s: Total: %d", name, lua_gettop(L));
        if(lua_gettop(L) == 3) {
            if(strrchr(lua_tostring(L, -3), '@') == NULL) {
                log_error ("Invalid output topic");
                goto next_iter;
            }
            fty_proto_t *n_met = fty_proto_new(FTY_PROTO_METRIC);
            log_debug ("Creating new bios proto message");
            char *buff = strdup(lua_tostring(L, -3));
            fty_proto_set_name (n_met, "%s", strrchr(buff, '@') + 1);
            (*strrchr(buff, '@')) = 0;
            fty_proto_set_type(n_met, "%s", buff);
            fty_proto_set_value(n_met, "%.2f", lua_tonumber(L, -2));
            fty_proto_set_unit(n_met,  "%s", lua_tostring(L, -1));
            fty_proto_set_ttl(n_met,  TTL);
            fty_proto_set_time(n_met, std::time (NULL));
            fty::shm::write_metric(n_met);
            zmsg_t* z_met = fty_proto_encode(&n_met);
            int rv = mlm_client_send(client, lua_tostring(L, -3), &z_met);
            if (rv != 0) {
                log_error ("mlm_client_send () failed.");
            }
            free (buff);
        } else {
            log_error ("Not enough valid data...\n");
        }
next_iter:
        lua_close(L);
    }

exit:
    free (name);
    zpoller_destroy (&poller);
    mlm_client_destroy (&client);
}

//  ---------------------------------------------------------------------------
//  Selftest

void
fty_metric_composite_server_test (bool verbose)
{
    ManageFtyLog::setInstanceFtylog ("fty-metric-composite-server-test", "");
    if ( verbose )
        ManageFtyLog::getInstanceFtylog()->setVeboseMode();
    static const char* endpoint = "inproc://bios-cm-server-test";

    // Note: If your selftest reads SCMed fixture data, please keep it in
    // src/selftest-ro; if your test creates filesystem objects, please
    // do so under src/selftest-rw. They are defined below along with a
    // usecase (asert) to make compilers happy.
    const char *SELFTEST_DIR_RO = "src/selftest-ro";
    const char *SELFTEST_DIR_RW = "src/selftest-rw";
    assert (SELFTEST_DIR_RO);
    assert (SELFTEST_DIR_RW);
    fty_shm_set_test_dir(SELFTEST_DIR_RW);
    // std::string str_SELFTEST_DIR_RO = std::string(SELFTEST_DIR_RO);
    // std::string str_SELFTEST_DIR_RW = std::string(SELFTEST_DIR_RW);

    //  @selftest
    zactor_t *server = zactor_new (mlm_server, (void*) "Malamute");
    zstr_sendx (server, "BIND", endpoint, NULL);

    mlm_client_t *producer = mlm_client_new ();
    mlm_client_connect (producer, endpoint, 1000, "producer");
    mlm_client_set_producer (producer, "_METRICS_SENSOR");

    mlm_client_t *consumer = mlm_client_new ();
    mlm_client_connect (consumer, endpoint, 1000, "consumer");
    mlm_client_set_consumer (consumer, FTY_PROTO_STREAM_METRICS, "temperature@world");

    char *name = NULL;
    if(asprintf(&name, "composite-metrics-%s", "sd") < 0) {
        log_error ("Can't allocate name of agent");
        exit(1);
    }

    zactor_t *cm_server = zactor_new (fty_metric_composite_server, (void*) name);
    free(name);
    zstr_sendx (cm_server, "CONNECT", endpoint, NULL);
    char *test_config_file = zsys_sprintf ("%s/fty-metric-composite.cfg.example", SELFTEST_DIR_RO);
    assert (test_config_file != NULL);
    zstr_sendx (cm_server, "CONFIG", test_config_file, NULL);
    zclock_sleep (500);   //THIS IS A HACK TO SETTLE DOWN THINGS
    zstr_free (&test_config_file);

    // send one value
    zmsg_t *msg_in;
    msg_in = fty_proto_encode_metric(
            NULL, ::time (NULL), 60, "temperature", "TH1", "40", "C");
    assert (msg_in);
    mlm_client_send (producer, "temperature@TH1", &msg_in);

    zmsg_t *msg_out;
    fty_proto_t *m;
    msg_out = mlm_client_recv (consumer);
    m = fty_proto_decode (&msg_out);
    fty_proto_print (m);
    assert ( streq (mlm_client_sender (consumer), "composite-metrics-sd") );
    assert (m);
    assert (streq (fty_proto_value (m), "40.00"));    // <<< 40 / 1
    fty_proto_destroy (&m);
    {
      fty::shm::shmMetrics resultT;
      fty::shm::read_metrics(FTY_SHM_METRIC_TYPE, "world", "temperature", resultT);
      m = resultT.get(0);
      fty_proto_print (m);
      assert (m);
      assert (streq (fty_proto_value (m), "40.00"));    // <<< 40 / 1
      fty_shm_delete_test_dir();
      fty_shm_set_test_dir(SELFTEST_DIR_RW);
      m = NULL;
    }

    // send another value
    msg_in = fty_proto_encode_metric(
            NULL, ::time (NULL), 60, "temperature", "TH2", "100", "C");
    assert (msg_in);
    mlm_client_send (producer, "temperature@TH2", &msg_in);

    msg_out = mlm_client_recv (consumer);
    m = fty_proto_decode (&msg_out);
    assert (m);
    log_error("value %s", fty_proto_value (m));    // <<< (100 + 40) / 2
    assert (streq (fty_proto_value (m), "70.00"));    // <<< (100 + 40) / 2
    fty_proto_destroy (&m);
    {
      fty::shm::shmMetrics resultT;
      fty::shm::read_metrics(FTY_SHM_METRIC_TYPE, "world", "temperature", resultT);
      m = resultT.get(0);
      assert (m);
      log_error("value %s", fty_proto_value (m));    // <<< (100 + 40) / 2
      assert (streq (fty_proto_value (m), "70.00"));    // <<< (100 + 40) / 2
      fty_shm_delete_test_dir();
      fty_shm_set_test_dir(SELFTEST_DIR_RW);
      m = NULL;
    }

    // send value for TH1 again
    msg_in = fty_proto_encode_metric(
            NULL, ::time (NULL), 60, "temperature", "TH1", "70.00", "C");
    assert (msg_in);
    mlm_client_send (producer, "temperature@TH1", &msg_in);

    msg_out = mlm_client_recv (consumer);
    m = fty_proto_decode (&msg_out);
    assert (m);
    assert (streq (fty_proto_value (m), "85.00"));     // <<< (100 + 70) / 2
    fty_proto_destroy (&m);    
    {
      fty::shm::shmMetrics resultT;
      fty::shm::read_metrics(FTY_SHM_METRIC_TYPE, "world", "temperature", resultT);
      m = resultT.get(0);
      assert (m);
      assert (streq (fty_proto_value (m), "85.00"));     // <<< (100 + 70) / 2
      fty_shm_delete_test_dir();
      m = NULL;
    }

    zactor_destroy (&cm_server);
    mlm_client_destroy (&consumer);
    mlm_client_destroy (&producer);
    zactor_destroy (&server);

    // @end
    log_info ("OK\n");
}
