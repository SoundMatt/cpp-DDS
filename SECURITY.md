# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| 0.1.x   | Yes       |

## Reporting a vulnerability

Report security vulnerabilities privately to **matt@jellybaby.com**.

Please include:
- A description of the vulnerability and its impact.
- Steps to reproduce.
- Affected versions.

Do not open a public issue for security vulnerabilities.

We aim to acknowledge reports within 48 hours and provide a fix or mitigation plan within 14 days.

## Security design notes

cpp-DDS v0.1.0 ships no cryptographic primitives. The `dds::mock` transport is in-process only and carries no network exposure. Authentication, encryption, and anti-replay are planned for v0.3.0.
