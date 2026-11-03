#!/bin/sh
# Compile a device config (default ervlink.yaml) for the ESP32-C6 against the package and
# component in this checkout (not the tagged ones on GitHub), with placeholder secrets.
# Checks the YAML and the core build for the device; the result is not for flashing.
#   test/compile.sh [example-wifi.yaml] [config|compile]
set -e
cd "$(dirname "$0")"
device=${1:-ervlink.yaml}
command=${2:-compile}
out=build/esphome/${device%.yaml}
mkdir -p "$out"
python3 - "$PWD/.." "$device" "$out" <<'PY'
import re, sys
fw, device, out = sys.argv[1:]
pkg = open(f"{fw}/ervlink-package.yaml").read()
pkg, n = re.subn(r"  - source:\n      type: git\n(?:      .*\n)+?      path: firmware/components\n",
                 f"  - source:\n      type: local\n      path: {fw}/components\n", pkg)
assert n == 1, "external_components source not found in the package"
open(f"{out}/ervlink-package.yaml", "w").write(pkg)
y = open(f"{fw}/{device}").read()
y, n = re.subn(r"github://lukewaite/panasonic-intellibalance-esphome/firmware/ervlink-package\.yaml@\S+",
               "!include ervlink-package.yaml", y)
assert n == 1, "package line not found in " + device
open(f"{out}/{device}", "w").write(y)
PY
# Placeholders: a made-up Thread dataset (only checked to be hex), Wi-Fi, a random API key.
cat > "$out/secrets.yaml" <<SECRETS
thread_tlv: "0e080000000000010000000300000f02080011223344556677"
wifi_ssid: "placeholder"
wifi_password: "placeholder"
ervlink_api_key: "$(head -c 32 /dev/urandom | base64)"
SECRETS
esphome "$command" "$out/$device"
