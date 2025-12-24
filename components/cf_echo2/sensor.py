import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import button, sensor, uart
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLUME,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
)

CONF_ENERGY = "energy"
CONF_VOLUME = "volume"
CONF_POWER = "power"
CONF_VOLUME_FLOW = "volume_flow"
CONF_FLOW_TEMP = "flow_temp"
CONF_RETURN_TEMP = "return_temp"
CONF_DELTA_T = "delta_t"
CONF_READ_BUTTON = "read_button"

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "button"]

cf_echo2_ns = cg.esphome_ns.namespace("cf_echo2")
CFEcho2Reader = cf_echo2_ns.class_("CFEcho2Reader", cg.PollingComponent, uart.UARTDevice)
CFEcho2ReadAction = cf_echo2_ns.class_("CFEcho2ReadAction", automation.Action)
CFEcho2ReadButton = cf_echo2_ns.class_("CFEcho2ReadButton", button.Button)

SENSOR_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=3,
    state_class=STATE_CLASS_TOTAL_INCREASING,
    device_class=DEVICE_CLASS_ENERGY,
    unit_of_measurement="kWh",
)
VOLUME_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=3,
    state_class=STATE_CLASS_TOTAL_INCREASING,
    device_class=DEVICE_CLASS_VOLUME,
    unit_of_measurement="m³",
)
POWER_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=3,
    state_class=STATE_CLASS_MEASUREMENT,
    device_class=DEVICE_CLASS_POWER,
    unit_of_measurement="W",
)
VOLUME_FLOW_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=3,
    state_class=STATE_CLASS_MEASUREMENT,
    unit_of_measurement="m³/h",
)
TEMP_SENSOR_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=1,
    unit_of_measurement="°C",
    device_class=DEVICE_CLASS_TEMPERATURE,
    state_class=STATE_CLASS_MEASUREMENT,
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(CFEcho2Reader),
            cv.Optional(CONF_READ_BUTTON): button.button_schema(
                CFEcho2ReadButton,
                icon="mdi:meter-gas",
            ),
            cv.Optional(CONF_ENERGY): SENSOR_SCHEMA,
            cv.Optional(CONF_VOLUME): VOLUME_SCHEMA,
            cv.Optional(CONF_POWER): POWER_SCHEMA,
            cv.Optional(CONF_VOLUME_FLOW): VOLUME_FLOW_SCHEMA,
            cv.Optional(CONF_FLOW_TEMP): TEMP_SENSOR_SCHEMA,
            cv.Optional(CONF_RETURN_TEMP): TEMP_SENSOR_SCHEMA,
            cv.Optional(CONF_DELTA_T): sensor.sensor_schema(
                accuracy_decimals=1,
                unit_of_measurement="K",
                state_class=STATE_CLASS_MEASUREMENT,
            ),
        }
    )
    .extend(cv.polling_component_schema("30s"))
    .extend(uart.UART_DEVICE_SCHEMA)
)

CF_ECHO2_READ_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(CFEcho2Reader),
    }
)


@automation.register_action(
    "cf_echo2.read",
    CFEcho2ReadAction,
    CF_ECHO2_READ_ACTION_SCHEMA,
)
async def cf_echo2_read_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    if read_button_config := config.get(CONF_READ_BUTTON):
        btn = await button.new_button(read_button_config)
        await cg.register_parented(btn, config[CONF_ID])

    if CONF_ENERGY in config:
        sens = await sensor.new_sensor(config[CONF_ENERGY])
        cg.add(var.set_energy_sensor(sens))
    if CONF_VOLUME in config:
        sens = await sensor.new_sensor(config[CONF_VOLUME])
        cg.add(var.set_volume_sensor(sens))
    if CONF_POWER in config:
        sens = await sensor.new_sensor(config[CONF_POWER])
        cg.add(var.set_power_sensor(sens))
    if CONF_VOLUME_FLOW in config:
        sens = await sensor.new_sensor(config[CONF_VOLUME_FLOW])
        cg.add(var.set_volume_flow_sensor(sens))
    if CONF_FLOW_TEMP in config:
        sens = await sensor.new_sensor(config[CONF_FLOW_TEMP])
        cg.add(var.set_flow_temp_sensor(sens))
    if CONF_RETURN_TEMP in config:
        sens = await sensor.new_sensor(config[CONF_RETURN_TEMP])
        cg.add(var.set_return_temp_sensor(sens))
    if CONF_DELTA_T in config:
        sens = await sensor.new_sensor(config[CONF_DELTA_T])
        cg.add(var.set_delta_t_sensor(sens))
