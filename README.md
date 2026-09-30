# ReHLDS NetDiag

Native Metamod/ReHLDS diagnostic plugin for recurring per-client `SZ_GetSpace` /
`datagram overflowed` errors.

## Why this exists

The AMXX diagnostic plugin can see user messages and many TempEntity messages, but
it cannot see the whole engine networking state. This module uses the public ReHLDS
API and logs engine-side state when ReHLDS prints a real overflow.

It hooks:

- `SV_CreatePacketEntities` to record entity snapshot count and bytes added;
- `Con_Printf` to detect the actual ReHLDS overflow console line.

ReHLDS exposes both hooks in its public API.

## Build in GitHub

The repository contains `.github/workflows/build.yml`.

1. Create a GitHub repository.
2. Upload all files from this package, preserving `.github/workflows/build.yml`.
3. Open **Actions**.
4. Select **Build ReHLDS NetDiag**.
5. Click **Run workflow**.
6. When the job succeeds, download artifact `rehlds-netdiag-linux-i386`.

The artifact contains the compiled `rehlds_netdiag_mm_i386.so`.

## Server installation

See `INSTALL.txt`.

## What the log means

At an overflow the module logs, for every connected human client:

- unreliable datagram current/max size and flags;
- reliable netchan message current/max size and flags;
- choke count;
- delta sequence;
- packet loss and latency;
- last packet-entity count;
- bytes added by the packet-entity stage;
- send/skip state and next-message timings.

This lets us distinguish:

1. datagram already near max -> unreliable/broadcast accumulation;
2. reliable message near max -> reliable backlog;
3. huge `packet_msg_delta` / entity count -> snapshot/entity traffic;
4. all normal -> investigate lower-level `SZ_GetSpace` call instrumentation.

## Safety

This module is diagnostic-only. It does not modify rates, kick clients, alter
entities, or suppress network messages.
