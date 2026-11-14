"""Disposable host checks. Does not instrument private inputs or touch device state."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEMP = Path(os.environ.get("LOCALAPPDATA", "")) / "Temp/opencode" if os.name == "nt" else None
cc = shutil.which("gcc") or shutil.which("clang") or "cc"
with tempfile.TemporaryDirectory(prefix="temporal_source_", dir=TEMP) as directory:
    tmp = Path(directory)
    exe = tmp / ("test.exe" if os.name == "nt" else "test")
    subprocess.run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-I", str(ROOT / "runtime"),
                    str(ROOT / "tests/temporal_source_capture_test.c"),
                    str(ROOT / "runtime/temporal_source_capture.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe), str(tmp)], check=True)
    window = json.loads((tmp / "window/temporal_source.json").read_text())
    assert window["records"] == 8 and window["backend_selection_records"] == 1
    assert window["join"] == "unknown" and window["backend_execution"] == "unavailable"
    assert window["motion_source_qualified"] is False
    assert window["complete"] and window["core_loss_checked"]
    epoch = json.loads((tmp / "epoch/temporal_source.json").read_text())
    assert epoch["complete"] and epoch["records"] == 1 and epoch["unavailable"] == 0
    malformed = json.loads((tmp / "malformed_current/temporal_source.json").read_text())
    assert not malformed["complete"] and malformed["records"] == 0 and malformed["unavailable"] == 1
    missing = json.loads((tmp / "missing/temporal_source.json").read_text())
    assert missing["unavailable"] == 1 and not missing["guest_bound"]
    budget = json.loads((tmp / "budget/temporal_source.json").read_text())
    assert budget["records"] == budget["record_cap"] and budget["overflow"] == 1
    assert budget["bytes"] <= budget["byte_cap"]
    unknown = json.loads((tmp / "unknown_generation/temporal_source.json").read_text())
    assert unknown["unavailable"] == 2 and unknown["records"] == 1
    no_scene = json.loads((tmp / "no_scene/temporal_source.json").read_text())
    assert not no_scene["eligible_seen"] and no_scene["guest_sites"] == "unavailable"
    buffers = json.loads((tmp / "buffer_budget/temporal_source.json").read_text())
    assert buffers["unavailable"] == 1 and buffers["records"] == 32
    lost = json.loads((tmp / "core_loss/temporal_source.json").read_text())
    assert lost["core_lost_records"] == 1 and not lost["complete"]
    for diagnostic in (0, 1):
        subprocess.run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", f"-DWT_DIAGNOSTIC_ENABLED={diagnostic}",
                        str(ROOT / "tests/temporal_source_bootstrap_test.c"), "-o", str(exe)], check=True)
        subprocess.run([str(exe), str(tmp)], check=True)
    subprocess.run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-pthread",
                    str(ROOT / "tests/temporal_source_concurrent_test.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe), str(tmp)], check=True)
    concurrent = json.loads((tmp / "concurrent/temporal_source.json").read_text())
    assert concurrent["complete"] and concurrent["records"] == 800
    unpublished = json.loads((tmp / "unpublished/temporal_source.json").read_text())
    assert not unpublished["complete"] and unpublished["pending_writers"] == 1
    assert unpublished["unpublished_slots"] == 1 and unpublished["records"] == 0
    boundary = json.loads((tmp / "boundary_0/temporal_source.json").read_text())
    assert boundary["complete"] and boundary["records"] == 1
    racing = json.loads((tmp / "boundary_1/temporal_source.json").read_text())
    assert not racing["complete"] and racing["unavailable"] == 1
    for malformed in (0, 1):
        stale = json.loads((tmp / f"stale_new_{malformed}/temporal_source.json").read_text())
        assert stale["complete"] and stale["records"] == 1 and stale["unavailable"] == 0
        assert stale["pending_writers"] == 0 and stale["unpublished_slots"] == 0
    finish_arm = json.loads((tmp / "finish_arm/temporal_source.json").read_text())
    assert not finish_arm["complete"] and finish_arm["tracking_busy"] and finish_arm["records"] == 0
    subprocess.run([cc, "-std=c11", "-O2", "-Wall", "-Wextra",
                    str(ROOT / "tests/temporal_source_permissions_test.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe), str(tmp)], check=True)

    provenance_spec = importlib.util.spec_from_file_location("provenance", ROOT / "tools/temporal_source_provenance.py")
    provenance = importlib.util.module_from_spec(provenance_spec); provenance_spec.loader.exec_module(provenance)
    fixture = tmp / "empty_repo"; fixture.mkdir()
    assert not provenance.verify(fixture, str(fixture), "unused")["reuse_verified"]
    (fixture / "build/gen_eh").mkdir(parents=True)
    assert not provenance.verify(fixture, str(fixture), "unused")["reuse_verified"]

    spec = importlib.util.spec_from_file_location("instrument", ROOT / "tools/temporal_source_instrument.py")
    instrument = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(instrument)
    # Public synthetic fixture: exercise labels, exact match checks, backups and failure atomicity.
    import hashlib
    gen = tmp / "gen"; gen.mkdir()
    for name, sites in instrument.SITES.items():
        lines = [f'L_{site}: synthetic_instruction;' for site in sites]
        for site, line in zip(sites, lines):
            sites[site] = hashlib.sha256(line.encode()).hexdigest()
        b = ('#include "funcs.h"\n' + '\n'.join(lines) + '\n').encode()
        instrument.SOURCE_SHA[name] = hashlib.sha256(b).hexdigest()
        (gen / name).write_bytes(b)
    original = {p.name: p.read_bytes() for p in gen.iterdir()}
    result = instrument.run(gen)
    assert all((gen / name).read_bytes() == b for name, b in original.items())
    backup = tmp / "private_backup"
    instrument.run(gen, backup)
    for name, b in original.items():
        assert (backup / name).read_bytes() == b
        text = (gen / name).read_text()
        for site in instrument.SITES[name]:
            assert f'L_{site}: wt_temporal_guest_site(c, 0x{site}ULL); ' in text
    try:
        instrument.run(gen, tmp / "bad_backup")
        raise AssertionError("re-instrumentation accepted")
    except ValueError:
        assert not (tmp / "bad_backup").exists()
print("Temporal source capture host, bootstrap and instrumentation checks passed; no device evidence")
