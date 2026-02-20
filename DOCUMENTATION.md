# infinity6e-pwm

Standalone Buildroot package for SigmaStar Infinity6E PWM fine pulse-width control.

## Purpose

This package applies a kernel patch that adds a new PWM sysfs node:

- `/sys/class/pwm/pwmchipX/pwmY/duty_us` (R/W, integer microseconds)

It keeps existing behavior unchanged:

- `period` remains frequency-style in this BSP ABI
- `duty_cycle` remains integer percent-style in this BSP ABI

## How To Use

1. Enable this package in your defconfig:
   - `BR2_PACKAGE_INFINITY6E_PWM=y`
2. Build firmware as usual.
3. On target, use PWM sysfs:
   - `echo 50 > /sys/class/pwm/pwmchip0/pwm0/period`
   - `echo 1500 > /sys/class/pwm/pwmchip0/pwm0/duty_us`
   - `cat /sys/class/pwm/pwmchip0/pwm0/duty_us`

## Migration Notes (from previous test setup)

Previous test setup:
- Patch was carried via `linux-patcher`.

Current standalone setup:
- Patch is carried only by this package:
  - `package/infinity6e-pwm/patches/0001-pwm-add-duty_us-sysfs-for-sigmastar.patch`
- `infinity6e-pwm.mk` applies its own patch directory directly through `LINUX_PATCHES`.
- No `linux-patcher` coupling or duplicate-protection logic is included.

## Code-Level Changes Included In The Patch

- Generic PWM framework:
  - `include/linux/pwm.h`
  - `drivers/pwm/core.c`
  - `drivers/pwm/sysfs.c`
- SigmaStar driver:
  - `drivers/sstar/pwm/mdrv_pwm.c`
  - `drivers/sstar/pwm/infinity6e/mhal_pwm.h`
  - `drivers/sstar/pwm/infinity6e/mhal_pwm.c`

## Functional Summary

- Adds microsecond duty API path (`duty_us`) for finer servo/motor control.
- Preserves existing sysfs ABI for compatibility.
- Supports Infinity6E HAL paths used by this BSP.
