# Incident Response

## Severity levels

| Severity | Description | Response time |
|----------|-------------|---------------|
| P0 | Data corruption, silent data loss, memory safety | < 24 h |
| P1 | Incorrect behavior, conformance regression | < 72 h |
| P2 | Performance regression, non-critical bug | < 2 weeks |
| P3 | Documentation, cosmetic | Next release |

## Response process

1. **Triage** — classify severity; assign owner.
2. **Isolate** — identify affected versions and transports.
3. **Fix** — implement regression test first, then fix.
4. **Release** — patch release for P0/P1; next minor for P2/P3.
5. **Post-mortem** — update HARA / TARA if safety-relevant.

## Contacts

- Primary: matt@jellybaby.com
- GitHub Issues: https://github.com/SoundMatt/cpp-DDS/issues
