################################################################################
#
# infinity6e-pwm
#
################################################################################

INFINITY6E_PWM_DEPENDENCIES = linux
INFINITY6E_PWM_PATCH_DIR = $(BR2_EXTERNAL_GENERAL_PATH)/package/infinity6e-pwm/patches

ifeq ($(BR2_PACKAGE_INFINITY6E_PWM),y)
LINUX_PATCHES += $(INFINITY6E_PWM_PATCH_DIR)
endif

define INFINITY6E_PWM_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -o $(@D)/waybeam-pwm \
		$(BR2_EXTERNAL_GENERAL_PATH)/package/infinity6e-pwm/files/waybeam-pwm.c
endef

define INFINITY6E_PWM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/waybeam-pwm $(TARGET_DIR)/usr/bin/waybeam-pwm
endef

$(eval $(generic-package))
