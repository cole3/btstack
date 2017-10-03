/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

#define __BTSTACK_FILE__ "main.c"
 
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/stm32/dma.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/stm32/exti.h>
#include <libopencmsis/core_cm3.h>

#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_embedded.h"
#include "hci.h"
#include "bluetooth_company_id.h"
#ifdef BCM_BT_CHIP
#include "btstack_chipset_bcm.h"
#else
#include "btstack_chipset_cc256x.h"
#endif
#include "btstack_memory.h"
#include "classic/btstack_link_key_db.h"

// STDOUT_FILENO and STDERR_FILENO are defined by <unistd.h> with GCC
// (this is a hack for IAR)
#ifndef STDOUT_FILENO
#define STDERR_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

// Configuration
// LED2 on PA5
// Debug: USART1, TX on PA2
// Bluetooth: USART3. TX PB10, RX PB11, CTS PB13 (in), RTS PB14 (out), N_SHUTDOWN PB15
#define BT_LED_IO GPIO7
#define BT_LED_PORT GPIOC
#define USART_CONSOLE USART1
#define GPIO_BT_N_SHUTDOWN GPIO15

// btstack code starts there
void btstack_main(void);

static void bluetooth_power_cycle(void);

// hal_tick.h inmplementation
#include "hal_tick.h"

static void dummy_handler(void);
static void (*tick_handler)(void) = &dummy_handler;
static int hal_uart_needed_during_sleep = 1;

static void dummy_handler(void){};

static btstack_packet_callback_registration_t hci_event_callback_registration;

void hal_tick_init(void){
	systick_set_reload(800000);	// 1/4 of clock -> 250 ms tick
	systick_set_clocksource(STK_CSR_CLKSOURCE_AHB);
	systick_counter_enable();
	systick_interrupt_enable();
}

int  hal_tick_get_tick_period_in_ms(void){
    return 100;
}

void hal_tick_set_handler(void (*handler)(void)){
    if (handler == NULL){
        tick_handler = &dummy_handler;
        return;
    }
    tick_handler = handler;
}

void sys_tick_handler(void){
	(*tick_handler)();
}

static void msleep(uint32_t delay) {
	uint32_t wake = btstack_run_loop_embedded_get_ticks() + delay / hal_tick_get_tick_period_in_ms();
	while (wake > btstack_run_loop_embedded_get_ticks());
}

// hal_led.h implementation
#include "hal_led.h"
void hal_led_off(void);
void hal_led_on(void);

void hal_led_off(void){
	gpio_set(BT_LED_PORT, BT_LED_IO);
}
void hal_led_on(void){
	gpio_clear(BT_LED_PORT, BT_LED_IO);
}
void hal_led_toggle(void){
	gpio_toggle(BT_LED_PORT, BT_LED_IO);
}

// hal_cpu.h implementation
#include "hal_cpu.h"

void hal_cpu_disable_irqs(void){
	__disable_irq();
}

void hal_cpu_enable_irqs(void){
	__enable_irq();
}

void hal_cpu_enable_irqs_and_sleep(void){
	hal_led_off();
	__enable_irq();
	__asm__("wfe");	// go to sleep if event flag isn't set. if set, just clear it. IRQs set event flag

	// note: hal_uart_needed_during_sleep can be used to disable peripheral clock if it's not needed for a timer
	hal_led_on();
}

// hal_uart_dma.c implementation
#include "hal_uart_dma.h"

// handlers
static void (*rx_done_handler)(void) = dummy_handler;
static void (*tx_done_handler)(void) = dummy_handler;
static void (*cts_irq_handler)(void) = dummy_handler;

static void hal_uart_manual_rts_set(void){
	gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, GPIO_USART3_RTS);
}

static void hal_uart_manual_rts_clear(void){
	gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART3_RTS);
}

void hal_uart_dma_set_sleep(uint8_t sleep){
	if (sleep){
		hal_uart_manual_rts_set();
	} else {
		hal_uart_manual_rts_clear();
	}
	hal_uart_needed_during_sleep = !sleep;
}

// DMA1_CHANNEL2 UART3_TX
void dma1_channel2_isr(void) {
	if ((DMA1_ISR & DMA_ISR_TCIF2) != 0) {
		DMA1_IFCR |= DMA_IFCR_CTCIF2;
		dma_disable_transfer_complete_interrupt(DMA1, DMA_CHANNEL2);
		usart_disable_tx_dma(USART3);
		dma_disable_channel(DMA1, DMA_CHANNEL2);
		(*tx_done_handler)();
	}
}

// DMA1_CHANNEL2 UART3_RX
void dma1_channel3_isr(void){
	if ((DMA1_ISR & DMA_ISR_TCIF3) != 0) {
		DMA1_IFCR |= DMA_IFCR_CTCIF3;
		dma_disable_transfer_complete_interrupt(DMA1, DMA_CHANNEL3);
		usart_disable_rx_dma(USART3);
		dma_disable_channel(DMA1, DMA_CHANNEL3);

		// hal_uart_manual_rts_set();
		(*rx_done_handler)();
	}
}

// CTS RISING ISR
void exti15_10_isr(void){
	exti_reset_request(EXTI13);
	(*cts_irq_handler)();
}

void hal_uart_dma_init(void){
	bluetooth_power_cycle();
}
void hal_uart_dma_set_block_received( void (*the_block_handler)(void)){
    rx_done_handler = the_block_handler;
}

void hal_uart_dma_set_block_sent( void (*the_block_handler)(void)){
    tx_done_handler = the_block_handler;
}

void hal_uart_dma_set_csr_irq_handler( void (*the_irq_handler)(void)){
	if (the_irq_handler){
		/* Configure the EXTI13 interrupt (USART3_CTS is on PB13) */
		nvic_enable_irq(NVIC_EXTI15_10_IRQ);
		exti_select_source(EXTI13, GPIOB);
		exti_set_trigger(EXTI13, EXTI_TRIGGER_RISING);
		exti_enable_request(EXTI13);
	} else {
		exti_disable_request(EXTI13);
		nvic_disable_irq(NVIC_EXTI15_10_IRQ);
	}
    cts_irq_handler = the_irq_handler;
}

int  hal_uart_dma_set_baud(uint32_t baud){
	usart_disable(USART3);
	usart_set_baudrate(USART3, baud);
	usart_enable(USART3);
	return 0;
}

void hal_uart_dma_send_block(const uint8_t *data, uint16_t size){

	// printf("hal_uart_dma_send_block size %u\n", size);
	/*
	 * USART3_TX Using DMA_CHANNEL2 
	 */

	/* Reset DMA channel*/
	dma_channel_reset(DMA1, DMA_CHANNEL2);

	dma_set_peripheral_address(DMA1, DMA_CHANNEL2, (uint32_t)&USART3_DR);
	dma_set_memory_address(DMA1, DMA_CHANNEL2, (uint32_t)data);
	dma_set_number_of_data(DMA1, DMA_CHANNEL2, size);
	dma_set_read_from_memory(DMA1, DMA_CHANNEL2);
	dma_enable_memory_increment_mode(DMA1, DMA_CHANNEL2);
	dma_set_peripheral_size(DMA1, DMA_CHANNEL2, DMA_CCR_PSIZE_8BIT);
	dma_set_memory_size(DMA1, DMA_CHANNEL2, DMA_CCR_MSIZE_8BIT);
	dma_set_priority(DMA1, DMA_CHANNEL2, DMA_CCR_PL_VERY_HIGH);
	dma_enable_transfer_complete_interrupt(DMA1, DMA_CHANNEL2);
	dma_enable_channel(DMA1, DMA_CHANNEL2);
    usart_enable_tx_dma(USART3);
}

void hal_uart_dma_receive_block(uint8_t *data, uint16_t size){

	// hal_uart_manual_rts_clear();

	/*
	 * USART3_RX is on DMA_CHANNEL3
	 */

	// printf("hal_uart_dma_receive_block req size %u\n", size);

	/* Reset DMA channel*/
	dma_channel_reset(DMA1, DMA_CHANNEL3);

	dma_set_peripheral_address(DMA1, DMA_CHANNEL3, (uint32_t)&USART3_DR);
	dma_set_memory_address(DMA1, DMA_CHANNEL3, (uint32_t)data);
	dma_set_number_of_data(DMA1, DMA_CHANNEL3, size);
	dma_set_read_from_peripheral(DMA1, DMA_CHANNEL3);
	dma_enable_memory_increment_mode(DMA1, DMA_CHANNEL3);
	dma_set_peripheral_size(DMA1, DMA_CHANNEL3, DMA_CCR_PSIZE_8BIT);
	dma_set_memory_size(DMA1, DMA_CHANNEL3, DMA_CCR_MSIZE_8BIT);
	dma_set_priority(DMA1, DMA_CHANNEL3, DMA_CCR_PL_HIGH);
	dma_enable_transfer_complete_interrupt(DMA1, DMA_CHANNEL3);
	dma_enable_channel(DMA1, DMA_CHANNEL3);
    usart_enable_rx_dma(USART3);
}

// end of hal_uart

/**
 * Use USART_CONSOLE as a console.
 * This is a syscall for newlib
 * @param file
 * @param ptr
 * @param len
 * @return
 */
int _write(int file, char *ptr, int len);
int _write(int file, char *ptr, int len){
	int i;

	if (file == STDOUT_FILENO || file == STDERR_FILENO) {
		for (i = 0; i < len; i++) {
			if (ptr[i] == '\n') {
				usart_send_blocking(USART_CONSOLE, '\r');
			}
			usart_send_blocking(USART_CONSOLE, ptr[i]);
		}
		return i;
	}
	errno = EIO;
	return -1;
}

static void clock_setup(void){
	/* set rcc clk to 72 MHz */
	rcc_clock_setup_in_hse_12mhz_out_72mhz();

	/* Enable clocks for GPIO port A (for GPIO_USART1_TX) and USART1 + USART2. */
	/* needs to be done before initializing other peripherals */
	rcc_periph_clock_enable(RCC_GPIOA);
	rcc_periph_clock_enable(RCC_GPIOB);
	rcc_periph_clock_enable(RCC_GPIOC);
	rcc_periph_clock_enable(RCC_USART1);
	rcc_periph_clock_enable(RCC_USART2);
	rcc_periph_clock_enable(RCC_DMA1);
	rcc_periph_clock_enable(RCC_AFIO); // needed by EXTI interrupts
}

static void gpio_setup(void){
	/* Set output push-pull [LED] */
	gpio_set_mode(BT_LED_PORT, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, BT_LED_IO);

    hal_led_off();
}

static void debug_usart_setup(void){
	/* Setup GPIO pin GPIO_USART1_TX/GPIO2 on GPIO port A for transmit. */
	gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART1_TX);

	/* Setup UART parameters. */
	usart_set_baudrate(USART1, 115200);
	usart_set_databits(USART1, 8);
	usart_set_stopbits(USART1, USART_STOPBITS_1);
	usart_set_mode(USART1, USART_MODE_TX);
	usart_set_parity(USART1, USART_PARITY_NONE);
	usart_set_flow_control(USART1, USART_FLOWCONTROL_NONE);

	/* Finally enable the USART. */
	usart_enable(USART1);
}

static void bluetooth_setup(void){
	printf("\nBluetooth starting...\n");

	// n_shutdown as output
	gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_PUSHPULL,GPIO_BT_N_SHUTDOWN);

	// tx output
	gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART3_TX);
	// rts output (default to 1)
	gpio_set(GPIOB, GPIO_USART3_RTS);
	gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART3_RTS);
	// rx input
	gpio_set_mode(GPIOB, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, GPIO_USART3_RX);
	// cts as input
	gpio_set_mode(GPIOB, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, GPIO_USART3_CTS);


	/* Setup UART parameters. */
	usart_set_baudrate(USART3, 115200);
	usart_set_databits(USART3, 8);
	usart_set_stopbits(USART3, USART_STOPBITS_1);
	usart_set_mode(USART3, USART_MODE_TX_RX);
	usart_set_parity(USART3, USART_PARITY_NONE);
	usart_set_flow_control(USART3, USART_FLOWCONTROL_RTS);

	/* Finally enable the USART. */
	usart_enable(USART3);

	// TX
	nvic_set_priority(NVIC_DMA1_CHANNEL2_IRQ, 0);
	nvic_enable_irq(NVIC_DMA1_CHANNEL2_IRQ);

	// RX
	nvic_set_priority(NVIC_DMA1_CHANNEL3_IRQ, 0);
	nvic_enable_irq(NVIC_DMA1_CHANNEL3_IRQ);
}

// reset Bluetooth using n_shutdown
static void bluetooth_power_cycle(void){
	printf("Bluetooth power cycle\n");
	gpio_clear(BT_LED_PORT, BT_LED_IO);
	gpio_clear(GPIOB, GPIO_BT_N_SHUTDOWN);
	msleep(250);
	gpio_set(BT_LED_PORT, BT_LED_IO);
	gpio_set(GPIOB, GPIO_BT_N_SHUTDOWN);
	msleep(500);
}


// after HCI Reset, use 115200. Then increase baud reate to 468000.
// (on nucleo board without external crystall, running at 8 Mhz, 1 mbps was not possible)
static const hci_transport_config_uart_t config = {
	HCI_TRANSPORT_CONFIG_UART,
    115200,
    460800,
    1,
    NULL
};

#ifdef BCM_BT_CHIP
static void local_version_information_handler(uint8_t * packet){
    printf("Local version information:\n");
    uint16_t hci_version    = packet[6];
    uint16_t hci_revision   = little_endian_read_16(packet, 7);
    uint16_t lmp_version    = packet[9];
    uint16_t manufacturer   = little_endian_read_16(packet, 10);
    uint16_t lmp_subversion = little_endian_read_16(packet, 12);
    printf("- HCI Version    0x%04x\n", hci_version);
    printf("- HCI Revision   0x%04x\n", hci_revision);
    printf("- LMP Version    0x%04x\n", lmp_version);
    printf("- LMP Subversion 0x%04x\n", lmp_subversion);
    printf("- Manufacturer 0x%04x\n", manufacturer);
    switch (manufacturer){
        case BLUETOOTH_COMPANY_ID_BROADCOM_CORPORATION:   
            printf("Broadcom - using BCM driver.\n");
            hci_set_chipset(btstack_chipset_bcm_instance());
            break;
        default:
            printf("Unknown manufacturer / manufacturer not supported yet (%d).\n", manufacturer);
            break;
    }
}
#endif

static void packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    if (packet_type != HCI_EVENT_PACKET) return;
    switch(hci_event_packet_get_type(packet)){
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) return;
            printf("BTstack up and running.\n");
            break;
        case HCI_EVENT_COMMAND_COMPLETE:
#ifdef BCM_BT_CHIP
            if (HCI_EVENT_IS_COMMAND_COMPLETE(packet, hci_read_local_name)){
                if (hci_event_command_complete_get_return_parameters(packet)[0]) break;
                // terminate, name 248 chars
                packet[6+248] = 0;
                printf("Local name: %s\n", &packet[6]);
                //btstack_chipset_bcm_set_device_name((const char *)&packet[6]);
            }        
            if (HCI_EVENT_IS_COMMAND_COMPLETE(packet, hci_read_local_version_information)){
                local_version_information_handler(packet);
            }
#else
            if (HCI_EVENT_IS_COMMAND_COMPLETE(packet, hci_read_local_version_information)){
                uint16_t manufacturer   = little_endian_read_16(packet, 10);
                uint16_t lmp_subversion = little_endian_read_16(packet, 12);
                // assert manufacturer is TI
                if (manufacturer != BLUETOOTH_COMPANY_ID_TEXAS_INSTRUMENTS_INC){
                    printf("ERROR: Expected Bluetooth Chipset from TI but got manufacturer 0x%04x\n", manufacturer);
                    break;
                }
                // assert correct init script is used based on expected lmp_subversion
                if (lmp_subversion != btstack_chipset_cc256x_lmp_subversion()){
                    printf("Error: LMP Subversion does not match initscript! ");
                    printf("Your initscripts is for %s chipset\n", btstack_chipset_cc256x_lmp_subversion() < lmp_subversion ? "an older" : "a newer");
                    printf("Please update Makefile to include the appropriate bluetooth_init_cc256???.c file\n");
                    break;
                }
            }
#endif
            break;
        default:
            break;
    }
}

extern uint32_t rcc_apb1_frequency;
extern uint32_t rcc_apb2_frequency;
extern uint32_t rcc_ahb_frequency;

int main(void)
{
	clock_setup();
	gpio_setup();
	hal_tick_init();
	debug_usart_setup();

	printf("rcc ahb %u, apb1 %u, apb2 %u\n", 
			(unsigned int)rcc_ahb_frequency,
			(unsigned int)rcc_apb1_frequency,
			(unsigned int)rcc_apb2_frequency);
	hal_led_on();

	bluetooth_setup();

	// start with BTstack init - especially configure HCI Transport
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_embedded_get_instance());
    
    // init HCI
    hci_init(hci_transport_h4_instance(btstack_uart_block_embedded_instance()), (void*) &config);
    hci_set_link_key_db(btstack_link_key_db_memory_instance());
#ifdef BCM_BT_CHIP
    hci_set_chipset(btstack_chipset_bcm_instance());
#else
    hci_set_chipset(btstack_chipset_cc256x_instance());
#endif

    // inform about BTstack state
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

	// hand over to btstack embedded code 
    btstack_main();

    // go
    btstack_run_loop_execute();

	return 0;
}