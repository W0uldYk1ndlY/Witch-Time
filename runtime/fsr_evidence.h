#ifndef WT_FSR_EVIDENCE_H
#define WT_FSR_EVIDENCE_H

typedef struct {
    int selected, supported, initialized, eligible, camera_conflict, input_mismatch, output_unsupported, dispatched, output_bound, draw_recorded, fallback;
} FsrEvidence;

static const char *fsr_evidence_result(const FsrEvidence *e) {
    if (!e->selected) return "off";
    if (!e->supported) return "unsupported";
    if (!e->eligible) return e->camera_conflict ? "camera-conflict" : "no-eligible-scene";
    if (e->input_mismatch) return "input-extent-mismatch";
    if (e->output_unsupported) return "output-unsupported";
    if (!e->initialized) return "setup-failed";
    if (!e->dispatched) return "not-dispatched";
    if (e->fallback) return "fallback";
    if (!e->output_bound || !e->draw_recorded) return "output-not-consumed";
    return "consumer-recorded";
}

#endif
