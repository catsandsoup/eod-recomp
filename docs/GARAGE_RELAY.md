# Garage Code service

A release publisher runs one public IPv4 UDP service for the game. Players host or enter their five-character Garage Code in Online. The game uses its outbound game socket to find the room, probes the direct path, and selects the relay when the probe receives no answer within 1.2 seconds of room registration. A direct probe that succeeds followed by missing admission or roster data switches to the relay after 2.5 seconds. A pending seat follows the same join attempt. Configured-service admission has a ten-second budget after the local content check. Each guest gets its own route. A single party can include direct and relayed guests. Room registration continues through events, results and rematches. The host's session decides admission and seat availability.

The path selection follows the connectivity-check and relay concepts described in ICE and TURN. This implementation speaks the game's own UDP protocol. It requires outbound UDP to the service. [ICE connectivity checks](https://www.rfc-editor.org/rfc/rfc8445.html#section-2.2) · [TURN relay allocations](https://www.rfc-editor.org/rfc/rfc8656.html#section-3.2)

When the service allocates a guest, it tells the host a player is joining, including in relay-only rooms. While any such join is pending, the host sees “Waiting for players to finish joining” and Start is disabled. The notice contains a short-lived attempt identifier. The host recognizes the same attempt when the game JOIN arrives, then waits for that guest's normal Ready state. Cancelling closes the hold, and an abandoned attempt expires after ten seconds. With an empty room the available action reads “Start Event Alone”.

## Run and package

On the service machine, run the following with Python 3.9 or newer. Allow inbound UDP on the selected port in the service firewall. Use a stable public address or a hostname with an IPv4 record.

```sh
python3 tools/garage_relay.py --port 27016
```

An Oracle Cloud Always Free VM.Standard.E2.1.Micro instance is a $0 hosting option within its free allowances. The shape includes a public IP, and the relay can run on its eligible Linux images. Capacity depends on the home region and Oracle can reclaim idle instances. Allow the relay's UDP port in the subnet security rules as well as in the guest firewall. [Oracle Always Free resources](https://docs.oracle.com/en-us/iaas/Content/FreeTier/freetier_topic-Always_Free_Resources.htm) · [Oracle public subnet and IPv4 setup](https://docs.oracle.com/en-us/iaas/Content/Compute/tutorials/first-linux-instance/overview.htm)

For a new Always Free Ubuntu 22.04 or 24.04 instance, generate the initialization file from the same checkout used to build the game.

```sh
python3 tools/garage_relay_cloud_init.py > garage-relay-cloud-init.yaml
```

Use the file as the instance's cloud-init user data at creation. It embeds the service, opens UDP 27016 in the Ubuntu guest firewall, and installs a systemd service that starts on boot and restarts after a process failure. In OCI, assign a public IPv4 address and allow stateful ingress from the internet to destination UDP 27016 on the instance's network security group or subnet. The free shape, image and boot storage must fit the tenancy's Always Free allowances in its home region. After initialization, `systemctl is-active eod-garage-relay` should return `active`. Verify admission from a game outside the VM's network before distributing its address in a release. [OCI instance creation](https://docs.oracle.com/en-us/iaas/Content/Compute/Tasks/launchinginstance.htm)

The game defaults to the project's Sydney relay at `152.67.103.52:27016`. Players host or join by Garage Code with no network settings to fill in. To package a different endpoint, set the CMake cache variable `EOD_RELAY_ENDPOINT`. Fresh configurations also read the environment variable of the same name, and the GitHub release workflow reads the corresponding repository Actions variable. For developer runs, `EOD_NETPLAY_RELAY=127.0.0.1:27016` overrides the packaged address and an empty runtime override selects tracker-based discovery. The public IP is assigned to this VM. Verify the address and update the default if the VM is replaced.

Use `--relay-only` on the service if peer addresses must remain private from the other drivers. By default the service exchanges host and guest addresses to probe a direct route. The service keeps rooms in memory, validates the origin of registration with a return cookie and binds a 128-bit bearer token to the sending endpoint. It only forwards a game's packets between the registered host and an admitted service member. The game then applies its own version, content, seat and event checks. The datagrams travel in cleartext. Healthy clients refresh their leases every two seconds. A graceful close releases the room immediately upon receipt, and abandoned allocations expire after 30 seconds.

## Verify

The service tests cover return cookies, room isolation, forwarding, leave, seat reuse, crew presence and timeout. Run them with `python3 -B tests/test_garage_relay.py`.

To exercise the time between service allocation and game admission on a native host, run `tools/test_online_pending_join.py EXE DISC EMPTY_OUTPUT_DIR MODE`. The modes `cancel` and `timeout` hold a guest before its game JOIN and check that the host can start alone after release. The `admit` case holds the guest unready after joining and verifies that Ready then permits Start. Add `--direct` to exercise admission on the advertised direct address.

The following commands launch a temporary loopback service and three real headless game processes. Supply the built executable, an extracted supported disc and a fresh output directory. The tests use normal Garage Codes, ready and load handshakes and two races with controlled native terminal records.

```sh
python3 tools/test_online_completion.py "$EOD_BINARY" "$DISC_DIR" "$LOG_DIR/mixed" race --garage-route mixed
python3 tools/test_online_completion.py "$EOD_BINARY" "$DISC_DIR" "$LOG_DIR/relay" race --garage-route unavailable-direct
```

The mixed run verifies that one guest uses a checked direct path and the second guest uses the relay. The unavailable-direct run withholds both direct candidates at the service. Both ordinary guests must automatically use the relay, agree on the standings and return to the lobby after each round.

To exercise the packaging path, configure a test build with `-DEOD_RELAY_ENDPOINT=127.0.0.1:27016`. Run `tools/test_online_completion.py` with the `native-wreck` scenario and `--garage-route unavailable-direct --packaged-relay-port 27016`. The runner clears the players' runtime endpoint overrides. The two games join by code, physically total a pre-damaged car, show matching standings and play a second derby. The `race-timeout` scenario exercises two rounds where a surviving racer is still driving at the deadline.

To race three independently connected players using the original game's Hard CPU drivers, run the public relay test with the packaged executable and a fresh output directory.

```sh
python3 tools/test_online_completion.py "$EOD_BINARY" "$DISC_DIR" "$LOG_DIR/public-pack" native-pack --public-garage relay --rotate-track --rendered-host
```

The host creates a Garage Code. Each guest enters the code once. The drivers race on Anoka and Ashland, and the runner checks native finish-line crossings, received rival movement, contact or passes, matching standings and the same seats in the lobby. For controlled internet impairment, add `--impaired-public-relay 152.67.103.52:27016`. The host keeps its compiled endpoint and each guest uses a loopback traffic conditioner to delay and drop packets travelling to the real service. A run without the conditioner checks the baked endpoint on all clients. Normal players use their controllers in both online and offline play.

For release acceptance on separate consumer networks, exercise direct and relay routes, voice, a full event, host loss, rejoin and rematch. Record the route and game outcomes from each client.
