/*  =========================================================================
    proto_metric_unavailable - metric unavailable protocol send part

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
    proto_metric_unavailable - metric unavailable protocol send part
@discuss
@end
*/

#include "fty_metric_composite_classes.h"

//  --------------------------------------------------------------------------
//  Send metric unavailable protocol message

void
proto_metric_unavailable_send (mlm_client_t *client, const char *topic)
{
    assert (client);
    assert (topic);

    zmsg_t *message = zmsg_new ();
    assert (message);

    zmsg_addstr (message, "METRICUNAVAILABLE");
    zmsg_addstr (message, topic);

    int rv = mlm_client_send (client, "metric_topic", &message);
    if (rv != 0) {
        zmsg_destroy (&message);
        log_error ("mlm_client_send (subject = '%s') failed.", "metric_topic");
    }
}

//  --------------------------------------------------------------------------
//  Self test of this class

void
proto_metric_unavailable_test (bool verbose)
{
    ManageFtyLog::setInstanceFtylog ("proto-metric-unavailable-test", "");
    if ( verbose )
        ManageFtyLog::getInstanceFtylog()->setVeboseMode();
    printf (" * proto_metric_unavailable: ");

    //  @selftest
    static const char* endpoint = "inproc://proto-metric-unavailable-server-test";

    printf (" * bios_composite_metrics_configurator_server: ");
    if (verbose)
        printf ("\n");

    zactor_t *server = zactor_new (mlm_server, (void*) "Malamute");
    zstr_sendx (server, "BIND", endpoint, NULL);
    zclock_sleep (100);

    mlm_client_t *producer = mlm_client_new ();
    mlm_client_connect (producer, endpoint, 1000, "producer");
    mlm_client_set_producer (producer, "XYZ");

    mlm_client_t *consumer = mlm_client_new ();
    mlm_client_connect (consumer, endpoint, 1000, "consumer");
    mlm_client_set_consumer (consumer, "XYZ", ".*");

    zclock_sleep (100);

    proto_metric_unavailable_send (producer, "average.temperature@DC-Roztoky");
    zclock_sleep (100);
    zmsg_t *message = mlm_client_recv (consumer);
    assert (message);
    char *piece = zmsg_popstr (message);
    assert (piece);
    assert (streq (piece, "METRICUNAVAILABLE"));
    zstr_free (&piece);
    piece = zmsg_popstr (message);
    assert (streq (piece, "average.temperature@DC-Roztoky"));
    zstr_free (&piece);
    zmsg_destroy (&message);

    mlm_client_destroy (&producer);
    mlm_client_destroy (&consumer);
    zactor_destroy (&server);
    //  @end
    printf ("OK\n");
}
