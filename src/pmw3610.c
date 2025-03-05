/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT pixart_pmw3610

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/input/input.h>
#include <zmk/keymap.h>
#include "pmw3610.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(pmw3610, CONFIG_PMW3610_LOG_LEVEL);

//////// Sensor initialization steps definition //////////
// init is done in non-blocking manner (i.e., async), a //
// delayable work is defined for this purpose           //
enum pmw3610_init_step {
    ASYNC_INIT_STEP_POWER_UP,  // reset cs line and assert power-up reset
    ASYNC_INIT_STEP_CLEAR_OB1, // clear observation1 register for self-test check
    ASYNC_INIT_STEP_CHECK_OB1, // check the value of observation1 register after self-test check
    ASYNC_INIT_STEP_CONFIGURE, // set other registes like cpi and donwshift time (run, rest1, rest2)
                               // and clear motion registers

    ASYNC_INIT_STEP_COUNT // end flag
};

/* Timings (in ms) needed in between steps to allow each step finishes succussfully. */
// - Since MCU is not involved in the sensor init process, i is allowed to do other tasks.
//   Thus, k_sleep or delayed schedule can be used.
static const int32_t async_init_delay[ASYNC_INIT_STEP_COUNT] = {
    [ASYNC_INIT_STEP_POWER_UP] = 10 + CONFIG_PMW3610_INIT_POWER_UP_EXTRA_DELAY_MS, // >10ms needed
    [ASYNC_INIT_STEP_CLEAR_OB1] = 200, // 150 us required, test shows too short,
                                       // also power-up reset is added in this step, thus using 50 ms
    [ASYNC_INIT_STEP_CHECK_OB1] = 50,  // 10 ms required in spec,
                                       // test shows too short,
                                       // especially when integrated with display,
                                       // > 50ms is needed
    [ASYNC_INIT_STEP_CONFIGURE] = 0,
};

static int pmw3610_async_init_power_up(const struct device *dev);
static int pmw3610_async_init_clear_ob1(const struct device *dev);
static int pmw3610_async_init_check_ob1(const struct device *dev);
static int pmw3610_async_init_configure(const struct device *dev);

static int (*const async_init_fn[ASYNC_INIT_STEP_COUNT])(const struct device *dev) = {
    [ASYNC_INIT_STEP_POWER_UP] = pmw3610_async_init_power_up,
    [ASYNC_INIT_STEP_CLEAR_OB1] = pmw3610_async_init_clear_ob1,
    [ASYNC_INIT_STEP_CHECK_OB1] = pmw3610_async_init_check_ob1,
    [ASYNC_INIT_STEP_CONFIGURE] = pmw3610_async_init_configure,
};

//////// Function definitions //////////

static int pmw3610_read(const struct device *dev, uint8_t addr, uint8_t *value, uint8_t len) {
	const struct pixart_config *cfg = dev->config;
	const struct spi_buf tx_buf = { .buf = &addr, .len = sizeof(addr) };
	const struct spi_buf_set tx = { .buffers = &tx_buf, .count = 1 };
	struct spi_buf rx_buf[] = {
		{ .buf = NULL, .len = sizeof(addr), },
		{ .buf = value, .len = len, },
	};
	const struct spi_buf_set rx = { .buffers = rx_buf, .count = ARRAY_SIZE(rx_buf) };
	return spi_transceive_dt(&cfg->spi, &tx, &rx);
}

static int pmw3610_read_reg(const struct device *dev, uint8_t addr, uint8_t *value) {
	return pmw3610_read(dev, addr, value, 1);
}

static int pmw3610_write_reg(const struct device *dev, uint8_t addr, uint8_t value) {
	const struct pixart_config *cfg = dev->config;
	uint8_t write_buf[] = {addr | SPI_WRITE_BIT, value};
	const struct spi_buf tx_buf = { .buf = write_buf, .len = sizeof(write_buf), };
	const struct spi_buf_set tx = { .buffers = &tx_buf, .count = 1, };
	return spi_write_dt(&cfg->spi, &tx);
}

static int pmw3610_write(const struct device *dev, uint8_t reg, uint8_t val) {
	pmw3610_write_reg(dev, PMW3610_REG_SPI_CLK_ON_REQ, PMW3610_SPI_CLOCK_CMD_ENABLE);
	k_sleep(K_USEC(T_CLOCK_ON_DELAY_US));

    int err = pmw3610_write_reg(dev, reg, val);
    if (unlikely(err != 0)) {
        return err;
    }
    
    pmw3610_write_reg(dev, PMW3610_REG_SPI_CLK_ON_REQ, PMW3610_SPI_CLOCK_CMD_DISABLE);
    return 0;
}

static int set_cpi(const struct device *dev, uint32_t cpi) {
    /* Set resolution with CPI step of 200 cpi
     * 0x1: 200 cpi (minimum cpi)
     * 0x2: 400 cpi
     * 0x3: 600 cpi
     * :
     */

    if ((cpi > PMW3610_MAX_CPI) || (cpi < PMW3610_MIN_CPI)) {
        LOG_ERR("CPI value %u out of range", cpi);
        return -EINVAL;
    }

    // Convert CPI to register value
    uint8_t value = (cpi / 200);
    LOG_INF("Setting CPI to %u (reg value 0x%x)", cpi, value);

    /* set the cpi */
    uint8_t addr[] = {0x7F, PMW3610_REG_RES_STEP, 0x7F};
    uint8_t data[] = {0xFF, value, 0x00};

	pmw3610_write_reg(dev, PMW3610_REG_SPI_CLK_ON_REQ, PMW3610_SPI_CLOCK_CMD_ENABLE);
	k_sleep(K_USEC(T_CLOCK_ON_DELAY_US));

    /* Write data */
    int err;
    for (size_t i = 0; i < sizeof(data); i++) {
        err = pmw3610_write_reg(dev, addr[i], data[i]);
        if (err) {
            LOG_ERR("Burst write failed on SPI write (data)");
            break;
        }
    }
    pmw3610_write_reg(dev, PMW3610_REG_SPI_CLK_ON_REQ, PMW3610_SPI_CLOCK_CMD_DISABLE);

    if (err) {
        LOG_ERR("Failed to set CPI");
        return err;
    }

    return 0;
}

/* Set sampling rate in each mode (in ms) */
static int set_sample_time(const struct device *dev, uint8_t reg_addr, uint32_t sample_time) {
    uint32_t maxtime = 2550;
    uint32_t mintime = 10;
    if ((sample_time > maxtime) || (sample_time < mintime)) {
        LOG_WRN("Sample time %u out of range [%u, %u]", sample_time, mintime, maxtime);
        return -EINVAL;
    }

    uint8_t value = sample_time / mintime;
    LOG_INF("Set sample time to %u ms (reg value: 0x%x)", sample_time, value);

    /* The sample time is (reg_value * mintime ) ms. 0x00 is rounded to 0x1 */
    int err = pmw3610_write(dev, reg_addr, value);
    if (err) {
        LOG_ERR("Failed to change sample time");
    }

    return err;
}

/* Set downshift time in ms. */
// NOTE: The unit of run-mode downshift is related to pos mode rate, which is hard coded to be 4 ms
// The pos-mode rate is configured in pmw3610_async_init_configure
static int set_downshift_time(const struct device *dev, uint8_t reg_addr, uint32_t time) {
    uint32_t maxtime;
    uint32_t mintime;

    switch (reg_addr) {
    case PMW3610_REG_RUN_DOWNSHIFT:
        /*
         * Run downshift time = PMW3610_REG_RUN_DOWNSHIFT
         *                      * 8 * pos-rate (fixed to 4ms)
         */
        maxtime = 8160; // 32 * 255;
        mintime = 32; // hard-coded in pmw3610_async_init_configure
        break;

    case PMW3610_REG_REST1_DOWNSHIFT:
        /*
         * Rest1 downshift time = PMW3610_REG_RUN_DOWNSHIFT
         *                        * 16 * Rest1_sample_period (default 40 ms)
         */
        maxtime = 255 * 16 * CONFIG_PMW3610_REST1_SAMPLE_TIME_MS;
        mintime = 16 * CONFIG_PMW3610_REST1_SAMPLE_TIME_MS;
        break;

    case PMW3610_REG_REST2_DOWNSHIFT:
        /*
         * Rest2 downshift time = PMW3610_REG_REST2_DOWNSHIFT
         *                        * 128 * Rest2 rate (default 100 ms)
         */
        maxtime = 255 * 128 * CONFIG_PMW3610_REST2_SAMPLE_TIME_MS;
        mintime = 128 * CONFIG_PMW3610_REST2_SAMPLE_TIME_MS;
        break;

    default:
        LOG_ERR("Not supported");
        return -ENOTSUP;
    }

    if ((time > maxtime) || (time < mintime)) {
        LOG_WRN("Downshift time %u out of range (%u - %u)", time, mintime, maxtime);
        return -EINVAL;
    }

    __ASSERT_NO_MSG((mintime > 0) && (maxtime / mintime <= UINT8_MAX));

    /* Convert time to register value */
    uint8_t value = time / mintime;

    LOG_INF("Set downshift time to %u ms (reg value 0x%x)", time, value);

    int err = pmw3610_write(dev, reg_addr, value);
    if (err) {
        LOG_ERR("Failed to change downshift time");
    }

    return err;
}

static void set_interrupt(const struct device *dev, const bool en) {
    const struct pixart_config *config = dev->config;
    int ret = gpio_pin_interrupt_configure_dt(&config->irq_gpio,
                                              en ? GPIO_INT_LEVEL_ACTIVE : GPIO_INT_DISABLE);
    if (ret < 0) {
        LOG_ERR("can't set interrupt");
    }
}

static int pmw3610_async_init_power_up(const struct device *dev) {
	int ret = pmw3610_write_reg(dev, PMW3610_REG_POWER_UP_RESET, PMW3610_POWERUP_CMD_RESET);
    if (ret < 0) {
        return ret;
    }
    return 0;
}

static int pmw3610_async_init_clear_ob1(const struct device *dev) {
    return pmw3610_write(dev, PMW3610_REG_OBSERVATION, 0x00);
}

static int pmw3610_async_init_check_ob1(const struct device *dev) {
    uint8_t value;
    int err = pmw3610_read_reg(dev, PMW3610_REG_OBSERVATION, &value);
    if (err) {
        LOG_ERR("Can't do self-test");
        return err;
    }

    if ((value & 0x0F) != 0x0F) {
        LOG_ERR("Failed self-test (0x%x)", value);
        return -EINVAL;
    }

    uint8_t product_id = 0x01;
    err = pmw3610_read_reg(dev, PMW3610_REG_PRODUCT_ID, &product_id);
    if (err) {
        LOG_ERR("Cannot obtain product id");
        return err;
    }

    if (product_id != PMW3610_PRODUCT_ID) {
        LOG_ERR("Incorrect product id 0x%x (expecting 0x%x)!", product_id, PMW3610_PRODUCT_ID);
        return -EIO;
    }

    return 0;
}

static int pmw3610_async_init_configure(const struct device *dev) {
    int err = 0;
    const struct pixart_config *config = dev->config;

    // clear motion registers first (required in datasheet)
    for (uint8_t reg = 0x02; (reg <= 0x05) && !err; reg++) {
        uint8_t buf[1];
        err = pmw3610_read_reg(dev, reg, buf);
    }

    if (!err) {
        err = set_cpi(dev, config->cpi);
    }

    // if (!err) {
    //     uint8_t perf = 0x00;
    //     if (config->pull_rate_250) {
    //         perf |= 0x0D;
    //     }
    //     if (config->force_awake) {
    //         perf |= 0xF0;
    //     }
    //     err = pmw3610_write(dev, PMW3610_REG_PERFORMANCE, perf);
    //     LOG_INF("Set performance register (reg value 0x%x)", perf);
    // }

    if (!err) {
        err = set_downshift_time(dev, PMW3610_REG_RUN_DOWNSHIFT,
                                 CONFIG_PMW3610_RUN_DOWNSHIFT_TIME_MS);
    }

    if (!err) {
        err = set_downshift_time(dev, PMW3610_REG_REST1_DOWNSHIFT,
                                 CONFIG_PMW3610_REST1_DOWNSHIFT_TIME_MS);
    }

    if (!err) {
        err = set_downshift_time(dev, PMW3610_REG_REST2_DOWNSHIFT,
                                 CONFIG_PMW3610_REST2_DOWNSHIFT_TIME_MS);
    }

    if (!err) {
        err = set_sample_time(dev, PMW3610_REG_REST1_RATE,
                              CONFIG_PMW3610_REST1_SAMPLE_TIME_MS);
    }

    if (!err) {
        err = set_sample_time(dev, PMW3610_REG_REST2_RATE,
                              CONFIG_PMW3610_REST2_SAMPLE_TIME_MS);
    }

    if (!err) {
        err = set_sample_time(dev, PMW3610_REG_REST3_RATE,
                              CONFIG_PMW3610_REST3_SAMPLE_TIME_MS);
    }

    if (err) {
        LOG_ERR("Config the sensor failed");
        return err;
    }

    return 0;
}

static void pmw3610_async_init(struct k_work *work) {
    struct k_work_delayable *work2 = (struct k_work_delayable *)work;
    struct pixart_data *data = CONTAINER_OF(work2, struct pixart_data, init_work);
    const struct device *dev = data->dev;

    LOG_INF("PMW3610 async init step %d", data->async_init_step);

    data->err = async_init_fn[data->async_init_step](dev);
    if (data->err) {
        LOG_ERR("PMW3610 initialization failed in step %d", data->async_init_step);
    } else {
        data->async_init_step++;

        if (data->async_init_step == ASYNC_INIT_STEP_COUNT) {
            data->ready = true; // sensor is ready to work
            LOG_INF("PMW3610 initialized");
            set_interrupt(dev, true);
        } else {
            k_work_schedule(&data->init_work, K_MSEC(async_init_delay[data->async_init_step]));
        }
    }
}

//teraknights add
#define AUTOMOUSE_LAYER (DT_PROP(DT_DRV_INST(0), automouse_layer))
#if AUTOMOUSE_LAYER > 0
struct k_timer automouse_layer_timer;
static bool automouse_triggered = false;

static void activate_automouse_layer() {
    automouse_triggered = true;
    zmk_keymap_layer_activate(AUTOMOUSE_LAYER);
    k_timer_start(&automouse_layer_timer, K_MSEC(CONFIG_PMW3610_AUTOMOUSE_TIMEOUT_MS), K_NO_WAIT);
}

static void deactivate_automouse_layer(struct k_timer *timer) {
    automouse_triggered = false;
    zmk_keymap_layer_deactivate(AUTOMOUSE_LAYER);
}

K_TIMER_DEFINE(automouse_layer_timer, deactivate_automouse_layer, NULL);
#endif
//teraknights end

static int pmw3610_report_data(const struct device *dev) {
    struct pixart_data *data = dev->data;
    const struct pixart_config *config = dev->config;
    uint8_t buf[PMW3610_BURST_SIZE];

    if (unlikely(!data->ready)) {
        LOG_WRN("Device is not initialized yet");
        return -EBUSY;
    }

    static int64_t dx = 0;
    static int64_t dy = 0;

#if CONFIG_PMW3610_REPORT_INTERVAL_MIN > 0
    static int64_t last_smp_time = 0;
    static int64_t last_rpt_time = 0;
    int64_t now = k_uptime_get();
#endif

// teraknights add
#if AUTOMOUSE_LAYER > 0
    if (input_mode == MOVE &&
         (automouse_triggered || zmk_keymap_highest_layer_active() != AUTOMOUSE_LAYER) &&
            (abs(x) + abs(y) > CONFIG_PMW3610_MOVEMENT_THRESHOLD)
) {
    activate_automouse_layer();
}
#endif
// teraknights end
	int err = pmw3610_read(dev, PMW3610_REG_MOTION_BURST, buf, sizeof(buf));
    if (err) {
        return err;
    }

// 12-bit two's complement value to int16_t
// adapted from https://stackoverflow.com/questions/70802306/convert-a-12-bit-signed-number-in-c
#define TOINT16(val, bits) (((struct { int16_t value : bits; }){val}).value)

    int16_t x = TOINT16((buf[PMW3610_X_L_POS] + ((buf[PMW3610_XY_H_POS] & 0xF0) << 4)), 12);
    int16_t y = TOINT16((buf[PMW3610_Y_L_POS] + ((buf[PMW3610_XY_H_POS] & 0x0F) << 8)), 12);

#if IS_ENABLED(CONFIG_PMW3610_SWAP_XY)
    int16_t a = x;
    x = y;
    y = a;
#endif
#if IS_ENABLED(CONFIG_PMW3610_INVERT_X)
    x = -x;
#endif
#if IS_ENABLED(CONFIG_PMW3610_INVERT_Y)
    y = -y;
#endif

#ifdef CONFIG_PMW3610_SMART_ALGORITHM
    int16_t shutter = ((int16_t)(buf[PMW3610_SHUTTER_H_POS] & 0x01) << 8) 
                    + buf[PMW3610_SHUTTER_L_POS];
    if (data->sw_smart_flag && shutter < 45) {
        pmw3610_write(dev, 0x32, 0x00);
        data->sw_smart_flag = false;
    }
    if (!data->sw_smart_flag && shutter > 45) {
        pmw3610_write(dev, 0x32, 0x80);
        data->sw_smart_flag = true;
    }
#endif

#if CONFIG_PMW3610_REPORT_INTERVAL_MIN > 0
    // purge accumulated delta, if last sampled had not been reported on last report tick
    if (now - last_smp_time >= CONFIG_PMW3610_REPORT_INTERVAL_MIN) {
        dx = 0;
        dy = 0;
    }
    last_smp_time = now;
#endif

    // accumulate delta until report in next iteration
    dx += x;
    dy += y;

#if CONFIG_PMW3610_REPORT_INTERVAL_MIN > 0
    // strict to report inerval
    if (now - last_rpt_time < CONFIG_PMW3610_REPORT_INTERVAL_MIN) {
        return 0;
    }
#endif

    // fetch report value
    int16_t rx = (int16_t)CLAMP(dx, INT16_MIN, INT16_MAX);
    int16_t ry = (int16_t)CLAMP(dy, INT16_MIN, INT16_MAX);
    bool have_x = rx != 0;
    bool have_y = ry != 0;

    if (have_x || have_y) {
#if CONFIG_PMW3610_REPORT_INTERVAL_MIN > 0
        last_rpt_time = now;
#endif
        dx = 0;
        dy = 0;
        if (have_x) {
            input_report(dev, config->evt_type, config->x_input_code, rx, !have_y, K_NO_WAIT);
        }
        if (have_y) {
            input_report(dev, config->evt_type, config->y_input_code, ry, true, K_NO_WAIT);
        }
    }

    return err;
}

static void pmw3610_gpio_callback(const struct device *gpiob, struct gpio_callback *cb,
                                  uint32_t pins) {
    struct pixart_data *data = CONTAINER_OF(cb, struct pixart_data, irq_gpio_cb);
    const struct device *dev = data->dev;
    set_interrupt(dev, false);
    k_work_submit(&data->trigger_work);
}

static void pmw3610_work_callback(struct k_work *work) {
    struct pixart_data *data = CONTAINER_OF(work, struct pixart_data, trigger_work);
    const struct device *dev = data->dev;
    pmw3610_report_data(dev);
    set_interrupt(dev, true);
}

static int pmw3610_init_irq(const struct device *dev) {
    int err;
    struct pixart_data *data = dev->data;
    const struct pixart_config *config = dev->config;

    // check readiness of irq gpio pin
    if (!device_is_ready(config->irq_gpio.port)) {
        LOG_ERR("IRQ GPIO device not ready");
        return -ENODEV;
    }

    // init the irq pin
    err = gpio_pin_configure_dt(&config->irq_gpio, GPIO_INPUT);
    if (err) {
        LOG_ERR("Cannot configure IRQ GPIO");
        return err;
    }

    // setup and add the irq callback associated
    gpio_init_callback(&data->irq_gpio_cb, pmw3610_gpio_callback, BIT(config->irq_gpio.pin));

    err = gpio_add_callback(config->irq_gpio.port, &data->irq_gpio_cb);
    if (err) {
        LOG_ERR("Cannot add IRQ GPIO callback");
    }

    return err;
}

static int pmw3610_init(const struct device *dev) {
    struct pixart_data *data = dev->data;
    const struct pixart_config *config = dev->config;
    int err;

	if (!spi_is_ready_dt(&config->spi)) {
		LOG_ERR("%s is not ready", config->spi.bus->name);
		return -ENODEV;
	}

    // init device pointer
    data->dev = dev;

    // init smart algorithm flag;
    data->sw_smart_flag = false;

    // init trigger handler work
    k_work_init(&data->trigger_work, pmw3610_work_callback);

    // init irq routine
    err = pmw3610_init_irq(dev);
    if (err) {
        return err;
    }

    // Setup delayable and non-blocking init jobs, including following steps:
    // 1. power reset
    // 2. upload initial settings
    // 3. other configs like cpi, downshift time, sample time etc.
    // The sensor is ready to work (i.e., data->ready=true after the above steps are finished)
    k_work_init_delayable(&data->init_work, pmw3610_async_init);

    k_work_schedule(&data->init_work, K_MSEC(async_init_delay[data->async_init_step]));

    return err;
}

static int pmw3610_attr_set(const struct device *dev, enum sensor_channel chan,
                            enum sensor_attribute attr, const struct sensor_value *val) {
    struct pixart_data *data = dev->data;
    int err;

    if (unlikely(chan != SENSOR_CHAN_ALL)) {
        return -ENOTSUP;
    }

    if (unlikely(!data->ready)) {
        LOG_DBG("Device is not initialized yet");
        return -EBUSY;
    }

    switch ((uint32_t)attr) {
    case PMW3610_ATTR_CPI:
        err = set_cpi(dev, PMW3610_SVALUE_TO_CPI(*val));
        break;

    case PMW3610_ATTR_RUN_DOWNSHIFT_TIME:
        err = set_downshift_time(dev, PMW3610_REG_RUN_DOWNSHIFT, PMW3610_SVALUE_TO_TIME(*val));
        break;

    case PMW3610_ATTR_REST1_DOWNSHIFT_TIME:
        err = set_downshift_time(dev, PMW3610_REG_REST1_DOWNSHIFT, PMW3610_SVALUE_TO_TIME(*val));
        break;

    case PMW3610_ATTR_REST2_DOWNSHIFT_TIME:
        err = set_downshift_time(dev, PMW3610_REG_REST2_DOWNSHIFT, PMW3610_SVALUE_TO_TIME(*val));
        break;

    case PMW3610_ATTR_REST1_SAMPLE_TIME:
        err = set_sample_time(dev, PMW3610_REG_REST1_RATE, PMW3610_SVALUE_TO_TIME(*val));
        break;

    case PMW3610_ATTR_REST2_SAMPLE_TIME:
        err = set_sample_time(dev, PMW3610_REG_REST2_RATE, PMW3610_SVALUE_TO_TIME(*val));
        break;

    case PMW3610_ATTR_REST3_SAMPLE_TIME:
        err = set_sample_time(dev, PMW3610_REG_REST3_RATE, PMW3610_SVALUE_TO_TIME(*val));
        break;

    default:
        LOG_ERR("Unknown attribute");
        err = -ENOTSUP;
    }

    return err;
}

static const struct sensor_driver_api pmw3610_driver_api = {
    .attr_set = pmw3610_attr_set,
};

#define PMW3610_SPI_MODE (SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_MODE_CPOL | \
                        SPI_MODE_CPHA | SPI_TRANSFER_MSB)

#define PMW3610_DEFINE(n)                                                                          \
    static struct pixart_data data##n;                                                             \
    static const struct pixart_config config##n = {                                                \
		.spi = SPI_DT_SPEC_INST_GET(n, PMW3610_SPI_MODE, 0),		                               \
        .irq_gpio = GPIO_DT_SPEC_INST_GET(n, irq_gpios),                                           \
        .cpi = DT_PROP(DT_DRV_INST(n), cpi),                                                       \
        .evt_type = DT_PROP(DT_DRV_INST(n), evt_type),                                             \
        .x_input_code = DT_PROP(DT_DRV_INST(n), x_input_code),                                     \
        .y_input_code = DT_PROP(DT_DRV_INST(n), y_input_code),                                     \
    };                                                                                             \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(n, pmw3610_init, NULL, &data##n, &config##n, POST_KERNEL,                \
                          CONFIG_SENSOR_INIT_PRIORITY, &pmw3610_driver_api);

DT_INST_FOREACH_STATUS_OKAY(PMW3610_DEFINE)
