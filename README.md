# fty-metric-composite

Agent fty-metric-composite receives sensor metrics and feeds them into LUA functions, which compute standard metrics.

It has a helper component fty-metric-composite-configurator which configures it based on received asset messages.

## How to build

To build fty-metric-composite project run:

```bash
./autogen.sh
./configure
make
make check # to run self-test
```

## How to run

To run fty-metric-composite project:

* from within the source tree, run:

```bash
./src/fty-metric-composite-configurator
./src/fty-metric-composite /var/lib/fty/fty-metric-composite/bios.cfg
```

For the other options available, refer to the manual pages of fty-metric-composite and fty-metric-composite-configurator.

* from an installed base, using systemd, run:

```bash
systemctl start fty-metric-composite-configurator
systemctl start fty-metric-composite@bios
```

## Component fty-metric-composite

### Configuration file

Standard configuration file for fty-metric-composite - fty-metric-composite.cfg - is currently ignored.

Component fty-metric-composite receives a name of non-standard configuration file as its first command-line argument.

Based on this file (by default /var/lib/fty/fty-metric-composite/bios.cfg),  
it sets an instance (by default bios) and types of metrics for which to listen on \_METRICS\_SENSOR stream.

Agent reads environment variable BIOS\_LOG\_LEVEL to set verbosity level.

## Architecture

### Overview

fty-metric-composite has 1 actor:

* fty-metric-composite-server: main actor

## Protocols

### Published metrics

Agent publishes metrics on METRICS stream.

### Published alerts

Agent doesn't publish any alerts.

### Mailbox requests

Agent doesn't receive any mailbox requests.

### Stream subscriptions

Agent is subscribed to \_METRICS\_SENSOR stream.

When it receives a metric, it updates the local cache and evaluates stored LUA function.  
Computed value is then published as a new metric on METRICS stream.

## Component fty-metric-composite-configurator

### Configuration file

Standard configuration file for fty-metric-composite-configurator - fty-metric-composite-configurator.cfg - is currently ignored.

Agent has a state file, stored by default in /var/lib/fty/fty-metric-composite/configurator\_state\_file.

In /var/lib/fty/fty-metric-composite/ are stored .cfg files for various assets.

Agent reads environment variable BIOS\_LOG\_LEVEL to set verbosity level.

## Architecture

### Overview

fty-metric-composite-configurator has 1 actor and 1 zloop timer:

* fty-metric-composite-configurator-server: main actor
* check-configuration-trigger: runs ever minute;

It reads environment variable BIOS\_DO\_SENSOR\_PROPAGATION and sends message  
IS\_PROPAGATION\_NEEDED/\{true,false\} based on its value.

It also has one built-in timer, which re-generates LUA functions for known assets  
and sends METRICS\_UNAVAILABLE for devices for which no data are available.

## Protocols

### Published metrics

Agent doesn't publish any metrics.

### Published alerts

Agent doesn't publish any alerts.

### Mailbox requests

Agent doesn't receive any mailbox messages.

### Stream subscriptions

Agent is subscribed to ASSETSS stream.

Received assets are stored into local cache.

