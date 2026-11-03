# The ERV link logic (ervlink_core.*), as an ESPHome external component so the YAML can
# load it from this repo. It has no config: firmware/ervlink.yaml creates and wires the core.
import esphome.config_validation as cv

CODEOWNERS = ["@lukewaite"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass
