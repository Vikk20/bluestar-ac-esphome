import esphome.codegen as cg
from esphome import automation
from esphome.components import climate, uart
import esphome.config_validation as cv
from esphome.const import CONF_TRIGGER_ID

DEPENDENCIES = ["uart"]

CONF_ON_RECORD = "on_record"

bluestar_ns = cg.esphome_ns.namespace("bluestar_ac")
BlueStarClimate = bluestar_ns.class_(
    "BlueStarClimate", climate.Climate, cg.Component, uart.UARTDevice
)
RecordTrigger = bluestar_ns.class_(
    "RecordTrigger", automation.Trigger.template(cg.std_string, cg.std_string)
)

CONFIG_SCHEMA = (
    climate.climate_schema(BlueStarClimate)
    .extend(
        {
            cv.Optional(CONF_ON_RECORD): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(RecordTrigger)}
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "bluestar_ac", baud_rate=9600, require_tx=True, require_rx=True
)


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    for conf in config.get(CONF_ON_RECORD, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.std_string, "key"), (cg.std_string, "value")], conf
        )
