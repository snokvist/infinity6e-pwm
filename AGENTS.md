# AGENTS.md

Living notes for repository work sessions, enhancements, and behavior decisions.

## Scope

This repository contains a standalone Buildroot package that patches SigmaStar Infinity6E PWM support with `duty_us` microsecond control.

## Current Baseline

- Package symbol: `BR2_PACKAGE_INFINITY6E_PWM`
- Patch delivery: `infinity6e-pwm.mk` via `LINUX_PATCHES += .../patches`
- Primary patch: `patches/0001-pwm-add-duty_us-sysfs-for-sigmastar.patch`
- User-facing sysfs additions: `duty_us` under PWM export nodes

## Guardrails For Changes

- Preserve current BSP ABI unless an explicit migration is documented.
- `period` interpreted as Hz (BSP-specific behavior).
- `duty_cycle` interpreted as integer percent.
- Keep `duty_us` path backward-compatible for existing scripts.
- Keep patch focused; avoid unrelated kernel refactors.

## Where To Update What

- Kernel or driver behavior: `patches/0001-pwm-add-duty_us-sysfs-for-sigmastar.patch`
- Buildroot wiring: `Config.in`, `infinity6e-pwm.mk`
- Standalone utility build flow: `Makefile` (for `files/waybeam-pwm.c`)
- Runtime helpers/examples: `files/`
- Human docs: `README.md`, `DOCUMENTATION.md`, and this file

## Validation Checklist

- Buildroot package still enables and builds.
- Booted target exposes `/sys/class/pwm/pwmchipX/pwmY/duty_us`.
- `duty_us` read/write works on active PWM channels.
- Legacy `duty_cycle` behavior still works.
- `files/infinity6e_pwm.sh` still functions for `center`, `us`, and `sweep`.

## Open Work Items

- Define desired defaults/limits for servo and motor profiles.
- Add a small scripted smoke test for sysfs PWM nodes.
- Decide whether `files/waybeam-pwm.c` should be packaged/installable or remain an example.
- Add optional network-side authentication/control constraints for `files/waybeam-pwm.c` when used beyond trusted networks.

## Session Notes

- 2026-02-20: Initialized repository-level `README.md` and `AGENTS.md` for ongoing enhancement tracking.
- 2026-02-20: Functional review of `files/waybeam-pwm.c` completed.
- Findings (high): UDP listener currently accepts control datagrams from any reachable host.
- Findings (medium): CRSF parser accepts length+CRC-valid frames without strict sync/address validation.
- Findings (low): Invalid numeric CLI arguments are ignored instead of returning a clear parse error.
- Build status: `gcc -O2 -Wall -Wextra -Wpedantic files/waybeam-pwm.c` builds cleanly.
- 2026-02-20: Implemented hardening in `files/waybeam-pwm.c` (no source whitelist, per request).
- Implemented: strict CLI numeric parsing with explicit error returns for invalid/missing values.
- Implemented: strict CRSF RC frame acceptance (`sync/address == 0xC8`, payload length must be 22 bytes).
- Implemented: socket error/hangup handling centers outputs and clears buffered stream state.
- 2026-02-20: Added root `Makefile` for `files/waybeam-pwm.c`.
- Makefile defaults to `arm-linux-gnueabihf-gcc` and provides `all`, `strip`, and `clean` targets.
- 2026-02-20: Expanded `files/waybeam-pwm.c` runtime diagnostics.
- Verbose `-v`: UDP packet reception + high-level RC state transitions.
- Verbose `-vv`: CRSF frame counters, channel mapping, and PWM output write logs.
- Verbose `-vvv`: extra-noisy diagnostics including unchanged output skips.
- 2026-02-20: Fixed CLI parsing so grouped verbosity flags work (`-vv`, `-vvv`, etc.).
- 2026-02-20: Field validation showed parser/output path works, but dual-output mode is board-limited by mux behavior.
- Confirmed working: `--pwm0-ch 1 --pwm1-ch 0` (single channel).
- Suspected root cause: two writes to mux register `0x1f207994` can override routing when both PWM channels are enabled.
- 2026-02-20: Added mux control CLI for `files/waybeam-pwm.c`.
- Added: `--no-mux`, `--mux-reg`, `--mux-pwm0`, and `--mux-pwm1`.
- Purpose: allow external board-specific mux setup and avoid forced in-app mux writes when dual-channel routing conflicts.
- 2026-02-20: Added `--mux-init-val` one-shot mode to avoid per-channel mux overwrite behavior.
- One-shot mode writes mux register once at startup and then skips per-channel mux writes.
- 2026-02-20: Set default dual-channel mux strategy to one-shot `0x1122` when no explicit mux mode is provided.
- 2026-02-20: Documented resolved dual-channel behavior.
- Behavior: dual-channel previously activated only one output due to per-channel mux overwrite on shared register.
- Fix: default to one-shot combined mux write (`0x1122`) for dual-channel startup, with explicit override options preserved.
