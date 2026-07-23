# Troubleshooting and recovery

## Restore the stock screen first

If the new UI is missing but SSH still works:

```sh
sudo q2-display-mode enable-qidi
```

If the helper is unavailable:

```sh
sudo systemctl disable KlipperScreen.service
sudo systemctl enable qidi-client.service
sudo systemctl stop KlipperScreen.service
sudo systemctl restart qidi-client.service
```

## Check status

```sh
sudo bash /home/qidi/install-klipperscreen-q2-on-printer.sh status
systemctl is-active KlipperScreen.service
systemctl is-active q2-display-gesture.service
```

## Black screen

```sh
sudo journalctl -u KlipperScreen.service -b --no-pager
tail -200 /home/qidi/printer_data/logs/KlipperScreen.log
```

Useful messages:

- `Cannot open X display` — Xvfb did not start in time.
- `Cannot open framebuffer` — the bridge cannot access `/dev/fb0`.
- `KlipperScreen splash hidden` — the bridge detected a painted GTK frame.
- `display component exited unexpectedly` — Xvfb, the bridge, or
  KlipperScreen exited.

## Touch misses its target

The active matrix lives here:

```sh
cat /etc/default/klipperscreen-q2
```

Measured points and the calculation are in `config/touch-calibration.txt`. Do
not apply an 800×480 scale on top of this matrix. Goodix advertises that range
in metadata, but the verified firmware reports actual coordinates already
close to 480×272.

## Gestures do not switch the UI

```sh
sudo systemctl status q2-display-gesture.service
sudo journalctl -u q2-display-gesture.service -b --no-pager
```

The gesture must begin near the edge and cross almost the entire display.
Travel mostly vertically; interpretive diagonal movement is ignored on purpose.

## KlipperScreen cannot see the printer

```sh
systemctl is-active moonraker.service klipper.service
curl -s http://127.0.0.1:7125/server/info
```

Connection settings:

```sh
cat /home/qidi/printer_data/config/KlipperScreen.conf
```

## Collect logs

Remove addresses and secrets before publishing:

```sh
sudo journalctl -u KlipperScreen.service -b --no-pager > /tmp/ks-service.log
sudo journalctl -u q2-display-gesture.service -b --no-pager > /tmp/ks-gesture.log
cp /home/qidi/printer_data/logs/KlipperScreen.log /tmp/
```

Private keys and Wi-Fi credentials do not help debug touch calibration, but
they do help strangers in exciting and unhelpful ways.
