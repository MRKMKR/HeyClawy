# OpenClaw Network Model

This is the intended deployment model for the OpenClaw host that serves HeyClawy devices.

## Goal

Keep the system simple:

- OpenClaw gateway is local-only on the host.
- Tailscale exposes the remote control path.
- HeyClawy and other ESP32 clients use separate LAN-facing services where needed.
- Do not mix OpenClaw gateway bind settings with ESP32 LAN service exposure.

## Current Intended Topology

### OpenClaw gateway

- Bind mode: `auto`
- Effective listener: `127.0.0.1:18789`
- Remote URL stored in config: `wss://openclaw.tail177081.ts.net`
- OpenClaw config path: `/home/mrkmkr_openclaw/.openclaw/openclaw.json`

OpenClaw should be treated as a loopback service. It should not bind directly to the LAN IP for normal operation.

### Tailscale exposure

- Public tailnet URL: `https://openclaw.tail177081.ts.net`
- Tailscale Serve target: `http://127.0.0.1:18789`

Tailscale is the remote exposure layer. If remote access breaks, check Tailscale Serve before changing OpenClaw gateway bind settings.

### LAN-facing services for devices

These can remain LAN-facing if required by ESP32 clients:

- STT adapter: port `5051`
- TTS adapter: port `5050` when used
- Any device-specific HTTP helpers or websocket bridges

These services are separate from the OpenClaw gateway. Keep that boundary intact.

## Safe Hardening Rules

- `~/.openclaw` may be private, but OpenClaw must still be able to rewrite its own config.
- `openclaw.json` should be writable by `mrkmkr_openclaw`.
- Avoid immutable flags on `openclaw.json`.
- Avoid root-owned overrides unless there is a strong reason and they are documented.
- Prefer packaged `systemd --user` units over manual `override.conf` network wiring.

Known-good baseline:

- `~/.openclaw` mode `700`
- `~/.openclaw/openclaw.json` mode `600`
- no immutable bit on `openclaw.json`

## Anti-Patterns To Avoid

- Setting `gateway.bind=custom` with the LAN IP just to help ESP32 clients.
- Pointing Tailscale Serve at `http://192.168.68.79:18789`.
- Keeping stale `OPENCLAW_GATEWAY_URL=ws://192.168.68.79:18789` in a systemd override.
- Using OpenClaw status output as proof that Tailscale proxying is configured, when Tailscale is being managed separately.
- Locking `openclaw.json` with `chattr +i`, which breaks config updates and some upgrade flows.

## Expected Steady State Checks

On the OpenClaw host:

```bash
ss -ltnp | grep 18789
```

Expected:

- `127.0.0.1:18789` is listening
- `192.168.68.79:18789` is not listening
- `100.x.y.z:18789` is not listening

Tailscale:

```bash
tailscale serve status
```

Expected:

- `https://openclaw.tail177081.ts.net`
- proxy target `http://127.0.0.1:18789`

## Recovery Notes

If the CLI starts failing with websocket `1006` or loopback refusal:

1. Check whether the gateway was rebound to the LAN IP.
2. Check whether a stale systemd override reintroduced `OPENCLAW_GATEWAY_URL`.
3. Check whether `openclaw.json` became immutable or unreadable/writable by the OpenClaw user.
4. Check whether Tailscale Serve still targets loopback.

Fix order:

1. Restore OpenClaw to loopback-only.
2. Restore Tailscale Serve to `127.0.0.1:18789`.
3. Leave ESP32 LAN services separate.

## Temporary App Testing

Safest pattern:

- run the test app on the OpenClaw host bound to `127.0.0.1`
- expose it on a separate Tailscale HTTPS port
- do not change the main OpenClaw mapping on port `443`
- tear it down when done

Recommended helpers:

- [openclaw_app_proxy.sh](/Users/mrkmkr/Documents/Projects/Desk_Ai/HeyClawy/tools/openclaw_app_proxy.sh) for proxy-only changes
- [openclaw_project_preview.sh](/Users/mrkmkr/Documents/Projects/Desk_Ai/tools/openclaw_project_preview.sh) for remote OpenClaw workspace apps

Example:

```bash
./tools/openclaw_project_preview.sh up christmas_list_app 3000 8443 -- npm run dev -- --host 127.0.0.1 --port 3000
./tools/openclaw_project_preview.sh status christmas_list_app 8443
./tools/openclaw_project_preview.sh down christmas_list_app 8443
```

For local phone testing on the same LAN, use a dedicated app port and bind the app
to `0.0.0.0` instead of changing the OpenClaw gateway:

```bash
./tools/openclaw_project_preview.sh up christmas_list_app 3000 8443 0.0.0.0 -- npm run dev -- --host 0.0.0.0 --port 3000
```

That gives:

- Tailscale preview: `https://openclaw.tail177081.ts.net:8443/`
- LAN preview: `http://192.168.68.79:3000/`

This keeps:

- `https://openclaw.tail177081.ts.net/` for OpenClaw itself
- `https://openclaw.tail177081.ts.net:8443/` for temporary app testing

## Intentional Quirk

`openclaw status` may still show Tailscale as off if OpenClaw is not directly managing Tailscale exposure and Tailscale Serve is configured independently. That is acceptable in this model.
