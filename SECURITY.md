# Security policy

## Supported version

Security fixes target the latest installer release and the verified QIDI Q2 /
firmware `01.01.02.03` combination.

## Reporting a vulnerability

If this repository is hosted on GitHub, use **Private vulnerability reporting**
in the Security tab. Do not place any of the following in a public issue:

- SSH passwords;
- private keys;
- Wi-Fi credentials;
- full Moonraker configs containing tokens;
- internet-accessible printer addresses.

If private reporting is not enabled yet, open an issue without exploitation
details and request a secure contact channel.

The project does not open network ports or modify SSH configuration. The
installer does run as root, manage systemd, and write to the framebuffer, so
checking its checksum is not a decorative ritual.
