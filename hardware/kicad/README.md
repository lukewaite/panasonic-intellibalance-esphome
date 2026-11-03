# KiCad project

Everything here is generated: edit the scripts in `gen/`, never the KiCad files.

```bash
cd gen
python3 gen_sch.py                       # schematic (ervlink.kicad_sch)
KPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3
$KPY gen_pcb.py                          # place, autoroute (Freerouting), pour
$KPY gen_pcb.py --silk-only              # change existing silkscreen text only
$KPY gen_fab.py                          # fab/: gerber zip, JLCPCB BOM + CPL
```

- **Freerouting isn't deterministic**: a full `gen_pcb.py` reroutes every track. For text
  changes use `--silk-only`, so the copper stays as ordered.
- Freerouting 2.4.1 is at `~/.local/share/freerouting/` and needs Java 25 (`mise.toml`).
  `kicad-cli` is at `/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli`.
- `gen_fab.py` carries JLCPCB rotation/offset corrections (`CPL_ROT`, `CPL_OFFSET`) measured
  against their placement preview; re-check the preview after any footprint change.
