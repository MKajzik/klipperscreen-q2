# How to help without arguing with the printer

Thanks for taking an interest—especially if you own another QIDI Q2. A sample
size of two printers is practically a laboratory.

## Before changing anything

1. Do not test the display stack during a print.
2. Back up `/home/qidi/printer_data/config`.
3. Record the exact `qd-q2-system` version.
4. Make sure you can restore the stock UI over SSH:

   ```sh
   sudo q2-display-mode enable-qidi
   ```

## Local checks

```sh
make check
```

This verifies:

- shell syntax;
- the self-contained installer checksum;
- embedded sources and splash against repository files;
- embedded ARM64 binary hashes;
- C sources when Linux headers are available.

## Changing embedded components

The installer is self-contained: the bridge, gesture daemon, and splash are
stored inside it as gzip/base64 payloads. If a source file or asset changes:

1. build the ARM64 binary on a Q2 or compatible environment;
2. update the matching payload and SHA-256;
3. bump the installer version;
4. update `install-klipperscreen-q2-on-printer.sh.sha256`;
5. run `make check`;
6. verify the physical framebuffer, not merely a handsome PNG on a laptop.

That final item is not theoretical.

## A useful bug report

Please include:

- printer model and firmware version;
- output from `sudo .../install-klipperscreen-q2-on-printer.sh status`;
- the relevant `journalctl` output;
- what was visible before and after the action;
- a photograph of the panel for visual issues.

Remove IP addresses, passwords, Wi-Fi credentials, and anything that would make
you rotate keys on a Sunday evening.

## Pull requests

Keep one logical change per PR. Explain not only what changed, but which
hardware physically tested it. For code that owns a printer display,
“it compiles on my laptop” is useful information, but not quite a victory lap.
