# Security Policy

> Copyright (c) 2026 Angad Singh Bains. All rights reserved.
> This project is distributed under the **Angad Singh Personal & Non-Commercial
> Source Available License**. See [LICENSE](LICENSE).

The security of **MAVLink Companion Service** is taken seriously. This
document explains which versions receive security fixes and how to report a
vulnerability responsibly.

## Supported Versions

| Version | Supported |
| ------- | --------- |
| 1.0.x   | Yes       |

Security fixes are applied to the latest release line. Older versions are not
maintained.

## Reporting a Vulnerability

**Do not report security vulnerabilities through public GitHub issues,
discussions, or pull requests.** Public disclosure before a fix is available can
put users at risk.

Instead, report the issue privately by email to:

    singh4anga@gmail.com

You may also use [GitHub private vulnerability reporting](https://github.com/Angad7600123/Mavlink-companion-log-service/security/advisories)
for this repository.

Please include as much of the following as you can:

- A description of the vulnerability and its potential impact.
- The affected version or commit.
- Step-by-step instructions to reproduce the issue.
- Any proof-of-concept code, logs, or configuration required to demonstrate it.
- Any suggested mitigation or fix, if you have one.

## Responsible Disclosure

We ask that you:

- Give us a reasonable opportunity to investigate and address the issue before
  any public disclosure.
- Avoid privacy violations, data destruction, and service disruption while
  researching.
- Do not access or modify data that does not belong to you.

In return, we will:

- Acknowledge your report promptly.
- Provide an initial assessment and a remediation plan within a reasonable
  timeframe.
- Keep you informed of progress toward a fix.
- Credit you in the release notes once the issue is resolved, unless you prefer
  to remain anonymous.

## Scope

In scope:

- The `mcls` source code and binary in this repository.
- The provided systemd unit and installation scripts.
- Configuration handling and file operations within the state directory.
- The MAVLink log protocol implementation and the SQLite catalog handling.

Out of scope:

- `mavlink-router`, which is a separate project and should be reported to its
  maintainers.
- Flight controller firmware.
- Issues that require pre-existing privileged or physical access to the host.

## Hardening Notes

- Run the service as the dedicated unprivileged user provided by the systemd
  unit, with write access limited to the state directory.
- Keep the configuration file readable only by privileged users.
- Bind the MAVLink endpoint to localhost when the service runs on the same
  device, and do not expose it to untrusted networks without protection.

For the broader security model and architecture context, see the Security Model
section of [README.md](README.md).
