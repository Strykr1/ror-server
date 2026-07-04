# RoR Server 2.45 Shutdown Investigation

## Goal

Diagnose and fix a Linux shutdown hang in the Rigs of Rods server.

Observed behavior:

1. The server receives SIGINT.
2. It prints:
   - "closing server ... unregistering ..."
   - "closing. disconnecting clients ..."
   - "all clients disconnected. exiting."
3. The process remains alive.
4. The listener can still accept a reconnecting RoR bot.
5. Pterodactyl must force-kill the process.

## Safety rules

- Work only in this Git repository and ~/ror-test-245.
- Do not access SSH keys, Discord tokens, Pterodactyl credentials, or unrelated files.
- Do not edit production server files.
- Do not upload anything to Pterodactyl.
- Create Git commits at major checkpoints.
- Prefer small and auditable changes.
- Show diffs before broad architectural changes.
- Preserve RoRNet 2.45 behavior and AngelScript support.
- Keep a rollback path.

## Investigation priorities

Inspect:

- source/server/rorserver.cpp
- Listener startup and Listener::Shutdown()
- Sequencer::Close()
- ScriptEngine destruction
- Authentication resolver destruction
- Killer thread shutdown and joins
- Signal handling
- Client receiver and broadcaster thread shutdown

## Expected architectural fix

The signal handler should only request shutdown.

The normal main thread should then:

1. Stop the listener.
2. Ensure the listening socket is closed.
3. Unregister from the master server.
4. Disconnect clients.
5. Destroy the script engine and authentication resolver.
6. Stop and join worker threads.
7. Return normally from main.

Do not assume this is the only bug. Add shutdown-stage instrumentation and reproduce the hang before finalizing the fix.

## Build requirements

- Linux x86-64 binary
- CMake and Ninja
- RelWithDebInfo for diagnostic builds
- AngelScript enabled
- Preserve Main.as support
- Record the exact Git commit and build options

## Required testing

- Baseline unmodified build
- SIGINT with no clients
- SIGINT with the RoR bot connected
- At least 10 repeated shutdown tests
- Verify the listening port closes immediately
- Verify the old process cannot accept a reconnect
- Verify Main.as still loads
- Verify RoRNet 2.45 still works
- Capture GDB thread backtraces if a hang remains
