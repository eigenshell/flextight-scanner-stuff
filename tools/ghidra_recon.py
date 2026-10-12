# SPDX-License-Identifier: GPL-2.0-or-later
# Ghidra post-script: executed via PyGhidra run_script against the saved project
# (analyzeHeadless -postScript does not work on this Ghidra 12.x install — see below):
#   from pyghidra.core import run_script
#   run_script(None, "tools/ghidra_recon.py", project_location="tools/ghidra_proj",
#              project_name="fc403", analyze=False, program_name="FlexColor.dll",
#              nested_project_location=False, install_dir="/opt/ghidra")
# Enumerates functions whose (demangled) name matches SCSI/ASPI target
# class names and dumps addresses as JSON on stdout.
#
# Note: this Ghidra (12.x) starts headless without PyGhidra, so running via
# `analyzeHeadless -postScript` errors out with "Ghidra was not started with
# PyGhidra. Python is not available". It is instead executed through PyGhidra
# (run_script / PyGhidraScript), where script `print` output is redirected to
# a Java PrintWriter that does not reach the terminal. For that reason the
# JSON is additionally written to tools/ghidra_out/recon_json.txt next to
# this script.
import json
import os

from ghidra.app.util.demangler import DemanglerUtil

TARGETS = ["CScannerSCSI", "SCSIScan", "ASPI", "SendASPI32", "CDB",
           "Firmware", "CodeLoaded", "CalcCCDPos", "CatchBands"]

out = {}
fm = currentProgram.getFunctionManager()
for f in fm.getFunctions(True):
    sym = DemanglerUtil.demangle(currentProgram, f.getSymbol())
    name = str(sym) if sym else f.getName()
    for t in TARGETS:
        if t.lower() in name.lower():
            out.setdefault(t, []).append(
                {"name": name, "addr": hex(f.getEntryPoint().getOffset())})

payload = "RECON_JSON " + json.dumps(out)
print(payload)

_repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_out_path = os.path.join(_repo_root, "tools", "ghidra_out", "recon_json.txt")
os.makedirs(os.path.dirname(_out_path), exist_ok=True)
with open(_out_path, "w") as fh:
    fh.write(payload + "\n")
print("RECON_WRITTEN " + _out_path)
