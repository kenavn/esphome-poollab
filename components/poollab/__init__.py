"""PoolLab 1.0 BLE photometer — ESPHome external component.

Reads stored measurements over BLE on demand and pushes them to Home Assistant
as both per-record events and per-parameter sensors. See PROTOCOL.md.

STATUS: scaffold — BLE state machine needs bench iteration against a real device.
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import ble_client, sensor
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    CONF_TYPE,
    CONF_UNIT_OF_MEASUREMENT,
    CONF_TRIGGER_ID,
)

CODEOWNERS = ["@kenavn"]
DEPENDENCIES = ["ble_client"]
AUTO_LOAD = ["sensor"]

poollab_ns = cg.esphome_ns.namespace("poollab")
PoolLab = poollab_ns.class_(
    "PoolLab", cg.Component, ble_client.BLEClientNode
)
PoolLabReadAction = poollab_ns.class_("PoolLabReadAction", automation.Action)
PoolLabResetAction = poollab_ns.class_("PoolLabResetMeasuresAction", automation.Action)

CONF_RESET_AFTER_READ = "reset_after_read"
CONF_EVENT_NAME = "event_name"
CONF_RESULT_COUNT = "result_count"
CONF_BATTERY_LEVEL = "battery_level"
CONF_PARAMETERS = "parameters"

PARAMETER_SCHEMA = sensor.sensor_schema().extend(
    {
        cv.Required(CONF_TYPE): cv.uint8_t,  # measure_type code (see PROTOCOL.md)
    }
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PoolLab),
            cv.Optional(CONF_RESET_AFTER_READ, default=False): cv.boolean,
            cv.Optional(
                CONF_EVENT_NAME, default="esphome.poollab_measurement"
            ): cv.string,
            cv.Optional(CONF_RESULT_COUNT): sensor.sensor_schema(
                accuracy_decimals=0
            ),
            cv.Optional(CONF_BATTERY_LEVEL): sensor.sensor_schema(
                unit_of_measurement="%", accuracy_decimals=0
            ),
            cv.Optional(CONF_PARAMETERS): cv.ensure_list(PARAMETER_SCHEMA),
        }
    )
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)

    cg.add(var.set_reset_after_read(config[CONF_RESET_AFTER_READ]))
    cg.add(var.set_event_name(config[CONF_EVENT_NAME]))

    if CONF_RESULT_COUNT in config:
        s = await sensor.new_sensor(config[CONF_RESULT_COUNT])
        cg.add(var.set_result_count_sensor(s))
    if CONF_BATTERY_LEVEL in config:
        s = await sensor.new_sensor(config[CONF_BATTERY_LEVEL])
        cg.add(var.set_battery_sensor(s))

    for p in config.get(CONF_PARAMETERS, []):
        s = await sensor.new_sensor(p)
        cg.add(var.register_parameter_sensor(p[CONF_TYPE], s))


POOLLAB_ACTION_SCHEMA = cv.Schema({cv.Required(CONF_ID): cv.use_id(PoolLab)})


@automation.register_action(
    "poollab.read", PoolLabReadAction, automation.maybe_simple_id(POOLLAB_ACTION_SCHEMA), synchronous=False
)
async def poollab_read_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "poollab.reset_measures", PoolLabResetAction, automation.maybe_simple_id(POOLLAB_ACTION_SCHEMA), synchronous=False
)
async def poollab_reset_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
