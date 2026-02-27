# infinity6e-pwm

Standalone Buildroot package for SigmaStar Infinity6E PWM fine pulse-width control.
<img width="617" height="514" alt="Screenshot From 2026-02-27 19-23-24" src="https://github.com/user-attachments/assets/3434208c-a2f6-47ec-8b2a-76ca3ab67b04" />
<img width="506" height="427" alt="image" src="https://github.com/user-attachments/assets/cf92d50b-8bae-472e-916e-b550d8555cb4" />



## What This Package Adds

- Kernel patch to expose `/sys/class/pwm/pwmchipX/pwmY/duty_us` (microseconds).
- Keeps existing BSP behavior unchanged.
- `period` remains frequency-style on this BSP.
- `duty_cycle` remains integer percent-style on this BSP.

## Repository Layout

- `Config.in`: Buildroot config symbol (`BR2_PACKAGE_INFINITY6E_PWM`).
- `infinity6e-pwm.mk`: package makefile; injects kernel patches via `LINUX_PATCHES`.
- `Makefile`: build helper for `files/waybeam-pwm.c` (cross-compiling by default).
- `patches/0001-pwm-add-duty_us-sysfs-for-sigmastar.patch`: kernel and driver changes.
- `files/infinity6e_pwm.sh`: target helper script for PWM setup/testing.
- `files/waybeam-pwm.c`: UDP/CRSF-to-PWM utility example.
- `DOCUMENTATION.md`: deeper technical notes.

## Buildroot Integration

1. Ensure this package is available in your Buildroot external tree.
2. Enable `BR2_PACKAGE_INFINITY6E_PWM=y`.
3. Build firmware normally.

## Target Quick Check

```sh
echo 50 > /sys/class/pwm/pwmchip0/pwm0/period
echo 1500 > /sys/class/pwm/pwmchip0/pwm0/duty_us
cat /sys/class/pwm/pwmchip0/pwm0/duty_us
```

If `duty_us` is missing, the patch is not applied in the built kernel.

## Build waybeam-pwm

The repository includes a root `Makefile` for `files/waybeam-pwm.c`.

```sh
make
make strip
make clean
```

Defaults:
- Compiler: `arm-linux-gnueabihf-gcc`
- Stripper: `arm-linux-gnueabihf-strip`

Host-build override example:

```sh
make CC=gcc STRIP=strip
```

## waybeam-pwm Verbose Logging

`files/waybeam-pwm.c` now has three verbosity levels:

- `-v`: packet/state logs (UDP packet reception with sender and RC/no-RC result).
- `-vv`: frame counters and output activity (frame stats, channel mapping, PWM writes).
- `-vvv`: very noisy mode including unchanged PWM write skips.
- You can use grouped flags (`-vv`, `-vvv`) or repeat `-v` multiple times.

## waybeam-pwm Mux Control

`files/waybeam-pwm.c` now supports mux control from CLI:

- `--no-mux`: do not write mux register from `waybeam-pwm` (recommended workaround for board-specific dual-channel conflicts).
- `--mux-reg ADDR`: override mux register address.
- `--mux-pwm0 VAL`: override mux value used when initializing `pwm0`.
- `--mux-pwm1 VAL`: override mux value used when initializing `pwm1`.
- `--mux-init-val VAL`: write mux register once at startup and skip per-channel mux writes.
- Default behavior when both PWM channels are enabled and no explicit mux strategy is provided: one-shot mux write with `0x1122`.

Examples:

```sh
# Let application write mux (default behavior, but values explicit)
./waybeam-pwm --port 8999 --pwm0-ch 1 --pwm1-ch 2 \
  --mux-reg 0x1f207994 --mux-pwm0 0x1102 --mux-pwm1 0x1121 -vv

# External mux setup + app skips mux writes
devmem 0x1f207994 16 0x01102
devmem 0x1f207994 16 0x01121
./waybeam-pwm --port 8999 --pwm0-ch 1 --pwm1-ch 2 --no-mux -vv

# One-shot combined mux write (avoids per-channel overwrite pattern)
./waybeam-pwm --port 8999 --pwm0-ch 1 --pwm1-ch 2 --mux-init-val 0x1122 -vv
```

## Dual-Channel Mux Behavior And Fix

Observed behavior:
- In dual-output mode, only one PWM channel could be active even though CRSF parsing and PWM writes were correct.
- Single-output mode worked as expected.

Root cause:
- The board uses a shared mux register (`0x1f207994`) for PWM routing.
- Sequential per-channel mux writes (`0x1102` then `0x1121`) are full register states, so the later write can override the earlier routing.

Implemented fix:
- Added one-shot mux mode (`--mux-init-val`) to write a combined register state once at startup.
- Set default dual-channel startup behavior to one-shot `0x1122` when both channels are enabled and no explicit mux mode is chosen.
- Kept manual override paths (`--no-mux`, `--mux-init-val`, `--mux-pwm0`, `--mux-pwm1`) for board-specific tuning.

## Development Notes

- Any PWM behavior change should keep ABI compatibility unless explicitly planned.
- Update both `patches/...patch` and `DOCUMENTATION.md` when behavior changes.
- Keep examples in `files/` aligned with current sysfs interface.

## waybeam-pwm.c Status (2026-02-20)

- `files/waybeam-pwm.c` is usable as a lab/test bridge on a trusted network.
- It is not yet production-safe for exposed networks or safety-critical actuation.
- Current known gaps include unauthenticated UDP control from any reachable source.
- Hardening implemented: strict CRSF address validation (`0xC8`) and strict RC payload length validation.
- Hardening implemented: invalid/missing numeric CLI options now fail fast with explicit error messages.
- Hardening implemented: socket error/hangup events center outputs and clear stale parser state.
- Board note: dual-output routing depends on mux configuration; default dual-channel startup now uses one-shot `--mux-init-val 0x1122` behavior.
