# ---------------------------------------------------------------------------
# STM32F407 ADIN2111 <-> USB CDC-ECM Ethernet bridge (FreeRTOS)
#
# Depends on the STM32CubeF4 package for CMSIS, the STM32F4xx HAL, the ST USB
# Device Library core and the FreeRTOS kernel. Fetch it once with:
#     make deps           (clones third_party/STM32CubeF4)
# or point CUBE at an existing checkout:
#     make CUBE=/path/to/STM32CubeF4
# ---------------------------------------------------------------------------

TARGET      = stm32f407-adin2111-ecm
BUILD_DIR   = build

# Toolchain
PREFIX      = arm-none-eabi-
CC          = $(PREFIX)gcc
AS          = $(PREFIX)gcc -x assembler-with-cpp
CP          = $(PREFIX)objcopy
SZ          = $(PREFIX)size

# STM32CubeF4 location (override on the command line if needed)
CUBE       ?= third_party/STM32CubeF4

CMSIS       = $(CUBE)/Drivers/CMSIS
HAL         = $(CUBE)/Drivers/STM32F4xx_HAL_Driver
USBCORE     = $(CUBE)/Middlewares/ST/STM32_USB_Device_Library/Core
FREERTOS    = $(CUBE)/Middlewares/Third_Party/FreeRTOS/Source

# ---------------------------------------------------------------------------
# MCU / architecture
# ---------------------------------------------------------------------------
CPU         = -mcpu=cortex-m4
FPU         = -mfpu=fpv4-sp-d16
FLOAT-ABI   = -mfloat-abi=hard
MCU         = $(CPU) -mthumb $(FPU) $(FLOAT-ABI)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
C_SOURCES = \
  Core/Src/main.c \
  Core/Src/bsp.c \
  Core/Src/stm32f4xx_it.c \
  Core/Src/stm32f4xx_hal_msp.c \
  Core/Src/stm32f4xx_hal_timebase_tim.c \
  Core/Src/syscalls.c \
  Drivers/ADIN2111/adin2111.c \
  Drivers/ADIN2111/adin2111_port_stm32.c \
  Net/frame_pool.c \
  Net/net_bridge.c \
  App/usb_device.c \
  Middlewares/USB_ECM/usbd_ecm.c \
  Middlewares/USB_ECM/usbd_ecm_if.c \
  Middlewares/USB_ECM/usbd_desc.c \
  Middlewares/USB_ECM/usbd_conf.c

# Vendor: HAL
C_SOURCES += \
  $(HAL)/Src/stm32f4xx_hal.c \
  $(HAL)/Src/stm32f4xx_hal_rcc.c \
  $(HAL)/Src/stm32f4xx_hal_rcc_ex.c \
  $(HAL)/Src/stm32f4xx_hal_gpio.c \
  $(HAL)/Src/stm32f4xx_hal_exti.c \
  $(HAL)/Src/stm32f4xx_hal_dma.c \
  $(HAL)/Src/stm32f4xx_hal_cortex.c \
  $(HAL)/Src/stm32f4xx_hal_pwr.c \
  $(HAL)/Src/stm32f4xx_hal_pwr_ex.c \
  $(HAL)/Src/stm32f4xx_hal_flash.c \
  $(HAL)/Src/stm32f4xx_hal_flash_ex.c \
  $(HAL)/Src/stm32f4xx_hal_spi.c \
  $(HAL)/Src/stm32f4xx_hal_tim.c \
  $(HAL)/Src/stm32f4xx_hal_tim_ex.c \
  $(HAL)/Src/stm32f4xx_hal_pcd.c \
  $(HAL)/Src/stm32f4xx_hal_pcd_ex.c \
  $(HAL)/Src/stm32f4xx_ll_usb.c

# Vendor: CMSIS system file
C_SOURCES += $(CMSIS)/Device/ST/STM32F4xx/Source/Templates/system_stm32f4xx.c

# Vendor: ST USB Device core
C_SOURCES += \
  $(USBCORE)/Src/usbd_core.c \
  $(USBCORE)/Src/usbd_ctlreq.c \
  $(USBCORE)/Src/usbd_ioreq.c

# Vendor: FreeRTOS kernel (native API)
C_SOURCES += \
  $(FREERTOS)/tasks.c \
  $(FREERTOS)/queue.c \
  $(FREERTOS)/list.c \
  $(FREERTOS)/timers.c \
  $(FREERTOS)/portable/GCC/ARM_CM4F/port.c \
  $(FREERTOS)/portable/MemMang/heap_4.c

# Startup (GCC)
ASM_SOURCES = $(CMSIS)/Device/ST/STM32F4xx/Source/Templates/gcc/startup_stm32f407xx.s

# ---------------------------------------------------------------------------
# Includes / defines
# ---------------------------------------------------------------------------
C_INCLUDES = \
  -Iconfig \
  -ICore/Inc \
  -IDrivers/ADIN2111 \
  -INet \
  -IApp \
  -IMiddlewares/USB_ECM \
  -I$(CMSIS)/Include \
  -I$(CMSIS)/Device/ST/STM32F4xx/Include \
  -I$(HAL)/Inc \
  -I$(HAL)/Inc/Legacy \
  -I$(USBCORE)/Inc \
  -I$(FREERTOS)/include \
  -I$(FREERTOS)/portable/GCC/ARM_CM4F

C_DEFS = -DUSE_HAL_DRIVER -DSTM32F407xx

# ---------------------------------------------------------------------------
# Flags
# ---------------------------------------------------------------------------
OPT       = -Og
WARN      = -Wall -Wextra -Wno-unused-parameter
CFLAGS    = $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) $(WARN) -g -gdwarf-2 \
            -fdata-sections -ffunction-sections -fstack-usage \
            -MMD -MP -MF"$(@:%.o=%.d)"
ASFLAGS   = $(MCU) $(OPT) -Wall -fdata-sections -ffunction-sections

LDSCRIPT  = ldscripts/STM32F407VGTx_FLASH.ld
LIBS      = -lc -lm -lnosys
LDFLAGS   = $(MCU) -specs=nano.specs -T$(LDSCRIPT) $(LIBS) \
            -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections

# ---------------------------------------------------------------------------
# Objects
# ---------------------------------------------------------------------------
OBJECTS  = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

# ---------------------------------------------------------------------------
# Rules
# ---------------------------------------------------------------------------
all: check-cube $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

check-cube:
	@test -d "$(HAL)/Src" || { \
	  echo "STM32CubeF4 not found at '$(CUBE)'."; \
	  echo "Run 'make deps' or set CUBE=/path/to/STM32CubeF4"; exit 1; }

$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(notdir $(<:.c=.lst)) $< -o $@

$(BUILD_DIR)/%.o: %.s Makefile | $(BUILD_DIR)
	$(AS) -c $(ASFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SZ) $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(CP) -O ihex $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(CP) -O binary -S $< $@

$(BUILD_DIR):
	mkdir -p $@

deps:
	scripts/get_deps.sh

flash: $(BUILD_DIR)/$(TARGET).elf
	openocd -f openocd.cfg -c "program $< verify reset exit"

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all check-cube deps flash clean

-include $(wildcard $(BUILD_DIR)/*.d)
