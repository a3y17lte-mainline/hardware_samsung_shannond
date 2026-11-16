# modemd

An open daemon for the Samsung Shannon modem of the Galaxy A3 2017 (SM-A320F,
Exynos7870), on the mainline kernel driver's WWAN ports. It replaces Samsung's
boot daemon and radio interface library; no Samsung userspace is used.

Layers:

- `core/`: no Android or front-end types. `Modem` (IPC and RFS channels,
  requests matched to their answers, notifications), `NvStore` + `RfsService`
  (the modem's non-volatile data, served from a file of our choice), and one
  module per domain: `Sim`, `Network`, `Data`, `Sms`, `Calls`. Single-threaded
  on an epoll `EventLoop`.
- `frontends/cli/`: `modemd` for Linux, one-line commands on a Unix socket.
- `frontends/aidl/`: the Android radio HAL (AIDL V5: config, modem, SIM,
  network, data, messaging, voice), `android.hardware.radio-service.a3y17lte`.
  The core runs on its own thread; binder calls are posted to its event loop.
  Each service derives from a generated base (`gen_stubs.py`, from the
  interfaces' frozen API) that answers REQUEST_NOT_SUPPORTED to everything it
  does not override. `Codec` (hex, base64, SIM file headers, AKA) has no
  Android dependencies and is tested on the host.
- `sim/`: a synthetic modem on SOCK_SEQPACKET socket pairs (which keep message
  boundaries like the WWAN ports) for the tests in `tests/`.

Message layouts come from libsamsung-ipc (`../libsamsung-ipc`, branch
a3y17lte), which carries the Shannon layouts worked out for this phone.

Build and test on the host (clang for its sanitizer runtime):

    CC=clang CXX=clang++ cmake -S . -B build -G Ninja -DMODEMD_SANITIZE=ON
    ninja -C build && build/modemd_tests

In an Android tree (`Android.bp`): put this repository and libsamsung-ipc
(its `Android.bp` builds `libsamsung-ipc-shannon`) into the tree, then build
`android.hardware.radio-service.a3y17lte`. The device side (init, sepolicy,
firmware path, NV setup) is in the a3y17lte device tree.

Cross build for the phone's Alpine: `cmake/alpine-aarch64.cmake` with
`-DALPINE_SYSROOT=...` (the project's tools/modem/build-userspace does this).

Safety: nothing dials, sends an SMS or enters a PIN unless a front end asks
for it; the NV store is pointed at a working copy, not at the EFS partition.

Not done: IMS (the hooks are in `Sim` and `Data`:
AKA, ISIM files, P-CSCF), Wi-Fi calling (data calls carry the access type so
it can be added), supplementary services, USSD, cell broadcast.
