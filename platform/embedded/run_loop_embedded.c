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

/*
 *  run_loop_embedded.c
 *
 *  For this run loop, we assume that there's no global way to wait for a list
 *  of data sources to get ready. Instead, each data source has to queried
 *  individually. Calling ds->isReady() before calling ds->process() doesn't 
 *  make sense, so we just poll each data source round robin.
 *
 *  To support an idle state, where an MCU could go to sleep, the process function
 *  has to return if it has to called again as soon as possible
 *
 *  After calling process() on every data source and evaluating the pending timers,
 *  the idle hook gets called if no data source did indicate that it needs to be
 *  called right away.
 *
 */


#include "run_loop.h"
#include "run_loop_embedded.h"
#include "btstack_linked_list.h"
#include "hal_tick.h"
#include "hal_cpu.h"

#include "run_loop_private.h"
#include "btstack_debug.h"

#include <stddef.h> // NULL

#ifdef HAVE_TIME_MS
#include "hal_time_ms.h"
#endif

#if defined(HAVE_TICK) && defined(HAVE_TIME_MS)
#error "Please specify either HAVE_TICK or HAVE_TIME_MS"
#endif

#if defined(HAVE_TICK) || defined(HAVE_TIME_MS)
#define TIMER_SUPPORT
#endif

static const run_loop_t run_loop_embedded;

// the run loop
static btstack_btstack_linked_list_t data_sources;

#ifdef TIMER_SUPPORT
static btstack_btstack_linked_list_t timers;
#endif

#ifdef HAVE_TICK
static volatile uint32_t system_ticks;
#endif

static int trigger_event_received = 0;

/**
 * Add data_source to run_loop
 */
static void run_loop_embedded_add_data_source(data_source_t *ds){
    btstack_linked_list_add(&data_sources, (btstack_linked_item_t *) ds);
}

/**
 * Remove data_source from run loop
 */
static int run_loop_embedded_remove_data_source(data_source_t *ds){
    return btstack_linked_list_remove(&data_sources, (btstack_linked_item_t *) ds);
}

// set timer
static void run_loop_embedded_set_timer(timer_source_t *ts, uint32_t timeout_in_ms){
#ifdef HAVE_TICK
    uint32_t ticks = run_loop_embedded_ticks_for_ms(timeout_in_ms);
    if (ticks == 0) ticks++;
    // time until next tick is < hal_tick_get_tick_period_in_ms() and we don't know, so we add one
    ts->timeout = system_ticks + 1 + ticks; 
#endif
#ifdef HAVE_TIME_MS
    ts->timeout = hal_time_ms() + timeout_in_ms + 1;
#endif
}

/**
 * Add timer to run_loop (keep list sorted)
 */
static void run_loop_embedded_add_timer(timer_source_t *ts){
#ifdef TIMER_SUPPORT
    btstack_linked_item_t *it;
    for (it = (btstack_linked_item_t *) &timers; it->next ; it = it->next){
        // don't add timer that's already in there
        if ((timer_source_t *) it->next == ts){
            log_error( "run_loop_timer_add error: timer to add already in list!");
            return;
        }
        if (ts->timeout < ((timer_source_t *) it->next)->timeout) {
            break;
        }
    }
    ts->item.next = it->next;
    it->next = (btstack_linked_item_t *) ts;
#endif
}

/**
 * Remove timer from run loop
 */
static int run_loop_embedded_remove_timer(timer_source_t *ts){
#ifdef TIMER_SUPPORT
    return btstack_linked_list_remove(&timers, (btstack_linked_item_t *) ts);
#else
    return 0;
#endif
}

static void run_loop_embedded_dump_timer(void){
#ifdef TIMER_SUPPORT
#ifdef ENABLE_LOG_INFO 
    btstack_linked_item_t *it;
    int i = 0;
    for (it = (btstack_linked_item_t *) timers; it ; it = it->next){
        timer_source_t *ts = (timer_source_t*) it;
        log_info("timer %u, timeout %u\n", i, (unsigned int) ts->timeout);
    }
#endif
#endif
}

/**
 * Execute run_loop once
 */
void run_loop_embedded_execute_once(void) {
    data_source_t *ds;

    // process data sources
    data_source_t *next;
    for (ds = (data_source_t *) data_sources; ds != NULL ; ds = next){
        next = (data_source_t *) ds->item.next; // cache pointer to next data_source to allow data source to remove itself
        ds->process(ds);
    }
    
#ifdef HAVE_TICK
    uint32_t now = system_ticks;
#endif
#ifdef HAVE_TIME_MS
    uint32_t now = hal_time_ms();
#endif
#ifdef TIMER_SUPPORT
    // process timers
    while (timers) {
        timer_source_t *ts = (timer_source_t *) timers;
        if (ts->timeout > now) break;
        run_loop_remove_timer(ts);
        ts->process(ts);
    }
#endif
    
    // disable IRQs and check if run loop iteration has been requested. if not, go to sleep
    hal_cpu_disable_irqs();
    if (trigger_event_received){
        trigger_event_received = 0;
        hal_cpu_enable_irqs();
    } else {
        hal_cpu_enable_irqs_and_sleep();
    }
}

/**
 * Execute run_loop
 */
static void run_loop_embedded_execute(void) {
    while (1) {
        run_loop_embedded_execute_once();
    }
}

#ifdef HAVE_TICK
static void run_loop_embedded_tick_handler(void){
    system_ticks++;
    trigger_event_received = 1;
}

uint32_t run_loop_embedded_get_ticks(void){
    return system_ticks;
}

uint32_t run_loop_embedded_ticks_for_ms(uint32_t time_in_ms){
    return time_in_ms / hal_tick_get_tick_period_in_ms();
}
#endif

static uint32_t run_loop_embedded_get_time_ms(void){
#ifdef HAVE_TIME_MS
    return hal_time_ms();
#endif
#ifdef HAVE_TICK
    return system_ticks * hal_tick_get_tick_period_in_ms();
#endif
    return 0;
}


/**
 * trigger run loop iteration
 */
void run_loop_embedded_trigger(void){
    trigger_event_received = 1;
}

static void run_loop_embedded_init(void){
    data_sources = NULL;

#ifdef TIMER_SUPPORT
    timers = NULL;
#endif

#ifdef HAVE_TICK
    system_ticks = 0;
    hal_tick_init();
    hal_tick_set_handler(&run_loop_embedded_tick_handler);
#endif
}

/**
 * Provide run_loop_embedded instance 
 */
const run_loop_t * run_loop_embedded_get_instance(void){
    return &run_loop_embedded;
}

static const run_loop_t run_loop_embedded = {
    &run_loop_embedded_init,
    &run_loop_embedded_add_data_source,
    &run_loop_embedded_remove_data_source,
    &run_loop_embedded_set_timer,
    &run_loop_embedded_add_timer,
    &run_loop_embedded_remove_timer,
    &run_loop_embedded_execute,
    &run_loop_embedded_dump_timer,
    &run_loop_embedded_get_time_ms,
};