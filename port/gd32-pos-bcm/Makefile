#
# Makefile for GD32-pos-bcm Nucleo board with bcm Bluetooth modules
#

# BINARY=led_counter
BINARY=spp_counter
OPENCM3_DIR = libopencm3
LDSCRIPT = gd32-pos-bcm.ld
BTCHIP=bcm
#BTCHIP=cc256x

BTSTACK_ROOT = ../..
VPATH  = $(BTSTACK_ROOT)/example
VPATH += $(BTSTACK_ROOT)/src
VPATH += $(BTSTACK_ROOT)/src/classic
VPATH += $(BTSTACK_ROOT)/platform/embedded
VPATH += $(BTSTACK_ROOT)/chipset/$(BTCHIP)

CORE = \
	main.c 					    \
	bcm_patch.c                 \
    btstack_linked_list.c	    \
    btstack_memory.c            \
    btstack_memory_pool.c       \
    btstack_run_loop.c	        \
    btstack_run_loop_embedded.c \

COMMON = \
	ad_parser.c 			  \
	btstack_chipset_bcm.c    \
	btstack_chipset_bcm_download_firmware.c \
	btstack_link_key_db_memory.c \
	rfcomm.c			      \
	sdp_client_rfcomm.c 		  \
    btstack_util.c			  \
    hci.c			          \
    hci_cmd.c		          \
    hci_dump.c		          \
    btstack_uart_block_embedded.c \
    hci_transport_h4.c        \
    l2cap.c			          \
    l2cap_signaling.c         \
    sdp_client.c              \
    sdp_server.c			  \
    sdp_util.c                \
    spp_server.c			  \


CORE_OBJ   = $(CORE:.c=.o)
COMMON_OBJ = $(COMMON:.c=.o)

OBJS += $(CORE_OBJ) $(COMMON_OBJ) 

CFLAGS = -I. \
		 -I$(BTSTACK_ROOT)/platform/embedded \
		 -I$(BTSTACK_ROOT)/src \
		 -I$(BTSTACK_ROOT)/chipset/$(BTCHIP)

ifeq ($(BTCHIP), bcm)
	CFLAGS += -DBCM_BT_CHIP
endif

examples: bcm20710a1.hcd libopencm3/lib/libopencm3_stm32f1.a spp_counter.elf 

include libopencm3.stm32f1.mk

clean:
	rm -f *.map *.o *.d *.out *.elf *.bin *.hex 

distclean: clean
	make -C libopencm3 clean

# fetch and convert init scripts
bcm20710a1.hcd:
	curl -O -L https://github.com/OpenELEC/wlan-firmware/raw/master/firmware/brcm/bcm20710a1.hcd
	echo "#define __BTSTACK_FILE__ "bcm_patch.c"" > bcm_patch.c
	echo "" >> bcm_patch.c
	echo "#include <stdint.h>" >> bcm_patch.c
	echo "" >> bcm_patch.c
	echo "const uint8_t brcm_patchram_buf[] = {" >> bcm_patch.c
	hexdump -v -e '"0x" 1/1 "%02X" ", "' bcm20710a1.hcd >> bcm_patch.c
	echo "};" >> bcm_patch.c
	echo "" >> bcm_patch.c
	echo "const int brcm_patch_ram_length = `wc -c < bcm20710a1.hcd`;" >> bcm_patch.c
	echo "" >> bcm_patch.c
	echo "const char brcm_patch_version[] = \"0.0.0\";" >> bcm_patch.c

# git clone and compile libopencm3
libopencm3/lib/libopencm3_stm32f1.a:
	rm -rf libopencm3
	git clone https://github.com/libopencm3/libopencm3.git
	make -C libopencm3


