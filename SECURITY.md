# Security Policy

NetcodeSim is a local/LAN educational simulator. It is not designed as an internet-facing matchmaking service or hardened production game server.

## Supported Versions

Before the first public release, only the default branch is supported.

## Reporting a Vulnerability

Please do not open a public issue for vulnerabilities that expose local files, crash hosts from malformed packets, or allow unexpected network behavior. Report them privately to the repository maintainer through GitHub's private vulnerability reporting if enabled, or by direct maintainer contact listed on the public repository.

Include:

- A concise description of the issue.
- Steps to reproduce it.
- The affected platform.
- Any proof-of-concept packet, save file, or command needed to validate the issue.

## Scope

Interesting reports include:

- Crashes or memory corruption from malformed network packets.
- Host/client behavior that accepts invalid protocol state.
- Local file writes outside intended project data paths.
- Reproducible denial-of-service behavior in the LAN runtime.

Out of scope:

- Internet matchmaking, account, anti-cheat, or payment concerns, because the project does not provide those systems.
