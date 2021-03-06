/*
 * floppy.c
 * 
 * Floppy interface control.
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#define GPI_bus GPI_floating
#define GPO_bus GPO_pushpull(_2MHz,O_FALSE)
#define AFO_bus AFO_pushpull(_2MHz)

#define m(pin) (1u<<(pin))

/* A soft IRQ for handling step pulses. */
static void drive_step_timer(void *_drv);
void IRQ_43(void) __attribute__((alias("IRQ_step")));
#define STEP_IRQ 43

/* A DMA buffer for running a timer associated with a floppy-data I/O pin. */
struct dma_ring {
    /* Current state of DMA (RDATA): 
     *  DMA_inactive: No activity, buffer is empty. 
     *  DMA_starting: Buffer is filling, DMA+timer not yet active.
     *  DMA_active: DMA is active, timer is operational. 
     *  DMA_stopping: DMA+timer halted, buffer waiting to be cleared. 
     * Current state of DMA (WDATA): 
     *  DMA_inactive: No activity, flux ring and MFM buffer are empty. 
     *  DMA_starting: Flux ring and MFM buffer are filling, DMA+timer active.
     *  DMA_active: Writeback processing is active (to mass storage).
     *  DMA_stopping: DMA+timer halted, buffers waiting to be cleared. */
#define DMA_inactive 0 /* -> {starting, active} */
#define DMA_starting 1 /* -> {active, stopping} */
#define DMA_active   2 /* -> {stopping} */
#define DMA_stopping 3 /* -> {inactive} */
    volatile uint8_t state;
    /* IRQ handler sets this if the read buffer runs dry. */
    volatile uint8_t kick_dma_irq;
    /* Indexes into the buf[] ring buffer. */
    uint16_t cons;
    union {
        uint16_t prod; /* dma_rd: our producer index for flux samples */
        uint16_t prev_sample; /* dma_wr: previous CCRx sample value */
    };
    /* DMA ring buffer of timer values (ARR or CCRx). */
    uint16_t buf[1024];
};

/* DMA buffers are permanently allocated while a disk image is loaded, allowing 
 * independent and concurrent management of the RDATA/WDATA pins. */
static struct dma_ring *dma_rd; /* RDATA DMA buffer */
static struct dma_ring *dma_wr; /* WDATA DMA buffer */

/* Statically-allocated floppy drive state. Tracks head movements and 
 * side changes at all times, even when the drive is empty. */
static struct drive {
    struct v2_slot *slot;
    uint8_t cyl, head;
    bool_t sel;
    struct {
        bool_t started; /* set by hi-irq, cleared by lo-irq */
        bool_t active;  /* set by hi-irq, cleared by step.timer */
        bool_t settling; /* set by timer, cleared by timer or hi-irq */
#define STEP_started  1 /* started by hi-pri IRQ */
#define STEP_latched  2 /* latched by lo-pri IRQ */
#define STEP_active   (STEP_started | STEP_latched)
#define STEP_settling 4 /* handled by step.timer */
        uint8_t state;
        bool_t inward;
        stk_time_t start;
        struct timer timer;
    } step;
    struct image *image;
} drive;

static struct image *image;
static stk_time_t sync_time;

static struct {
    struct timer timer;
    bool_t active;
    stk_time_t prev_time;
} index;
static void index_pulse(void *);

static uint32_t max_read_us;

static void rdata_stop(void);
static void wdata_start(void);
static void wdata_stop(void);

static void floppy_change_outputs(uint16_t mask, uint8_t val);

struct exti_irq {
    uint8_t irq, pri;
    uint16_t pr_mask; /* != 0: irq- and exti-pending flags are cleared */
};

#if BUILD_TOUCH
#include "touch/floppy.c"
#elif BUILD_GOTEK
#include "gotek/floppy.c"
#endif

static void floppy_change_outputs(uint16_t mask, uint8_t val)
{
    IRQ_global_disable();
    if (val == O_TRUE)
        gpio_out_active |= mask;
    else
        gpio_out_active &= ~mask;
    if (drive.sel)
        gpio_write_pins(gpio_out, mask, val);
    IRQ_global_enable();
}

void floppy_cancel(void)
{
    /* Initialised? Bail if not. */
    if (!dma_rd)
        return;

    /* Stop DMA/timer work. */
    IRQx_disable(dma_rdata_irq);
    IRQx_disable(dma_wdata_irq);
    timer_cancel(&index.timer);
    rdata_stop();
    wdata_stop();

    /* Clear soft state. */
    drive.image = NULL;
    drive.slot = NULL;
    max_read_us = 0;
    image = NULL;
    dma_rd = dma_wr = NULL;

    /* Set outputs for empty drive. */
    index.active = FALSE;
    floppy_change_outputs(m(pin_index) | m(pin_rdy), O_FALSE);
    floppy_change_outputs(m(pin_dskchg) | m(pin_wrprot), O_TRUE);
}

static struct dma_ring *dma_ring_alloc(void)
{
    struct dma_ring *dma = arena_alloc(sizeof(*dma));
    memset(dma, 0, offsetof(struct dma_ring, buf));
    return dma;
}

void floppy_init(void)
{
    const struct exti_irq *e;
    unsigned int i;

    board_floppy_init();

    timer_init(&drive.step.timer, drive_step_timer, &drive);

    gpio_configure_pin(gpio_out, pin_dskchg, GPO_bus);
    gpio_configure_pin(gpio_out, pin_index,  GPO_bus);
    gpio_configure_pin(gpio_out, pin_trk0,   GPO_bus);
    gpio_configure_pin(gpio_out, pin_wrprot, GPO_bus);
    gpio_configure_pin(gpio_out, pin_rdy,    GPO_bus);

    gpio_configure_pin(gpio_data, pin_wdata, GPI_bus);
    gpio_configure_pin(gpio_data, pin_rdata, GPO_bus);

    floppy_change_outputs(m(pin_dskchg) | m(pin_wrprot) | m(pin_trk0), O_TRUE);

    /* Configure physical interface interrupts. */
    for (i = 0, e = exti_irqs; i < ARRAY_SIZE(exti_irqs); i++, e++) {
        IRQx_set_prio(e->irq, e->pri);
        if (e->pr_mask != 0) {
            /* Do not trigger an initial interrupt on this line. Clear EXTI_PR
             * before IRQ-pending, otherwise IRQ-pending is immediately
             * reasserted. */
            exti->pr = e->pr_mask;
            IRQx_clear_pending(e->irq);
        } else {
            /* Common case: we deliberately trigger the first interrupt to 
             * prime the ISR's state. */
            IRQx_set_pending(e->irq);
        }
    }

    /* Enable physical interface interrupts. */
    for (i = 0, e = exti_irqs; i < ARRAY_SIZE(exti_irqs); i++, e++) {
        IRQx_enable(e->irq);
    }

    IRQx_set_prio(STEP_IRQ, FLOPPY_IRQ_LO_PRI);
    IRQx_enable(STEP_IRQ);

    timer_init(&index.timer, index_pulse, NULL);
}

void floppy_insert(unsigned int unit, struct v2_slot *slot)
{
    arena_init();

    dma_rd = dma_ring_alloc();
    dma_wr = dma_ring_alloc();

    image = arena_alloc(sizeof(*image));
    memset(image, 0, sizeof(*image));

    /* Large buffer to absorb long write latencies at mass-storage layer. */
    image->bufs.write_mfm.len = 20*1024;
    image->bufs.write_mfm.p = arena_alloc(image->bufs.write_mfm.len);

    /* Any remaining space is used for staging writes to mass storage, for 
     * example when format conversion is required and it is not possible to 
     * do this in place within the write_mfm buffer. */
    image->bufs.write_data.len = arena_avail();
    image->bufs.write_data.p = arena_alloc(image->bufs.write_data.len);

    /* Read MFM buffer overlaps the second half of the write MFM buffer.
     * This is because:
     *  (a) The read MFM buffer does not need to absorb such large latencies
     *      (reads are much more predictable than writes to mass storage).
     *  (b) By dedicating the first half of the write buffer to writes, we
     *      can safely start processing write flux while read-data is still
     *      processing (eg. in-flight mass storage io). At say 10kB of
     *      dedicated write buffer, this is good for >80ms before colliding
     *      with read buffers, even at HD data rate (1us/bitcell).
     *      This is more than enough time for read
     *      processing to complete. */
    image->bufs.read_mfm.len = image->bufs.write_mfm.len / 2;
    image->bufs.read_mfm.p = (char *)image->bufs.write_mfm.p
        + image->bufs.read_mfm.len;

    /* Read-data buffer can entirely share the space of the write-data buffer. 
     * Change of use of this memory space is fully serialised. */
    image->bufs.read_data = image->bufs.write_data;

    drive.slot = slot;

    index.prev_time = stk_now();
    timer_set(&index.timer, stk_add(index.prev_time, stk_ms(200)));

    /* Enable DMA interrupts. */
    dma1->ifcr = DMA_IFCR_CGIF(dma_rdata_ch) | DMA_IFCR_CGIF(dma_wdata_ch);
    IRQx_set_prio(dma_rdata_irq, RDATA_IRQ_PRI);
    IRQx_set_prio(dma_wdata_irq, WDATA_IRQ_PRI);
    IRQx_enable(dma_rdata_irq);
    IRQx_enable(dma_wdata_irq);

    /* RDATA Timer setup:
     * The counter is incremented at full SYSCLK rate. 
     *  
     * Ch.2 (RDATA) is in PWM mode 1. It outputs O_TRUE for 400ns and then 
     * O_FALSE until the counter reloads. By changing the ARR via DMA we alter
     * the time between (fixed-width) O_TRUE pulses, mimicking floppy drive 
     * timings. */
    tim_rdata->psc = 0;
    tim_rdata->ccmr1 = (TIM_CCMR1_CC2S(TIM_CCS_OUTPUT) |
                        TIM_CCMR1_OC2M(TIM_OCM_PWM1));
    tim_rdata->ccer = TIM_CCER_CC2E | ((O_TRUE==0) ? TIM_CCER_CC2P : 0);
    tim_rdata->ccr2 = sysclk_ns(400);
    tim_rdata->dier = TIM_DIER_UDE;
    tim_rdata->cr2 = 0;

    /* DMA setup: From a circular buffer into the RDATA Timer's ARR. */
    dma_rdata.cpar = (uint32_t)(unsigned long)&tim_rdata->arr;
    dma_rdata.cmar = (uint32_t)(unsigned long)dma_rd->buf;
    dma_rdata.cndtr = ARRAY_SIZE(dma_rd->buf);

    /* WDATA Timer setup: 
     * The counter runs from 0x0000-0xFFFF inclusive at full SYSCLK rate.
     *  
     * Ch.1 (WDATA) is in Input Capture mode, sampling on every clock and with
     * no input prescaling or filtering. Samples are captured on the falling 
     * edge of the input (CCxP=1). DMA is used to copy the sample into a ring
     * buffer for batch processing in the DMA-completion ISR. */
    tim_wdata->psc = 0;
    tim_wdata->arr = 0xffff;
    tim_wdata->ccmr1 = TIM_CCMR1_CC1S(TIM_CCS_INPUT_TI1);
    tim_wdata->dier = TIM_DIER_CC1DE;
    tim_wdata->cr2 = 0;

    /* DMA setup: From the WDATA Timer's CCRx into a circular buffer. */
    dma_wdata.cpar = (uint32_t)(unsigned long)&tim_wdata->ccr1;
    dma_wdata.cmar = (uint32_t)(unsigned long)dma_wr->buf;

    /* Drive is 'ready'. */
    floppy_change_outputs(m(pin_rdy), O_TRUE);
}

/* Called from IRQ context to stop the write stream. */
static void wdata_stop(void)
{
    uint8_t prev_state = dma_wr->state;

    /* Already inactive? Nothing to do. */
    if ((prev_state == DMA_inactive) || (prev_state == DMA_stopping))
        return;

    /* Ok we're now stopping DMA activity. */
    dma_wr->state = DMA_stopping;

    /* Turn off timer and DMA. */
    tim_wdata->ccer = 0;
    tim_wdata->cr1 = 0;
    dma_wdata.ccr = 0;

    /* Drain out the DMA buffer. */
    IRQx_set_pending(dma_wdata_irq);
}

static void wdata_start(void)
{
    uint32_t start_pos;

    if (dma_wr->state != DMA_inactive) {
        printk("*** Missed write\n");
        return;
    }
    dma_wr->state = DMA_starting;

    /* Start DMA to circular buffer. */
    dma_wdata.cndtr = ARRAY_SIZE(dma_wr->buf);
    dma_wdata.ccr = (DMA_CCR_PL_HIGH |
                     DMA_CCR_MSIZE_16BIT |
                     DMA_CCR_PSIZE_16BIT |
                     DMA_CCR_MINC |
                     DMA_CCR_CIRC |
                     DMA_CCR_DIR_P2M |
                     DMA_CCR_HTIE |
                     DMA_CCR_TCIE |
                     DMA_CCR_EN);

    /* Start timer. */
    tim_wdata->ccer = TIM_CCER_CC1E | TIM_CCER_CC1P;
    tim_wdata->egr = TIM_EGR_UG;
    tim_wdata->sr = 0; /* dummy write, gives h/w time to process EGR.UG=1 */
    tim_wdata->cr1 = TIM_CR1_CEN;

    /* Find rotational start position of the write, in systicks since index. */
    start_pos = max_t(int32_t, 0, stk_delta(index.prev_time, stk_now()));
    start_pos %= stk_ms(DRIVE_MS_PER_REV);
    start_pos *= SYSCLK_MHZ / STK_MHZ;
    image->write_start = start_pos;
    printk("Write start %u us\n", start_pos / SYSCLK_MHZ);
    delay_us(100); /* XXX X-Copy workaround -- fix me properly!!!! */
}

/* Called from IRQ context to stop the read stream. */
static void rdata_stop(void)
{
    uint8_t prev_state = dma_rd->state;

    /* Already inactive? Nothing to do. */
    if (prev_state == DMA_inactive)
        return;

    /* Ok we're now stopping DMA activity. */
    dma_rd->state = DMA_stopping;

    /* If DMA was not yet active, don't need to touch peripherals. */
    if (prev_state != DMA_active)
        return;

    /* Turn off the output pin */
    gpio_configure_pin(gpio_data, pin_rdata, GPO_bus);

    /* Turn off timer and DMA. */
    tim_rdata->cr1 = 0;
    dma_rdata.ccr = 0;
    dma_rdata.cndtr = ARRAY_SIZE(dma_rd->buf);
}

/* Called from user context to start the read stream. */
static void rdata_start(void)
{
    IRQ_global_disable();

    /* Did we race rdata_stop()? Then bail. */
    if (dma_rd->state == DMA_stopping)
        goto out;

    dma_rd->state = DMA_active;

    /* Start DMA from circular buffer. */
    dma_rdata.ccr = (DMA_CCR_PL_HIGH |
                     DMA_CCR_MSIZE_16BIT |
                     DMA_CCR_PSIZE_16BIT |
                     DMA_CCR_MINC |
                     DMA_CCR_CIRC |
                     DMA_CCR_DIR_M2P |
                     DMA_CCR_HTIE |
                     DMA_CCR_TCIE |
                     DMA_CCR_EN);

    /* Start timer. */
    tim_rdata->egr = TIM_EGR_UG;
    tim_rdata->sr = 0; /* dummy write, gives h/w time to process EGR.UG=1 */
    tim_rdata->cr1 = TIM_CR1_CEN;

    /* Enable output. */
    if (drive.sel)
        gpio_configure_pin(gpio_data, pin_rdata, AFO_bus);

out:
    IRQ_global_enable();
}

static void floppy_sync_flux(void)
{
    struct drive *drv = &drive;
    int32_t ticks;
    uint32_t nr;

    nr = ARRAY_SIZE(dma_rd->buf) - dma_rd->prod - 1;
    if (nr)
        dma_rd->prod += image_rdata_flux(
            drv->image, &dma_rd->buf[dma_rd->prod], nr);

    if (dma_rd->prod < ARRAY_SIZE(dma_rd->buf)/2)
        return;

    ticks = stk_delta(stk_now(), sync_time) - stk_us(1);
    if (ticks > stk_ms(5)) /* ages to wait; go do other work */
        return;

    if (ticks > 0)
        delay_ticks(ticks);
    ticks = stk_delta(stk_now(), sync_time); /* XXX */
    rdata_start();
    printk("Trk %u: sync_ticks=%d\n", drv->image->cur_track, ticks);
}

static void floppy_read_data(struct drive *drv)
{
    uint32_t read_us;
    stk_time_t timestamp;

    /* Read some track data if there is buffer space. */
    timestamp = stk_now();
    if (image_read_track(drv->image) && dma_rd->kick_dma_irq) {
        /* We buffered some more data and the DMA handler requested a kick. */
        dma_rd->kick_dma_irq = FALSE;
        IRQx_set_pending(dma_rdata_irq);
    }

    /* Log maximum time taken to read track data, in microseconds. */
    read_us = stk_diff(timestamp, stk_now()) / STK_MHZ;
    if (read_us > max_read_us) {
        max_read_us = max_t(uint32_t, max_read_us, read_us);
        printk("New max: read_us=%u\n", max_read_us);
    }
}

static bool_t dma_rd_handle(struct drive *drv)
{
    switch (dma_rd->state) {

    case DMA_inactive: {
        stk_time_t index_time, read_start_pos;
        unsigned int track;
        /* Allow 10ms from current rotational position to load new track */
        int32_t delay = stk_ms(10);
        /* Allow extra time if heads are settling. */
        if (drv->step.state & STEP_settling) {
            stk_time_t step_settle = stk_add(drv->step.start,
                                             stk_ms(DRIVE_SETTLE_MS));
            int32_t delta = stk_delta(stk_now(), step_settle);
            delay = max_t(int32_t, delta, delay);
        }
        /* No data fetch while stepping. */
        barrier(); /* check STEP_settling /then/ check STEP_active */
        if (drv->step.state & STEP_active)
            break;
        /* Work out where in new track to start reading data from. */
        index_time = index.prev_time;
        read_start_pos = stk_timesince(index_time) + delay;
        if (read_start_pos > stk_ms(DRIVE_MS_PER_REV))
            read_start_pos -= stk_ms(DRIVE_MS_PER_REV);
        /* Seek to the new track. */
        track = drv->cyl*2 + drv->head;
        read_start_pos *= SYSCLK_MHZ/STK_MHZ;
        if (image_seek_track(drv->image, track, &read_start_pos))
            return TRUE;
        read_start_pos /= SYSCLK_MHZ/STK_MHZ;
        /* Set the deadline. */
        sync_time = stk_add(index_time, read_start_pos);
        if (stk_delta(stk_now(), sync_time) < 0)
            sync_time = stk_add(sync_time, stk_ms(DRIVE_MS_PER_REV));
        /* Change state /then/ check for race against step or side change. */
        dma_rd->state = DMA_starting;
        barrier();
        if ((drv->step.state & STEP_active)
            || (track != (drv->cyl*2 + drv->head))
            || (dma_wr->state != DMA_inactive))
            dma_rd->state = DMA_stopping;
        break;
    }

    case DMA_starting:
        floppy_read_data(drv);
        floppy_sync_flux();
        break;

    case DMA_active:
        floppy_read_data(drv);
        break;

    case DMA_stopping:
        dma_rd->state = DMA_inactive;
        /* Reinitialise the circular buffer to empty. */
        dma_rd->cons = dma_rd->prod = 0;
        /* Free-running index timer. */
        if (!index.active)
            timer_set(&index.timer, stk_add(index.prev_time, stk_ms(200)));
        break;
    }

    return FALSE;
}

void floppy_get_track(uint8_t *p_cyl, uint8_t *p_side)
{
    *p_cyl = drive.cyl;
    *p_side = drive.head;
}

bool_t floppy_handle(void)
{
    struct drive *drv = &drive;

    if (!drv->image) {
        if (!image_open(image, drv->slot))
            return TRUE;
        drv->image = image;
        dma_rd->state = DMA_stopping;
        if (image->handler->write_track)
            floppy_change_outputs(m(pin_wrprot), O_FALSE);
    }

    switch (dma_wr->state) {

    case DMA_inactive:
        if (dma_rd_handle(drv))
            return TRUE;
        break;

    case DMA_starting: {
        unsigned int track;
        /* Bail out of read mode. */
        if (dma_rd->state != DMA_inactive) {
            ASSERT(dma_rd->state == DMA_stopping);
            if (dma_rd_handle(drv))
                return TRUE;
            ASSERT(dma_rd->state == DMA_inactive);
        }
        /* Make sure we're on the correct track. */
        track = drv->cyl*2 + drv->head;
        if (image_seek_track(drv->image, track, NULL))
            return TRUE;
        /* May race wdata_stop(). */
        cmpxchg(&dma_wr->state, DMA_starting, DMA_active);
        break;
    }

    case DMA_active:
        image_write_track(drv->image, FALSE);
        break;

    case DMA_stopping: {
        /* Wait for the flux ring to drain out into the MFM buffer. 
         * Write data to mass storage meanwhile. */
        uint16_t prod = ARRAY_SIZE(dma_wr->buf) - dma_wdata.cndtr;
        uint16_t cons = dma_wr->cons;
        barrier(); /* take dma indexes /then/ process data tail */
        image_write_track(drv->image, cons == prod);
        if (cons != prod)
            break;
        /* Clear the flux ring, flush dirty buffers. */
        dma_wr->cons = 0;
        dma_wr->prev_sample = 0;
        image->bufs.write_mfm.cons = image->bufs.write_data.cons = 0;
        image->bufs.write_mfm.prod = image->bufs.write_data.prod = 0;
        F_sync(&drv->image->fp);
        barrier(); /* allow reactivation of write path /last/ */
        dma_wr->state = DMA_inactive;
        break;
    }
    }

    return FALSE;
}

static void index_pulse(void *dat)
{
    index.active ^= 1;
    if (index.active) {
        index.prev_time = index.timer.deadline;
        floppy_change_outputs(m(pin_index), O_TRUE);
        timer_set(&index.timer, stk_add(index.prev_time, stk_ms(2)));
    } else {
        floppy_change_outputs(m(pin_index), O_FALSE);
        if (dma_rd->state != DMA_active) /* timer set from input flux stream */
            timer_set(&index.timer, stk_add(index.prev_time, stk_ms(200)));
    }
}

static void drive_step_timer(void *_drv)
{
    struct drive *drv = _drv;

    switch (drv->step.state) {
    case STEP_started:
        /* nothing to do, IRQ_step() needs to reset our deadline */
        break;
    case STEP_latched:
        speaker_pulse();
        if ((drv->cyl >= 84) && !drv->step.inward)
            drv->cyl = 84; /* Fast step back from D-A cyl 255 */
        drv->cyl += drv->step.inward ? 1 : -1;
        timer_set(&drv->step.timer, stk_add(drv->step.start, DRIVE_SETTLE_MS));
        if (drv->cyl == 0)
            floppy_change_outputs(m(pin_trk0), O_TRUE);
        /* New state last, as that lets hi-pri IRQ start another step. */
        barrier();
        drv->step.state = STEP_settling;
        break;
    case STEP_settling:
        /* Can race transition to STEP_started. */
        cmpxchg(&drv->step.state, STEP_settling, 0);
        break;
    }
}

static void IRQ_step(void)
{
    struct drive *drv = &drive;

    if (drv->step.state == STEP_started) {
        timer_cancel(&drv->step.timer);
        drv->step.state = STEP_latched;
        timer_set(&drv->step.timer, stk_add(drv->step.start, stk_ms(2)));
    }
}

static void IRQ_rdata_dma(void)
{
    const uint16_t buf_mask = ARRAY_SIZE(dma_rd->buf) - 1;
    uint32_t prev_ticks_since_index, ticks, i;
    uint16_t nr_to_wrap, nr_to_cons, nr, dmacons, done;
    stk_time_t now;
    struct drive *drv = &drive;

    /* Clear DMA peripheral interrupts. */
    dma1->ifcr = DMA_IFCR_CGIF(dma_rdata_ch);

    /* If we happen to be called in the wrong state, just bail. */
    if (dma_rd->state != DMA_active)
        return;

    /* Find out where the DMA engine's consumer index has got to. */
    dmacons = ARRAY_SIZE(dma_rd->buf) - dma_rdata.cndtr;

    /* Check for DMA catching up with the producer index (underrun). */
    if (((dmacons < dma_rd->cons)
         ? (dma_rd->prod >= dma_rd->cons) || (dma_rd->prod < dmacons)
         : (dma_rd->prod >= dma_rd->cons) && (dma_rd->prod < dmacons))
        && (dmacons != dma_rd->cons))
        printk("RDATA underrun! %x-%x-%x\n",
               dma_rd->cons, dma_rd->prod, dmacons);

    dma_rd->cons = dmacons;

    /* Find largest contiguous stretch of ring buffer we can fill. */
    nr_to_wrap = ARRAY_SIZE(dma_rd->buf) - dma_rd->prod;
    nr_to_cons = (dmacons - dma_rd->prod - 1) & buf_mask;
    nr = min(nr_to_wrap, nr_to_cons);
    if (nr == 0) /* Buffer already full? Then bail. */
        return;

    /* Now attempt to fill the contiguous stretch with flux data calculated 
     * from buffered image data. */
    prev_ticks_since_index = image_ticks_since_index(drv->image);
    dma_rd->prod += done = image_rdata_flux(
        drv->image, &dma_rd->buf[dma_rd->prod], nr);
    dma_rd->prod &= buf_mask;
    if (done != nr) {
        /* Read buffer ran dry: kick us when more data is available. */
        dma_rd->kick_dma_irq = TRUE;
    } else if (nr != nr_to_cons) {
        /* We didn't fill the ring: re-enter this ISR to do more work. */
        IRQx_set_pending(dma_rdata_irq);
    }

    /* Check if we have crossed the index mark. If not, we're done. */
    if (image_ticks_since_index(drv->image) >= prev_ticks_since_index)
        return;

    /* We crossed the index mark: Synchronise index pulse to the bitstream. */
    for (;;) {
        /* Snapshot current position in flux stream, including progress through
         * current timer sample. */
        now = stk_now();
        /* Ticks left in current sample. */
        ticks = tim_rdata->arr - tim_rdata->cnt;
        /* Index of next sample. */
        dmacons = ARRAY_SIZE(dma_rd->buf) - dma_rdata.cndtr;
        /* If another sample was loaded meanwhile, try again for a consistent
         * snapshot. */
        if (dmacons == dma_rd->cons)
            break;
        dma_rd->cons = dmacons;
    }
    /* Sum all flux timings in the DMA buffer. */
    for (i = dmacons; i != dma_rd->prod; i = (i+1) & buf_mask)
        ticks += dma_rd->buf[i] + 1;
    /* Subtract current flux offset beyond the index. */
    ticks -= image_ticks_since_index(drv->image);
    /* Calculate deadline for index timer. */
    ticks /= SYSCLK_MHZ/STK_MHZ;
    timer_set(&index.timer, stk_add(now, ticks));
}

static void IRQ_wdata_dma(void)
{
    const uint16_t buf_mask = ARRAY_SIZE(dma_rd->buf) - 1;
    uint16_t cons, prod, prev, curr, next;
    uint32_t mfm = 0, mfmprod, syncword = image->handler->syncword;
    uint32_t *mfmbuf = image->bufs.write_mfm.p;
    unsigned int mfmbuflen = image->bufs.write_mfm.len / 4;

    /* Clear DMA peripheral interrupts. */
    dma1->ifcr = DMA_IFCR_CGIF(dma_wdata_ch);

    /* If we happen to be called in the wrong state, just bail. */
    if (dma_wr->state == DMA_inactive)
        return;

    /* Find out where the DMA engine's producer index has got to. */
    prod = ARRAY_SIZE(dma_wr->buf) - dma_wdata.cndtr;

    /* Process the flux timings into the MFM raw buffer. */
    prev = dma_wr->prev_sample;
    mfmprod = image->bufs.write_mfm.prod;
    if (mfmprod & 31)
        mfm = be32toh(mfmbuf[(mfmprod / 32) % mfmbuflen]) >> (-mfmprod&31);
    for (cons = dma_wr->cons; cons != prod; cons = (cons+1) & buf_mask) {
        next = dma_wr->buf[cons];
        curr = next - prev;
        prev = next;
        while (curr > 3*SYSCLK_MHZ) {
            curr -= 2*SYSCLK_MHZ;
            mfm <<= 1;
            mfmprod++;
            if (!(mfmprod&31))
                mfmbuf[((mfmprod-1) / 32) % mfmbuflen] = htobe32(mfm);
        }
        mfm = (mfm << 1) | 1;
        mfmprod++;
        if (mfm == syncword)
            mfmprod &= ~31;
        if (!(mfmprod&31))
            mfmbuf[((mfmprod-1) / 32) % mfmbuflen] = htobe32(mfm);
    }

    /* Save our progress for next time. */
    if (mfmprod & 31)
        mfmbuf[(mfmprod / 32) % mfmbuflen] = htobe32(mfm << (-mfmprod&31));
    image->bufs.write_mfm.prod = mfmprod;
    dma_wr->cons = cons;
    dma_wr->prev_sample = prev;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "Linux"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
