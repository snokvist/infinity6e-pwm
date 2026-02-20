#!/bin/sh
set -eu

# servo_pwm_sigma_us.sh
#
# SigmaStar PWM sysfs wrapper with support for new fine-control API:
#   period     = frequency in Hz
#   duty_cycle = integer percent (legacy)
#   duty_us    = pulse width in microseconds (new, preferred)
#
# Examples:
#   ./servo_pwm_sigma_us.sh pwm0 center
#   ./servo_pwm_sigma_us.sh pwm0 us 1500
#   ./servo_pwm_sigma_us.sh pwm0 sweep
#   ./servo_pwm_sigma_us.sh pwm1 --hz 50 --min-us 1000 --center-us 1500 --max-us 2000 sweep
#   ./servo_pwm_sigma_us.sh pwm0 pct 8         # legacy mode still supported
#
# Notes:
# - Uses duty_us if present, otherwise falls back to duty_cycle (%)
# - Servo defaults are conservative and easy to adjust
# - period is frequency in Hz on this SigmaStar BSP

PWMCHIP="/sys/class/pwm/pwmchip0"
MUX_REG="0x1f207994"

# Defaults
HZ=50
MIN_US=1000
CENTER_US=1500
MAX_US=2000
STEP_US=50
STEP_DELAY_MS=300

# Legacy percent defaults (only used if duty_us is not available)
MIN_PCT=5
CENTER_PCT=8
MAX_PCT=10

usage() {
  cat <<EOF
Usage:
  $0 pwm0|pwm1 [options] COMMAND [ARG]

Commands:
  center              Set center position
  min                 Set min position
  max                 Set max position
  us <N>              Set pulse width in microseconds (preferred)
  pct <N>             Set duty percent (legacy fallback)
  sweep               Sweep MIN_US -> MAX_US -> CENTER_US using duty_us
  info                Print current config + readback

Options:
  --hz N              PWM frequency in Hz (default: $HZ)
  --min-us N          Min pulse width in us (default: $MIN_US)
  --center-us N       Center pulse width in us (default: $CENTER_US)
  --max-us N          Max pulse width in us (default: $MAX_US)
  --step-us N         Sweep step size in us (default: $STEP_US)
  --step-delay-ms N   Delay between sweep steps (default: $STEP_DELAY_MS)

Examples:
  $0 pwm0 center
  $0 pwm0 us 1450
  $0 pwm0 --hz 50 --min-us 900 --center-us 1500 --max-us 2100 sweep
  $0 pwm1 pct 8
EOF
  exit 1
}

[ $# -ge 1 ] || usage
PWM_NAME="$1"
shift

case "$PWM_NAME" in
  pwm0) CH=0; MUX_VAL="0x01102" ;;
  pwm1) CH=1; MUX_VAL="0x01121" ;;
  *) usage ;;
esac

# Parse options
while [ $# -gt 0 ]; do
  case "$1" in
    --hz) [ $# -ge 2 ] || usage; HZ="$2"; shift 2 ;;
    --min-us) [ $# -ge 2 ] || usage; MIN_US="$2"; shift 2 ;;
    --center-us) [ $# -ge 2 ] || usage; CENTER_US="$2"; shift 2 ;;
    --max-us) [ $# -ge 2 ] || usage; MAX_US="$2"; shift 2 ;;
    --step-us) [ $# -ge 2 ] || usage; STEP_US="$2"; shift 2 ;;
    --step-delay-ms) [ $# -ge 2 ] || usage; STEP_DELAY_MS="$2"; shift 2 ;;
    *) break ;;
  esac
done

[ $# -ge 1 ] || usage
CMD="$1"
ARG="${2:-}"

P="$PWMCHIP/pwm$CH"
DUTY_US_NODE="$P/duty_us"
DUTY_PCT_NODE="$P/duty_cycle"

is_uint() {
  case "$1" in
    ''|*[!0-9]*) return 1 ;;
    *) return 0 ;;
  esac
}

# Fallback conversion for legacy percent-only mode:
# duty_pct = round(us * HZ / 10000)
us_to_pct() {
  us="$1"
  echo $(( (us * HZ + 5000) / 10000 ))
}

# Approximate pct -> us (for info)
pct_to_us() {
  pct="$1"
  echo $(( (pct * 10000 + HZ/2) / HZ ))
}

ensure_pwm() {
  devmem "$MUX_REG" 16 "$MUX_VAL" >/dev/null
  [ -d "$P" ] || echo "$CH" > "$PWMCHIP/export"

  echo 0 > "$P/enable" || true
  echo "$HZ" > "$P/period"

  # Seed with center before enable
  if [ -e "$DUTY_US_NODE" ]; then
    echo "$CENTER_US" > "$DUTY_US_NODE"
  else
    echo "$(us_to_pct "$CENTER_US")" > "$DUTY_PCT_NODE"
  fi

  echo 1 > "$P/enable"
}

set_us() {
  us="$1"
  if [ -e "$DUTY_US_NODE" ]; then
    echo "$us" > "$DUTY_US_NODE"
  else
    pct="$(us_to_pct "$us")"
    echo "$pct" > "$DUTY_PCT_NODE"
  fi
}

set_pct() {
  pct="$1"
  echo "$pct" > "$DUTY_PCT_NODE"
}

read_current_us() {
  if [ -e "$DUTY_US_NODE" ]; then
    cat "$DUTY_US_NODE"
  else
    pct="$(cat "$DUTY_PCT_NODE")"
    pct_to_us "$pct"
  fi
}

print_info() {
  rb_mux="$(devmem "$MUX_REG" 16 2>/dev/null || echo '?')"
  rb_period="$(cat "$P/period" 2>/dev/null || echo '?')"
  rb_enable="$(cat "$P/enable" 2>/dev/null || echo '?')"

  echo "PWM:      $PWM_NAME (CH=$CH)"
  echo "MUX:      $MUX_REG = $rb_mux (target $MUX_VAL)"
  echo "Freq:     ${rb_period} Hz"
  echo "Enable:   $rb_enable"

  if [ -e "$DUTY_US_NODE" ]; then
    rb_us="$(cat "$DUTY_US_NODE" 2>/dev/null || echo '?')"
    rb_pct="$(cat "$DUTY_PCT_NODE" 2>/dev/null || echo '?')"
    echo "Duty API: duty_us (fine) + duty_cycle (legacy)"
    echo "Pulse:    ${rb_us} us"
    echo "Duty %:   ${rb_pct} %"
  else
    rb_pct="$(cat "$DUTY_PCT_NODE" 2>/dev/null || echo '?')"
    approx_us="$(read_current_us 2>/dev/null || echo '?')"
    echo "Duty API: duty_cycle only (legacy)"
    echo "Duty %:   ${rb_pct} %"
    echo "Pulse:    ~${approx_us} us"
  fi

  echo "Config:   HZ=$HZ MIN_US=$MIN_US CENTER_US=$CENTER_US MAX_US=$MAX_US STEP_US=$STEP_US"
}

sleep_ms() {
  # BusyBox usually has usleep, fallback to sleep
  ms="$1"
  if command -v usleep >/dev/null 2>&1; then
    usleep $((ms * 1000))
  else
    # fallback (coarser)
    sleep 1
  fi
}

sweep_us() {
  echo "Sweeping $PWM_NAME using duty_us-compatible path: ${MIN_US}us -> ${MAX_US}us -> ${CENTER_US}us"
  v="$MIN_US"
  while [ "$v" -le "$MAX_US" ]; do
    set_us "$v"
    sleep_ms "$STEP_DELAY_MS"
    v=$((v + STEP_US))
  done

  v="$MAX_US"
  while [ "$v" -ge "$MIN_US" ]; do
    set_us "$v"
    sleep_ms "$STEP_DELAY_MS"
    v=$((v - STEP_US))
  done

  set_us "$CENTER_US"
}

# Validate numeric options
for n in "$HZ" "$MIN_US" "$CENTER_US" "$MAX_US" "$STEP_US" "$STEP_DELAY_MS"; do
  is_uint "$n" || { echo "Numeric option expected, got '$n'"; exit 1; }
done

ensure_pwm

case "$CMD" in
  info)
    print_info
    ;;
  center)
    set_us "$CENTER_US"
    print_info
    ;;
  min)
    set_us "$MIN_US"
    print_info
    ;;
  max)
    set_us "$MAX_US"
    print_info
    ;;
  us)
    [ -n "$ARG" ] || usage
    is_uint "$ARG" || { echo "us requires integer microseconds"; exit 1; }
    set_us "$ARG"
    print_info
    ;;
  pct)
    [ -n "$ARG" ] || usage
    is_uint "$ARG" || { echo "pct requires integer percent"; exit 1; }
    set_pct "$ARG"
    print_info
    ;;
  sweep)
    sweep_us
    print_info
    ;;
  *)
    usage
    ;;
esac
