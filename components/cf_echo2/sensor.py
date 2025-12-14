import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, uart
from esphome.const import CONF_ID

CONF_ENERGY = "energy"
CONF_VOLUME = "volume"
CONF_POWER = "power"
CONF_VOLUME_FLOW = "volume_flow"
CONF_FLOW_TEMP = "flow_temp"
CONF_RETURN_TEMP = "return_temp"
CONF_DELTA_T = "delta_t"

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor"]

cf_echo2_ns = cg.esphome_ns.namespace("cf_echo2")
CFEcho2Reader = cf_echo2_ns.class_("CFEcho2Reader", cg.PollingComponent, uart.UARTDevice)

SENSOR_SCHEMA = sensor.sensor_schema(accuracy_decimals=3)
TEMP_SENSOR_SCHEMA = sensor.sensor_schema(accuracy_decimals=1, unit_of_measurement="°C")

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(CFEcho2Reader),
            cv.Optional(CONF_ENERGY): SENSOR_SCHEMA,
            cv.Optional(CONF_VOLUME): SENSOR_SCHEMA,
            cv.Optional(CONF_POWER): SENSOR_SCHEMA,
            cv.Optional(CONF_VOLUME_FLOW): sensor.sensor_schema(accuracy_decimals=3, unit_of_measurement="m³/h"),
            cv.Optional(CONF_FLOW_TEMP): TEMP_SENSOR_SCHEMA,
            cv.Optional(CONF_RETURN_TEMP): TEMP_SENSOR_SCHEMA,
            cv.Optional(CONF_DELTA_T): sensor.sensor_schema(
                accuracy_decimals=1, unit_of_measurement="K"
            ),
        }
    )
    .extend(cv.polling_component_schema("30s"))
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

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
