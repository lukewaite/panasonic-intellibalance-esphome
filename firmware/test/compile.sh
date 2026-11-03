#!/bin/sh
# Compile ervlink.yaml for the ESP32-C6 against the component in this checkout (not the
# tagged one on GitHub), with placeholder secrets. Checks the YAML and the core build for
# the device; the result is not for flashing.
set -e
cd "$(dirname "$0")"
out=build/esphome
mkdir -p "$out"
python3 - "$PWD/../ervlink.yaml" "$PWD/../components" "$out/ervlink.yaml" <<'PY'
import re, sys
src, components, dst = sys.argv[1:]
y = open(src).read()
y, n = re.subn(r"  - source:\n      type: git\n(?:      .*\n)+?      path: firmware/components\n",
               f"  - source:\n      type: local\n      path: {components}\n", y)
assert n == 1, "external_components source not found"
open(dst, "w").write(y)
PY
# Placeholders: a made-up Thread dataset (only checked to be hex) and a random API key.
cat > "$out/secrets.yaml" <<SECRETS
thread_tlv: "0e080000000000010000000300000f02080011223344556677"
ervlink_api_key: "$(head -c 32 /dev/urandom | base64)"
SECRETS
esphome compile "$out/ervlink.yaml"
