# Dell C1660w Universal Mac Driver

A native, Universal (Apple silicon and Intel) macOS driver for the Dell C1660w color LED
printer, with no Rosetta needed. It runs a small print server in the background, so your Mac
sees the C1660w as a standard driverless (AirPrint / IPP Everywhere) printer.

## Why I made this

I love my Dell C1660w. The model came out in 2013, and I've been using mine for over 13
years.

The original macOS driver hasn't kept up. It's Intel-only, so on a modern Mac it only runs
under Rosetta, which Apple is phasing out. It seemed a shame for a perfectly good printer to
stop working because of its driver, so I took matters into my own hands. I wrote a native
replacement, first for myself, and now for anyone else who still has one of these wonderful
printers and wants to keep using it on the latest macOS.

## What it is

This program is an [IPP Everywhere](https://www.pwg.org/ipp/everywhere.html) Printer
Application built on [PAPPL](https://www.msweet.org/pappl). It runs natively and offers the
printer to macOS, Linux and other IPP clients as a standard driverless printer. Clients send
it page rasters. It converts them to printer color, compresses them to HBPL v1 and sends the
job to the printer over the network.

## Supported macOS versions

It runs natively on Apple silicon and Intel Macs. You don't need Rosetta.

| macOS | version |
|---|---|
| macOS Golden Gate | 27 |
| macOS Tahoe | 26 |
| macOS Sequoia | 15 |
| macOS Sonoma | 14 |
| macOS Ventura | 13 |
| macOS Monterey | 12 |
| macOS Big Sur | 11 |

So far it has been tested on macOS Golden Gate 27.0.1 on Apple silicon. The installer is
built for all of the versions above, but the older ones haven't been tried yet.

## Status

Early, but in use. Tested on macOS Golden Gate 27.0.1 with a networked C1660w:

- confirmed on paper: color printing and manual long-edge duplex;
- confirmed offline, in byte-for-byte comparisons with the original driver's output:
  color, monochrome and duplex jobs;
- not yet tested: USB connections, and other printers that use HBPL v1.

## Features

- Paper sizes: Letter, Legal, A4, A5, B5 (JIS), Executive, Folio, envelopes (#10, Monarch,
  C5, DL) and custom sizes from 76.2 x 127 mm to 215.9 x 355.6 mm.
- Media types: plain, heavyweight, cardstock, coated, labels, envelope.
- Color or monochrome. Print quality "draft" turns on the printer's toner saver.
- Manual two-sided printing, long edge or short edge. The printer prints the back sides
  first, pauses, and prints the front sides once you put the stack back in the tray. For
  an odd page count it adds a blank sheet so the pages pair up.
- Jobs already in HBPL format (`application/vnd.hbpl`) are passed through unchanged.

## Installing on macOS

Download `c1660w-universal-mac-driver-VERSION.pkg` from the
[latest release](../../releases/latest) and open it. The installer is signed with a Developer
ID and notarized by Apple. It works on macOS Big Sur 11 or later, up to and including macOS
Golden Gate 27, on Apple silicon or Intel.

It installs the app as a background service that starts at boot. On a first install it looks
for C1660w printers on the network and adds each one to macOS as "Dell C1660w". The printer
then appears in System Settings > Printers & Scanners.

| path | contents |
|---|---|
| `/usr/local/bin/c1660w-universal-mac-driver` | the program |
| `/Library/LaunchDaemons/com.warpgate.c1660w-universal-mac-driver.plist` | the service (runs as `_lp`, on `localhost:8631`) |
| `/Library/Application Support/c1660w-universal-mac-driver/` | saved printers and settings, spool, log |
| `/usr/local/share/c1660w-universal-mac-driver/` | licenses and `uninstall.sh` |

To add a printer later, use the web interface at <http://localhost:8631/>. From the command
line, `c1660w-universal-mac-driver autoadd` looks for C1660w printers on the network again,
and `c1660w-universal-mac-driver add -d NAME -m c1660w -v socket://HOST:9100` adds one by
address. Command-line calls such as `c1660w-universal-mac-driver printers` or `jobs` go to
the installed service.

To uninstall, run `sudo /usr/local/share/c1660w-universal-mac-driver/uninstall.sh`. Add
`--purge` to also delete saved printers and logs.

## Building

You need a C11 compiler, `make`, `pkg-config` and PAPPL 1.4 or later, with its
dependencies (OpenSSL or GnuTLS, libpng, libjpeg).

```sh
make            # library and command-line tools, in build/
make test       # unit tests
make app        # the Printer Application, build/c1660w-universal-mac-driver
```

`make app` looks for PAPPL in `PAPPL_PREFIX` (default `/usr/local`),
or anywhere `pkg-config` can find `pappl.pc`. For a static PAPPL build, `PKG_CONFIG_PATH`
must also find the `.pc` files of its dependencies. On macOS with Homebrew:

```sh
export PKG_CONFIG_PATH="$(brew --prefix openssl@3)/lib/pkgconfig:$(brew --prefix libpng)/lib/pkgconfig:$(brew --prefix jpeg-turbo)/lib/pkgconfig"
make app PAPPL_PREFIX=/path/to/pappl
```

### macOS installer

```sh
packaging/macos/build-deps.sh   # once: universal static OpenSSL, libpng, libjpeg-turbo, PAPPL
packaging/macos/build-pkg.sh    # build/c1660w-universal-mac-driver-VERSION.pkg
```

`build-deps.sh` downloads pinned, checksum-verified sources into `build-deps/`.
`build-pkg.sh` builds a universal binary and checks that it links only system libraries and
targets macOS 11. It then signs the binary (Developer ID, hardened runtime), builds a signed
installer and notarizes it. Use `SIGN=0` for an unsigned test build, or `NOTARIZE=0` to sign
without notarizing. The identities and the notarytool profile can be changed with
`APP_IDENTITY`, `PKG_IDENTITY` and `NOTARY_PROFILE`.

## Running

Start the server, listening only on this computer:

```sh
build/c1660w-universal-mac-driver server -o server-hostname=localhost \
    -o listen-hostname=localhost -o server-port=8631
```

The server saves its printers and settings and restores them on the next start. Add a
network printer by its hostname or address (raw socket, port 9100):

```sh
build/c1660w-universal-mac-driver add -d C1660w -m c1660w -v socket://PRINTER-HOSTNAME:9100
```

Then add it to your computer as a driverless printer. On macOS or Linux with CUPS:

```sh
lpadmin -p C1660w -E -v ipp://localhost:8631/ipp/print/C1660w -m everywhere
```

The server also has a web interface at <http://localhost:8631/>. Run
`build/c1660w-universal-mac-driver --help` for the other sub-commands (`printers`, `jobs`,
`status`, `submit`, `shutdown`, ...).

## Command-line tools

- `hbpl-encode` turns PAM page images (KCMY, or grayscale for monochrome) into an HBPL job.
- `hbpl-decode` prints the structure of an HBPL job and can write each page out as a PAM
  image.

## How it works

[SPEC.md](SPEC.md) describes the HBPL v1 job format and the encoder. The code is organized as
follows:

| path | contents |
|---|---|
| `src/` | HBPL encoder, job framing and decoder (C11, no dependencies) |
| `tools/` | `hbpl-encode` and `hbpl-decode` |
| `tests/` | unit tests |
| `app/` | the PAPPL Printer Application and color conversion |

The test suite can also check the encoder against real jobs from the original driver. These
captures are not distributed. If you have your own, set `HBPL_VECTORS` to their directory
before running `make test`. Without them those checks are skipped.

## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE), which also credit the prior
work this project builds on.

Dell is a trademark of Dell Inc. This project is not affiliated with or endorsed by Dell.
