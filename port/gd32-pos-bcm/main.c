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
#include <libopencm3/stm32/timer.h>
#include <libopencmsis/core_cm3.h>

#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_embedded.h"
#include "hci.h"
#include "hci_dump.h"
#include "bluetooth_company_id.h"
#ifdef BCM_BT_CHIP
#include "btstack_chipset_bcm.h"
#include "btstack_chipset_bcm_download_firmware.h"
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
// LED on PC7
// Debug: USART1, TX on PA9
// Bluetooth: USART2. TX PA2, RX PA3, CTS PA0 (in), RTS PA1 (out), N_RST PC2
#define BT_LED_IO GPIO7
#define BT_LED_PORT GPIOC
#define USART_CONSOLE USART1
#define BT_N_RST_PORT GPIOC
#define BT_N_RST_IO GPIO2
#define BT_N_WAKE_PORT GPIOA
#define BT_N_WAKE_IO GPIO5
#define BT_HOST_WAKE_PORT GPIOA
#define BT_HOST_WAKE_IO GPIO4


extern uint32_t rcc_apb1_frequency;
extern uint32_t rcc_apb2_frequency;
extern uint32_t rcc_ahb_frequency;


// btstack code starts there
extern void btstack_main(void);

static void bluetooth_power_cycle(void);

// hal_tick.h inmplementation
#include "hal_tick.h"

static void dummy_handler(void);
static void (*tick_handler)(void) = &dummy_handler;
static int hal_uart_needed_during_sleep = 1;
static btstack_uart_config_t uart_config;

static void dummy_handler(void){};

static btstack_packet_callback_registration_t hci_event_callback_registration;

void hal_tick_init(void){
	systick_set_reload(7200000);	// 100 ms tick
	systick_set_clocksource(STK_CSR_CLKSOURCE_AHB);
	systick_counter_enable();
	systick_interrupt_enable();
}

int hal_tick_get_tick_period_in_ms(void){
    return 100; // 100 ms tick
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
	static int n;
	n++;
	if (n % 10 == 0) {
		//printf("tick n %d\n", n++);
	}
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
	gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, GPIO_USART2_RTS);
}

static void hal_uart_manual_rts_clear(void){
	gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART2_RTS);
}

void hal_uart_dma_set_sleep(uint8_t sleep){
	if (sleep){
		hal_uart_manual_rts_set();
	} else {
		hal_uart_manual_rts_clear();
	}
	hal_uart_needed_during_sleep = !sleep;
}

// DMA1_CHANNEL7 UART2_TX
void dma1_channel7_isr(void) {
	if ((DMA1_ISR & DMA_ISR_TCIF7) != 0) {
		DMA1_IFCR |= DMA_IFCR_CTCIF7;
		dma_disable_transfer_complete_interrupt(DMA1, DMA_CHANNEL7);
		usart_disable_tx_dma(USART2);
		dma_disable_channel(DMA1, DMA_CHANNEL7);
		(*tx_done_handler)();
	}
}

// DMA1_CHANNEL6 UART2_RX
void dma1_channel6_isr(void){
	if ((DMA1_ISR & DMA_ISR_TCIF6) != 0) {
		DMA1_IFCR |= DMA_IFCR_CTCIF6;
		dma_disable_transfer_complete_interrupt(DMA1, DMA_CHANNEL6);
		usart_disable_rx_dma(USART2);
		dma_disable_channel(DMA1, DMA_CHANNEL6);
		
		//unsigned char *p = (unsigned char *)*(unsigned long *)0x40020078;
		//printf("data @ %p\n", p);
		//printf("rcv %02x, %02x, %02x, %02x, %02x, %02x, %02x\n", *p, *(p+1), *(p+2), *(p+3), *(p+4), *(p+5), *(p+6));
		// hal_uart_manual_rts_set();
		(*rx_done_handler)();
	}
}

// CTS RISING ISR
void exti0_isr(void){
	exti_reset_request(EXTI0);
	(*cts_irq_handler)();
}

void hal_uart_dma_init(void){
	static int uart_init = 0;
	if (!uart_init) {
		uart_init = 1;
		bluetooth_power_cycle();
	}
}
void hal_uart_dma_set_block_received( void (*the_block_handler)(void)){
    rx_done_handler = the_block_handler;
}

void hal_uart_dma_set_block_sent( void (*the_block_handler)(void)){
    tx_done_handler = the_block_handler;
}

void hal_uart_dma_set_csr_irq_handler( void (*the_irq_handler)(void)){
	if (the_irq_handler){
		/* Configure the EXTI0 interrupt (USART2_CTS is on PA0) */
		nvic_enable_irq(NVIC_EXTI0_IRQ);
		exti_select_source(EXTI0, GPIOA);
		exti_set_trigger(EXTI0, EXTI_TRIGGER_RISING);
		exti_enable_request(EXTI0);
	} else {
		exti_disable_request(EXTI0);
		nvic_disable_irq(NVIC_EXTI0_IRQ);
	}
    cts_irq_handler = the_irq_handler;
}

int hal_uart_dma_set_baud(uint32_t baud){
	usart_disable(USART2);
	printf("hal_uart_dma_set_baud baud %lu\n", (unsigned long)baud);
	usart_set_baudrate(USART2, baud);
	usart_enable(USART2);
	return 0;
}

void hal_uart_dma_send_block(const uint8_t *data, uint16_t size){

	printf("hal_uart_dma_send_block size %u\n", size);
	/*
	 * USART2_TX Using DMA_CHANNEL7
	 */

	/* Reset DMA channel*/
	dma_channel_reset(DMA1, DMA_CHANNEL7);

	dma_set_peripheral_address(DMA1, DMA_CHANNEL7, (uint32_t)&USART2_DR);
	dma_set_memory_address(DMA1, DMA_CHANNEL7, (uint32_t)data);
	dma_set_number_of_data(DMA1, DMA_CHANNEL7, size);
	dma_set_read_from_memory(DMA1, DMA_CHANNEL7);
	dma_enable_memory_increment_mode(DMA1, DMA_CHANNEL7);
	dma_set_peripheral_size(DMA1, DMA_CHANNEL7, DMA_CCR_PSIZE_8BIT);
	dma_set_memory_size(DMA1, DMA_CHANNEL7, DMA_CCR_MSIZE_8BIT);
	dma_set_priority(DMA1, DMA_CHANNEL7, DMA_CCR_PL_VERY_HIGH);
	dma_enable_transfer_complete_interrupt(DMA1, DMA_CHANNEL7);
	dma_enable_channel(DMA1, DMA_CHANNEL7);
    usart_enable_tx_dma(USART2);

#if 0
#define UASRT2BASE 0x40004400
	printf("usart2 str 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x00));
	printf("usart2 dr 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x04));
	printf("usart2 brr 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x08));
	printf("usart2 ctlr1 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x0C));
	printf("usart2 ctlr2 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x10));
	printf("usart2 ctlr3 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x14));
	printf("usart2 gtpr 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x1C));

	msleep(500);

	printf("hal_uart_dma_send_block DMA_IFR 0x%lx\n",
			*(volatile unsigned long *)(0x40020000 + 0));
	printf("hal_uart_dma_send_block DMA_CTLR7 0x%lx\n",
			*(volatile unsigned long *)(0x40020000 + 0x80));
	printf("hal_uart_dma_send_block DMA_RCNT7 0x%lx\n",
			*(volatile unsigned long *)(0x40020000 + 0x84));
	printf("hal_uart_dma_send_block DMA_PBAR7 0x%lx\n",
			*(volatile unsigned long *)(0x40020000 + 0x88));
	printf("hal_uart_dma_send_block DMA_MBAR7 0x%lx\n",
			*(volatile unsigned long *)(0x40020000 + 0x8C));

	printf("usart2 str 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x00));
	printf("usart2 dr 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x04));
	printf("usart2 brr 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x08));
	printf("usart2 ctlr1 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x0C));
	printf("usart2 ctlr2 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x10));
	printf("usart2 ctlr3 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x14));
	printf("usart2 gtpr 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x1C));
#endif
}

void hal_uart_dma_receive_block(uint8_t *data, uint16_t size){

	//hal_uart_manual_rts_clear();

	/*
	 * USART2_RX is on DMA_CHANNEL6
	 */

	printf("hal_uart_dma_receive_block req size %u\n", size);

	/* Reset DMA channel*/
	dma_channel_reset(DMA1, DMA_CHANNEL6);

	dma_set_peripheral_address(DMA1, DMA_CHANNEL6, (uint32_t)&USART2_DR);
	dma_set_memory_address(DMA1, DMA_CHANNEL6, (uint32_t)data);
	dma_set_number_of_data(DMA1, DMA_CHANNEL6, size);
	dma_set_read_from_peripheral(DMA1, DMA_CHANNEL6);
	dma_enable_memory_increment_mode(DMA1, DMA_CHANNEL6);
	dma_set_peripheral_size(DMA1, DMA_CHANNEL6, DMA_CCR_PSIZE_8BIT);
	dma_set_memory_size(DMA1, DMA_CHANNEL6, DMA_CCR_MSIZE_8BIT);
	dma_set_priority(DMA1, DMA_CHANNEL6, DMA_CCR_PL_HIGH);
	dma_enable_transfer_complete_interrupt(DMA1, DMA_CHANNEL6);
	dma_enable_channel(DMA1, DMA_CHANNEL6);
    usart_enable_rx_dma(USART2);
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
	rcc_periph_clock_enable(RCC_TIM3);
}

static void gpio_setup(void){
	/* Set output push-pull [LED] */
	gpio_set_mode(BT_LED_PORT, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, BT_LED_IO);

    hal_led_off();
}

static void debug_usart_setup(void){
	/* Setup GPIO pin GPIO_USART1_TX/GPIO2 on GPIO port A for transmit. */
	gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART1_TX);
	gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, GPIO_USART1_RX);

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

	// reset as output
	gpio_set_mode(BT_N_RST_PORT, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, BT_N_RST_IO);
	// wakeup as output
	gpio_set_mode(BT_N_WAKE_PORT, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, BT_N_WAKE_IO);
	// host wakeup as input
	gpio_set_mode(BT_HOST_WAKE_PORT, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, BT_HOST_WAKE_IO);

	// tx output
	gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART2_TX);
	// rts output (default to 1)
	gpio_set(GPIOA, GPIO_USART2_RTS);
	gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART2_RTS);
	// rx input
	gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, GPIO_USART2_RX);
	// cts as input
	gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, GPIO_USART2_CTS);

	/* Setup UART parameters. */
	usart_set_baudrate(USART2, 115200);
	usart_set_databits(USART2, 8);
	usart_set_stopbits(USART2, USART_STOPBITS_1);
	usart_set_mode(USART2, USART_MODE_TX_RX);
	usart_set_parity(USART2, USART_PARITY_NONE);
	usart_set_flow_control(USART2, USART_FLOWCONTROL_RTS);
	//usart_set_flow_control(USART2, USART_FLOWCONTROL_NONE);

	/* Finally enable the USART. */
	usart_enable(USART2);

	// TX
	nvic_set_priority(NVIC_DMA1_CHANNEL7_IRQ, 0);
	nvic_enable_irq(NVIC_DMA1_CHANNEL7_IRQ);

	// RX
	nvic_set_priority(NVIC_DMA1_CHANNEL6_IRQ, 0);
	nvic_enable_irq(NVIC_DMA1_CHANNEL6_IRQ);

#if 0
#define UASRT2BASE 0x40004400
	printf("usart2 str 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x00));
	printf("usart2 dr 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x04));
	printf("usart2 brr 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x08));
	printf("usart2 ctlr1 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x0C));
	printf("usart2 ctlr2 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x10));
	printf("usart2 ctlr3 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x14));
	printf("usart2 gtpr 0x%lx\n",
			*(volatile unsigned long *)(UASRT2BASE + 0x1C));
#endif
}

static void bluetooth_clk_init(void)
{
	gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO6);

	timer_set_mode(TIM3, TIM_CR1_CKD_CK_INT, TIM_CR1_CMS_EDGE, TIM_CR1_DIR_UP);

	timer_set_prescaler(TIM3, 54);

	timer_disable_preload(TIM3);

	timer_continuous_mode(TIM3);

	timer_set_period(TIM3, 39);

	timer_set_oc_value(TIM3, TIM_OC1, 19);

	timer_set_oc_mode(TIM3, TIM_OC1, TIM_OCM_PWM1);

	timer_set_oc_polarity_high(TIM3, TIM_OC1);

	timer_set_oc_idle_state_set(TIM3, TIM_OC1);
	
	timer_enable_oc_output(TIM3, TIM_OC1);

	timer_enable_counter(TIM3);
}

// reset Bluetooth using n_shutdown
static void bluetooth_power_cycle(void){
	printf("Bluetooth power cycle\n");

	bluetooth_clk_init();

	msleep(1000);
	gpio_clear(BT_N_RST_PORT, BT_N_RST_IO);
	gpio_clear(BT_N_WAKE_PORT, BT_N_WAKE_IO);
	msleep(2000);
	gpio_set(BT_N_RST_PORT, BT_N_RST_IO);
	msleep(1000);

	printf("Bluetooth power cycle end\n");
	__enable_irq();
}


// after HCI Reset, use 115200. Then increase baud reate to 468000.
// (on nucleo board without external crystall, running at 8 Mhz, 1 mbps was not possible)
static const hci_transport_config_uart_t config = {
	HCI_TRANSPORT_CONFIG_UART,
    115200,
    115200,
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
            //hci_set_chipset(btstack_chipset_bcm_instance());
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

static void phase2(int status){

    if (status){
        printf("Download firmware failed\n");
        return;
    }

    printf("Phase 2: Main app\n");

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

    // setup app
    btstack_main();
}

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

	bluetooth_setup();

	hal_led_on();

	hci_dump_open(NULL, HCI_DUMP_STDOUT);

	// start with BTstack init - especially configure HCI Transport
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_embedded_get_instance());

	// setup UART HAL + Run Loop integration
	const btstack_uart_block_t *uart_driver = btstack_uart_block_embedded_instance();

    // extract UART config from transport config, but disable flow control and use default baudrate
    uart_config.baudrate    = config.baudrate_main;
    uart_config.flowcontrol = 1;
    uart_config.device_name = config.device_name;
    uart_driver->init(&uart_config);

    // phase #1 download firmware
    printf("Phase 1: Download firmware\n");

	const btstack_chipset_t *chipset = btstack_chipset_bcm_instance();

	chipset->init(NULL);
    btstack_chipset_bcm_download_firmware(uart_driver, config.baudrate_main, &phase2);

    // go
    btstack_run_loop_execute();

	return 0;
}
