./pwm_100hz.sh pwm0 --hz 100 sweep


infinity6e_pwm_driver.sh pwm0 --hz 50 --min-us 1400 --center-us 1500 --max-us 1600 --step-us 10 --step-delay-ms 200 sweep
# Direct set
./servo_pwm_sigma_us.sh pwm0 us 1450
./servo_pwm_sigma_us.sh pwm0 us 1500
./servo_pwm_sigma_us.sh pwm0 us 1550

# PWM1 (if wired/routed)
./servo_pwm_sigma_us.sh pwm1 --hz 50 center
