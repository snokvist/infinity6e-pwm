################################################################################
#
# infinity6e-pwm
#
################################################################################

INFINITY6E_PWM_DEPENDENCIES = linux
INFINITY6E_PWM_PATCH_DIR = $(BR2_EXTERNAL_GENERAL_PATH)/package/infinity6e-pwm/patches

LINUX_PATCHES += $(INFINITY6E_PWM_PATCH_DIR)

$(eval $(generic-package))
