# RoRNet 2.44 shutdown correction

## Problem

After SIGTERM, the server could report that all clients were disconnected while
the original process and listener thread remained alive. A reconnecting client
could then be accepted by the old process and receive the next UID instead of a
fresh UID from a restarted process.

## Correction

Shutdown now has explicit ownership and ordering. Signal handling requests
shutdown; the normal main thread closes admission, stops and joins the listener,
stops and joins client sender/receiver and killer workers, performs owner-thread
socket cleanup, and only then destroys authentication and AngelScript state.

Handshake header and payload reads retry short timeout/interrupted operations,
but share a bounded overall deadline. Complete headers and payloads are required,
oversized payloads remain rejected, and silent or stalled clients are closed.
The listener's expected polling timeout is no longer logged as an ERROR; other
socket and protocol failures remain visible. Two noisy AngelScript registration
messages use verbose logging without changing callback behavior.

## Validation

Validation covered idle INET/LAN shutdown, valid and reconnecting clients,
SIGTERM/SIGINT and repeated signals, silent and stalled handshakes, fragmented
headers and payloads, heartbeat activity, Main.as callback activity, active
script shutdown, and repeated restart cycles. Shutdown completed without forced
kills, stale listeners, stale sockets, bind failures, or post-shutdown UIDs.

Production fleet validation confirmed clean offline transitions, fresh process
and uptime after restart, fresh UID 1, working INET registration and Main.as,
and removal of recurring listener timeout ERRORs.

## Compatibility and deployment

The change retains RoRNet 2.44 packet IDs, structures, negotiation, registration,
authentication, heartbeat behavior, and Rigs of Rods 2022.12 compatibility.
Deploy the matching `rorserver` and `libmysocketw.so`, preserve live server
configuration and `resources/scripts/storage`, and set both files executable.
The intended Ubuntu Pterodactyl image resolves SocketW without an
`LD_LIBRARY_PATH` override.

For rollback, stop the server, restore the saved previous binary and startup
configuration, restore executable permissions, and restart.
