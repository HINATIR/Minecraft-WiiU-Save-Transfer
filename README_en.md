# MCU Save Transfer

[Japanese](README.md)

`MCU Save Transfer` is a wxWidgets desktop application for transferring a Wii U world save to a Switch over a local UDP connection.

## Features

- Automatic Switch discovery on the local network, or manual Switch IPv4 entry.
- Selection of a world data file and an optional `.ext` file.
- `.ext` world-name preview and editing.
- `.ext` icon preview and PNG import.
- Optional `.ext` support with a built-in transparent default icon.
- Transfer status, progress, completion, cancellation, and error details in the UI.
- Debug communication log for diagnosing connection and transfer problems.

## How It Works

The application first waits for the Switch LAN session to become available. It then establishes the local PIA session, waits for the receiver to join, and transfers the combined save data in reliable chunks. Each chunk is acknowledged before the next transfer step continues. The progress bar represents the amount of combined world data and icon data sent successfully.

The transmitted data consists of:

```text
world file + .ext PNG payload
```

The 0x100-byte binary header of a selected `.ext` file is used for metadata and is not included in the transferred payload.

## `.ext` Metadata

When an `.ext` file is selected, the application displays the world name, seed, host options, extra-data value, and save metadata information. The world name can be edited and an icon PNG can be imported without modifying the original `.ext` file.

An `.ext` file is optional. When it is not selected, the application uses these defaults:

```text
4J_HOSTOPTIONS = 3e9c
4J_TEXTUREPACK = 0
4J_EXTRADATA   = 79900a8
4J_#LOADS      = 0
```

The default icon is a fully transparent PNG. Negative `4J_SEED` values are supported.

### Compatibility Warning

The world data and the `.ext` file must come from the same game update generation. If the world data is from after the Sea update but the `.ext` file is from before the Sea update, the world may appear to open successfully but can freeze before loading begins. The `4J_EXTRADATA` value appears to represent the save or game-data version, although this interpretation is not yet fully confirmed. Use matching values from the same update when possible.

## Sending a Save

1. Start `MCU_Save_Transfer` while the PC and Switch are on the same local network.
2. Choose `Auto` to detect the Switch, or choose `Manual IP` and enter its IPv4 address.
3. Select the world data file.
4. Select the matching `.ext` file, or leave it empty to use the built-in defaults.
5. Optionally edit the displayed world name or import an icon PNG.
6. Press `Send` and monitor the status label, progress bar, and debug log.

The status label reports `Ready`, `Discovering`, `Connecting`, `Waiting for receiver`, `Sending`, `Completed`, `Cancelled`, or an error followed by its reason.

## Build

The project uses git submodules for mbedTLS and wxWidgets. Initialize them before building:

```sh
git submodule update --init --recursive
```

With MinGW or another GNU Make environment:

```sh
make
```

With Visual Studio C++ Build Tools on Windows:

```bat
build-msvc.bat
```

The executable is written to:

```text
bin/MCU_Save_Transfer.exe
```

## Project Layout

- `src/main.cpp` - wxWidgets UI and user interaction.
- `src/transfer_worker.*` - transfer workflow and background sender.
- `src/pia_lan.*` - LAN discovery and session setup.
- `src/pia_packet.*` - encrypted PIA packet framing.
- `src/save_transfer_metadata.*` - `.ext` metadata and default payload handling.
- `src/reliable_sliding_window.*` - reliable fragmentation, acknowledgements, and retries.
- `third_party/mbedtls` - mbedTLS submodule.
- `third_party/wxWidgets` - wxWidgets submodule.

## Third-Party Libraries and Licenses

- [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) - mbedTLS 3.6.7. The source is available under a dual [Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0) or [GPL-2.0-or-later](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html) license. This project uses the Apache-2.0 option. The full notice is included in [`third_party/mbedtls/LICENSE`](third_party/mbedtls/LICENSE).
- [wxWidgets](https://github.com/wxWidgets/wxWidgets) - wxWidgets 3.2.8.1 under the wxWindows Library Licence 3.1, which includes an exception permitting binary applications to be distributed under their own terms. The full notice is included in [`third_party/wxWidgets/docs/licence.txt`](third_party/wxWidgets/docs/licence.txt).

wxWidgets also contains third-party components with their own notices. When distributing the application, retain the relevant license and copyright files from both submodules.
