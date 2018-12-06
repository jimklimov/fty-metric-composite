/*  =========================================================================
    fty_metric_composite_configurator - Metrics calculator configurator

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
    fty_metric_composite_configurator - Metrics calculator configurator
@discuss
@end
*/

#include <getopt.h>

#include "fty_metric_composite_classes.h"

#define str(x) #x

static const char *AGENT_NAME = "fty-metric-composite-configurator";
static const char *ENDPOINT = "ipc://@/malamute";
static const char *DIRECTORY = "/var/lib/fty/fty-metric-composite";

void usage () {
    puts ("fty-metric-composite-configurator [options] ...\n"
          "  --verbose / -v         verbose logging mode\n"
          "  --output-dir / -s      directory, where configuration files would be created (directory MUST exist)\n"
          "  --help / -h            this information\n"
          );
}

static int
s_timer_event (zloop_t *loop, int timer_id, void *output)
{
    char *env = getenv ("BIOS_DO_SENSOR_PROPAGATION");
    if (env == NULL)
        zstr_sendx (output, "IS_PROPAGATION_NEEDED", "true", NULL);
    else
        zstr_sendx (output, "IS_PROPAGATION_NEEDED", env, NULL);
    return 0;
}

int main (int argc, char *argv [])
{
    int help = 0;
    bool verbose = false;
    char *output_dir = NULL;

// Some systems define struct option with non-"const" "char *"
#if defined(__GNUC__) || defined(__GNUG__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wwrite-strings"
#endif
    static const char *short_options = "hvs:";
    static struct option long_options[] =
    {
            {"help",            no_argument,        0,  1},
            {"verbose",         no_argument,        0,  'v'},
            {"output-dir",      required_argument,  0,  'o'},
            {0,                 0,                  0,  0}
    };
#if defined(__GNUC__) || defined(__GNUG__)
#pragma GCC diagnostic pop
#endif

    while (true) {

        int option_index = 0;
        int c = getopt_long (argc, argv, short_options, long_options, &option_index);
        if (c == -1)
            break;
        switch (c) {
            case 'v':
            {
                verbose = true;
                break;
            }
            case 'o':
            {
                output_dir = optarg;
                break;
            }
            case 'h':
            default:
            {
                help = 1;
                break;
            }
        }
    }
    if (help) {
        usage ();
        return EXIT_FAILURE;
    }

    ManageFtyLog::setInstanceFtylog ("fty-metric-composite-configurator", LOG_CONFIG);
    if (verbose)
        ManageFtyLog::getInstanceFtylog()->setVeboseMode();


    if (!output_dir) {
        output_dir = strdup (DIRECTORY);
    }
    log_debug ("output_dir == '%s'", output_dir ? output_dir : "(null)");

    zactor_t *server = zactor_new (fty_metric_composite_configurator_server, (void *) AGENT_NAME);
    if (!server) {
        log_fatal ("zactor_new (task = 'fty_metric_composite_configurator_server', args = 'NULL') failed");
        zstr_free (&output_dir);
        return EXIT_FAILURE;
    }
    zstr_sendx (server,  "CFG_DIRECTORY", output_dir, NULL);
    zstr_sendx (server,  "LOAD", NULL);
    zstr_sendx (server,  "CONNECT", ENDPOINT, NULL);
    zstr_sendx (server,  "PRODUCER", "_METRICS_UNAVAILABLE", NULL);
    zstr_sendx (server,  "CONSUMER", FTY_PROTO_STREAM_ASSETS, ".*", NULL);

    zloop_t *check_configuration_trigger = zloop_new();
    // one in a minute
    zloop_timer (check_configuration_trigger, 60*1000, 0, s_timer_event, server);
    zloop_start (check_configuration_trigger);

    zloop_destroy (&check_configuration_trigger);

    zstr_free (&output_dir);
    zactor_destroy (&server);
    return EXIT_SUCCESS;
}
