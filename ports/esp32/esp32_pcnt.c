/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2021-22 Jonathan Hogg
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */


#include "py/runtime.h"
#include "py/mphal.h"
#include "py/obj.h"

#if MICROPY_PY_ESP32_PCNT

#include "modesp32.h"
#include "driver/pcnt.h"


typedef struct _esp32_pcnt_obj_t {
    mp_obj_base_t base;
    pcnt_unit_t unit;
    struct _esp32_pcnt_obj_t *next;
    mp_obj_t handler;
    uint32_t pending;
} esp32_pcnt_obj_t;

MP_REGISTER_ROOT_POINTER(struct _esp32_pcnt_obj_t *esp32_pcnt_obj_head);

static bool pcnt_isr_service_installed = false;

static mp_sched_node_t pcnt_irq_sched_node = {0};

static void IRAM_ATTR esp32_pcnt_run_irq_handlers(mp_sched_node_t *node) {
    (void)node;
    esp32_pcnt_obj_t *pcnt = MP_STATE_PORT(esp32_pcnt_obj_head);
    while (pcnt != NULL) {
        uint32_t status = pcnt->pending;
        pcnt->pending ^= status;
        if (status && pcnt->handler != MP_OBJ_NULL) {
            mp_call_function_1_protected(pcnt->handler, MP_OBJ_NEW_SMALL_INT(status));
        }
        pcnt = pcnt->next;
    }
}

static void esp32_pcnt_intr_handler(void *arg) {
    esp32_pcnt_obj_t *self = (esp32_pcnt_obj_t *)arg;
    pcnt_unit_t unit = self->unit;
    #if ((ESP_IDF_VERSION_MAJOR == 4) && (ESP_IDF_VERSION_MINOR >= 2)) || ESP_IDF_VERSION_MAJOR > 4
    uint32_t status;
    pcnt_get_event_status(unit, &status);
    #else
    uint32_t status = PCNT.status_unit[unit].val;
    #endif
    self->pending |= status;
    mp_sched_schedule_node(&pcnt_irq_sched_node, esp32_pcnt_run_irq_handlers);
}

void esp32_pcnt_deinit_all(void) {
    esp32_pcnt_obj_t **pcnt = &MP_STATE_PORT(esp32_pcnt_obj_head);
    while (*pcnt != NULL) {
        esp32_pcnt_obj_t *next = (*pcnt)->next;
        m_del_obj(esp32_pcnt_obj_t, *pcnt);
        *pcnt = next;
    }
}

static void esp32_pcnt_init_helper(esp32_pcnt_obj_t *self, size_t n_pos_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum {
        ARG_channel,
        ARG_pin,
        ARG_rising,
        ARG_falling,
        ARG_mode_pin,
        ARG_mode_low,
        ARG_mode_high,
        ARG_minimum,
        ARG_maximum,
        ARG_filter,
        ARG_threshold0,
        ARG_threshold1,
        ARG_value,
    };

    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_channel,     MP_ARG_KW_ONLY | MP_ARG_INT,   {.u_int = 0} },
        { MP_QSTR_pin,         MP_ARG_KW_ONLY | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_rising,      MP_ARG_KW_ONLY | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_falling,     MP_ARG_KW_ONLY | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_mode_pin,    MP_ARG_KW_ONLY | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_mode_low,    MP_ARG_KW_ONLY | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_mode_high,   MP_ARG_KW_ONLY | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_minimum,     MP_ARG_KW_ONLY | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_maximum,     MP_ARG_KW_ONLY | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_filter,      MP_ARG_KW_ONLY | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_threshold0,  MP_ARG_KW_ONLY | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_threshold1,  MP_ARG_KW_ONLY | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_value,       MP_ARG_KW_ONLY | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_pos_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_uint_t channel = args[ARG_channel].u_int;
    if (channel >= PCNT_CHANNEL_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("channel"));
    }

    if (args[ARG_pin].u_obj != MP_OBJ_NULL || args[ARG_mode_pin].u_obj != MP_OBJ_NULL) {
        if (args[ARG_pin].u_obj == MP_OBJ_NULL) {
            mp_raise_ValueError(MP_ERROR_TEXT("pin"));
        }
        mp_hal_pin_obj_t pin = mp_hal_get_pin_obj(args[ARG_pin].u_obj);
        mp_hal_pin_obj_t mode_pin = args[ARG_mode_pin].u_obj == MP_OBJ_NULL ? PCNT_PIN_NOT_USED : mp_hal_get_pin_obj(args[ARG_mode_pin].u_obj);
        pcnt_set_pin(self->unit, channel, pin, mode_pin);
    }

    if (
        args[ARG_rising].u_obj != MP_OBJ_NULL || args[ARG_falling].u_obj != MP_OBJ_NULL ||
        args[ARG_mode_low].u_obj != MP_OBJ_NULL || args[ARG_mode_high].u_obj != MP_OBJ_NULL
        ) {
        mp_uint_t rising = args[ARG_rising].u_obj == MP_OBJ_NULL ? PCNT_COUNT_DIS : mp_obj_get_int(args[ARG_rising].u_obj);
        mp_uint_t falling = args[ARG_falling].u_obj == MP_OBJ_NULL ? PCNT_COUNT_DIS : mp_obj_get_int(args[ARG_falling].u_obj);
        mp_uint_t mode_low = args[ARG_mode_low].u_obj == MP_OBJ_NULL ? PCNT_MODE_KEEP : mp_obj_get_int(args[ARG_mode_low].u_obj);
        mp_uint_t mode_high = args[ARG_mode_high].u_obj == MP_OBJ_NULL ? PCNT_MODE_KEEP : mp_obj_get_int(args[ARG_mode_high].u_obj);
        if (rising >= PCNT_COUNT_MAX) {
            mp_raise_ValueError(MP_ERROR_TEXT("rising"));
        }
        if (falling >= PCNT_COUNT_MAX) {
            mp_raise_ValueError(MP_ERROR_TEXT("falling"));
        }
        if (mode_low >= PCNT_MODE_MAX) {
            mp_raise_ValueError(MP_ERROR_TEXT("mode_low"));
        }
        if (mode_high >= PCNT_MODE_MAX) {
            mp_raise_ValueError(MP_ERROR_TEXT("mode_high"));
        }
        pcnt_set_mode(self->unit, channel, rising, falling, mode_high, mode_low);
    }

    if (args[ARG_filter].u_obj != MP_OBJ_NULL) {
        mp_uint_t filter = mp_obj_get_int(args[ARG_filter].u_obj);
        if (filter > 1023) {
            mp_raise_ValueError(MP_ERROR_TEXT("filter"));
        }
        if (filter) {
            check_esp_err(pcnt_set_filter_value(self->unit, filter));
            check_esp_err(pcnt_filter_enable(self->unit));
        } else {
            check_esp_err(pcnt_filter_disable(self->unit));
        }
    }

    bool clear = false;

    if (args[ARG_minimum].u_obj != MP_OBJ_NULL) {
        mp_int_t minimum = mp_obj_get_int(args[ARG_minimum].u_obj);
        if (minimum < -32768 || minimum > 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("minimum"));
        }
        check_esp_err(pcnt_set_event_value(self->unit, PCNT_EVT_L_LIM, minimum));
        clear = true;
    }

    if (args[ARG_maximum].u_obj != MP_OBJ_NULL) {
        mp_int_t maximum = mp_obj_get_int(args[ARG_maximum].u_obj);
        if (maximum < 0 || maximum > 32767) {
            mp_raise_ValueError(MP_ERROR_TEXT("maximum"));
        }
        check_esp_err(pcnt_set_event_value(self->unit, PCNT_EVT_H_LIM, maximum));
        clear = true;
    }

    if (args[ARG_threshold0].u_obj != MP_OBJ_NULL) {
        mp_int_t threshold0 = mp_obj_get_int(args[ARG_threshold0].u_obj);
        if (threshold0 < -32768 || threshold0 > 32767) {
            mp_raise_ValueError(MP_ERROR_TEXT("threshold0"));
        }
        check_esp_err(pcnt_set_event_value(self->unit, PCNT_EVT_THRES_0, threshold0));
        clear = true;
    }

    if (args[ARG_threshold1].u_obj != MP_OBJ_NULL) {
        mp_int_t threshold1 = mp_obj_get_int(args[ARG_threshold1].u_obj);
        if (threshold1 < -32768 || threshold1 > 32767) {
            mp_raise_ValueError(MP_ERROR_TEXT("threshold1"));
        }
        check_esp_err(pcnt_set_event_value(self->unit, PCNT_EVT_THRES_1, threshold1));
        clear = true;
    }

    if (args[ARG_value].u_obj != MP_OBJ_NULL) {
        mp_int_t value = mp_obj_get_int(args[ARG_value].u_obj);
        if (value != 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("can only set value to 0"));
        }
        clear = true;
    }

    if (clear) {
        check_esp_err(pcnt_counter_clear(self->unit));
    }
}

static void esp32_pcnt_disable_events_helper(esp32_pcnt_obj_t *self) {
    if (self->handler != MP_OBJ_NULL) {
        for (pcnt_evt_type_t evt_type = PCNT_EVT_THRES_1; evt_type <= PCNT_EVT_ZERO; evt_type <<= 1) {
            check_esp_err(pcnt_event_disable(self->unit, evt_type));
        }
        check_esp_err(pcnt_isr_handler_remove(self->unit));
        self->handler = MP_OBJ_NULL;
        self->pending = 0;
    }
}

static void esp32_pcnt_deinit_helper(esp32_pcnt_obj_t *self) {
    esp32_pcnt_disable_events_helper(self);

    pcnt_config_t channel_config = {
        .unit = self->unit,
        .channel = 0,
        .pulse_gpio_num = PCNT_PIN_NOT_USED,
        .pos_mode = PCNT_COUNT_DIS,
        .neg_mode = PCNT_COUNT_DIS,
        .ctrl_gpio_num = PCNT_PIN_NOT_USED,
        .lctrl_mode = PCNT_MODE_KEEP,
        .hctrl_mode = PCNT_MODE_KEEP,
        .counter_l_lim = 0,
        .counter_h_lim = 0,
    };
    check_esp_err(pcnt_unit_config(&channel_config));
    channel_config.channel = 1;
    check_esp_err(pcnt_unit_config(&channel_config));

    check_esp_err(pcnt_filter_disable(self->unit));
    check_esp_err(pcnt_set_event_value(self->unit, PCNT_EVT_THRES_0, 0));
    check_esp_err(pcnt_set_event_value(self->unit, PCNT_EVT_THRES_1, 0));
    check_esp_err(pcnt_counter_pause(self->unit));
    check_esp_err(pcnt_counter_clear(self->unit));
}

static mp_obj_t esp32_pcnt_make_new(const mp_obj_type_t *type, size_t n_pos_args, size_t n_kw_args, const mp_obj_t *args) {
    if (n_pos_args > 1) {
        mp_raise_TypeError(MP_ERROR_TEXT("function takes at most 1 positional argument"));
    }

    pcnt_unit_t unit = PCNT_UNIT_MAX;
    if (n_pos_args == 1) {
        unit = mp_obj_get_int(args[0]);
        if (unit < 0 || unit >= PCNT_UNIT_MAX) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid id"));
        }
    }

    esp32_pcnt_obj_t *self = MP_STATE_PORT(esp32_pcnt_obj_head);
    mp_uint_t allocated = 0;
    while (self != NULL) {
        if (self->unit == unit) {
            break;
        }
        allocated |= 1 << self->unit;
        self = self->next;
    }
    if (self == NULL) {
        if (unit == PCNT_UNIT_MAX) {
            for (unit = 0; unit < PCNT_UNIT_MAX; unit++) {
                if (!(allocated & 1)) {
                    break;
                }
                allocated >>= 1;
            }
            if (unit == PCNT_UNIT_MAX) {
                mp_raise_TypeError(MP_ERROR_TEXT("no unused PCNT unit"));
            }
        }
        self = m_new_obj(esp32_pcnt_obj_t);
        self->base.type = &esp32_pcnt_type;
        self->unit = unit;
        self->next = MP_STATE_PORT(esp32_pcnt_obj_head);
        MP_STATE_PORT(esp32_pcnt_obj_head) = self;
        esp32_pcnt_deinit_helper(self);
    }

    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw_args, args + n_pos_args);
    esp32_pcnt_init_helper(self, 0, args + n_pos_args, &kw_args);

    if (!pcnt_isr_service_installed) {
        check_esp_err(pcnt_isr_service_install(ESP_INTR_FLAG_IRAM));
        pcnt_isr_service_installed = true;
    }
    check_esp_err(pcnt_intr_enable(self->unit));

    return MP_OBJ_FROM_PTR(self);
}

static void esp32_pcnt_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    esp32_pcnt_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "PCNT(%u)", self->unit);
}

static mp_obj_t esp32_pcnt_init(size_t n_pos_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    esp32_pcnt_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    esp32_pcnt_init_helper(self, n_pos_args - 1, pos_args + 1, kw_args);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esp32_pcnt_init_obj, 1, esp32_pcnt_init);

static mp_obj_t esp32_pcnt_deinit(mp_obj_t self_in) {
    esp32_pcnt_obj_t *self = MP_OBJ_TO_PTR(self_in);
    esp32_pcnt_deinit_helper(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp32_pcnt_deinit_obj, esp32_pcnt_deinit);

static mp_obj_t esp32_pcnt_value(size_t n_args, const mp_obj_t *pos_args) {
    esp32_pcnt_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    int16_t value;
    if (n_args == 2) {
        value = mp_obj_get_int(pos_args[1]);
        if (value != 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("can only set value to 0"));
        }
    }
    while (true) {
        check_esp_err(pcnt_get_counter_value(self->unit, &value));
        if (self->pending && self->handler != MP_OBJ_NULL) {
            esp32_pcnt_run_irq_handlers(NULL);
        } else {
            break;
        }
    }
    if (n_args == 2) {
        check_esp_err(pcnt_counter_clear(self->unit));
    }
    return MP_OBJ_NEW_SMALL_INT(value);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esp32_pcnt_value_obj, 1, 2, esp32_pcnt_value);

static mp_obj_t esp32_pcnt_irq(size_t n_pos_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum {
        ARG_handler,
        ARG_trigger,
    };

    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_handler,  MP_ARG_OBJ,  {.u_obj = mp_const_none} },
        { MP_QSTR_trigger,  MP_ARG_INT,  {.u_int = PCNT_EVT_ZERO} },
    };

    esp32_pcnt_obj_t *self = pos_args[0];
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_pos_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_t handler = args[ARG_handler].u_obj;
    mp_uint_t trigger = args[ARG_trigger].u_int;

    if (trigger < PCNT_EVT_THRES_1 || trigger >= (PCNT_EVT_ZERO << 1)) {
        mp_raise_ValueError(MP_ERROR_TEXT("trigger"));
    }

    if (handler != mp_const_none) {
        if (self->handler == MP_OBJ_NULL) {
            pcnt_isr_handler_add(self->unit, esp32_pcnt_intr_handler, (void *)self);
        }
        self->handler = handler;
        for (pcnt_evt_type_t evt_type = PCNT_EVT_THRES_1; evt_type <= PCNT_EVT_ZERO; evt_type <<= 1) {
            if (trigger & evt_type) {
                pcnt_event_enable(self->unit, evt_type);
            } else {
                pcnt_event_disable(self->unit, evt_type);
            }
        }
    } else {
        esp32_pcnt_disable_events_helper(self);
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esp32_pcnt_irq_obj, 1, esp32_pcnt_irq);

static mp_obj_t esp32_pcnt_start(mp_obj_t self_in) {
    esp32_pcnt_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_esp_err(pcnt_counter_resume(self->unit));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp32_pcnt_start_obj, esp32_pcnt_start);

static mp_obj_t esp32_pcnt_stop(mp_obj_t self_in) {
    esp32_pcnt_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_esp_err(pcnt_counter_pause(self->unit));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp32_pcnt_stop_obj, esp32_pcnt_stop);

static const mp_rom_map_elem_t esp32_pcnt_locals_dict_table[] = {
    // Methods
    { MP_ROM_QSTR(MP_QSTR_init),            MP_ROM_PTR(&esp32_pcnt_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_value),           MP_ROM_PTR(&esp32_pcnt_value_obj) },
    { MP_ROM_QSTR(MP_QSTR_irq),             MP_ROM_PTR(&esp32_pcnt_irq_obj) },
    { MP_ROM_QSTR(MP_QSTR_start),           MP_ROM_PTR(&esp32_pcnt_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop),            MP_ROM_PTR(&esp32_pcnt_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),          MP_ROM_PTR(&esp32_pcnt_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__),         MP_ROM_PTR(&esp32_pcnt_deinit_obj) },

    // Constants
    { MP_ROM_QSTR(MP_QSTR_IGNORE),          MP_ROM_INT(PCNT_COUNT_DIS) },
    { MP_ROM_QSTR(MP_QSTR_INCREMENT),       MP_ROM_INT(PCNT_COUNT_INC) },
    { MP_ROM_QSTR(MP_QSTR_DECREMENT),       MP_ROM_INT(PCNT_COUNT_DEC) },
    { MP_ROM_QSTR(MP_QSTR_NORMAL),          MP_ROM_INT(PCNT_MODE_KEEP) },
    { MP_ROM_QSTR(MP_QSTR_REVERSE),         MP_ROM_INT(PCNT_MODE_REVERSE) },
    { MP_ROM_QSTR(MP_QSTR_HOLD),            MP_ROM_INT(PCNT_MODE_DISABLE) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_ZERO),        MP_ROM_INT(PCNT_EVT_ZERO) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_THRESHOLD0),  MP_ROM_INT(PCNT_EVT_THRES_0) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_THRESHOLD1),  MP_ROM_INT(PCNT_EVT_THRES_1) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_MINIMUM),     MP_ROM_INT(PCNT_EVT_L_LIM) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_MAXIMUM),     MP_ROM_INT(PCNT_EVT_H_LIM) },
};
static MP_DEFINE_CONST_DICT(esp32_pcnt_locals_dict, esp32_pcnt_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    esp32_pcnt_type,
    MP_QSTR_PCNT,
    MP_TYPE_FLAG_NONE,
    make_new, esp32_pcnt_make_new,
    print, esp32_pcnt_print,
    locals_dict, &esp32_pcnt_locals_dict
    );


#endif // MICROPY_PY_ESP32_PCNT