"""Check that FSR observation hooks remain at the actual renderer boundaries."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
fsr = (root / "runtime" / "nvn_vk_fsr.c").read_text(encoding="utf-8")
draw = (root / "runtime" / "nvn_vk_draw.c").read_text(encoding="utf-8")
dither = fsr.split("static VkImageView fsr_dither_view(", 1)[1].split("static VkImageView fsr_up_view(", 1)[0]
assert "F.evidence." not in dither, "discovering dither must not report FSR execution"
run = fsr.split("static void fsr_run(", 1)[1].split("#include \"fsr_dither.h\"", 1)[0]
assert run.index("fsr_dispatch(P_ACC") < run.index("F.evidence.dispatched = 1")
sample = draw.split("static void resolve_sampler(", 1)[1].split("static int have_targets_live(void);", 1)[0]
assert sample.index("if (ok && vt->samples == 1)") < sample.index("fsr_evidence_sample(")
assert draw.count("fsr_evidence_draw();") == 3, "direct, indexed and CPU-resolved indirect draws must report recorded work"
print("FSR evidence wiring checks passed (source-level; no GPU execution)")
