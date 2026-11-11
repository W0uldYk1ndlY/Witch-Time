#include "fsr_evidence.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    FsrEvidence e = {0};
    assert(!strcmp(fsr_evidence_result(&e), "off"));
    e.selected = 1;
    assert(!strcmp(fsr_evidence_result(&e), "unsupported"));
    e.supported = 1;
    assert(!strcmp(fsr_evidence_result(&e), "no-eligible-scene"));
    e.camera_conflict = 1;
    assert(!strcmp(fsr_evidence_result(&e), "camera-conflict"));
    e.camera_conflict = 0;
    e.eligible = 1;
    e.input_mismatch = 1;
    assert(!strcmp(fsr_evidence_result(&e), "input-extent-mismatch"));
    e.input_mismatch = 0;
    e.output_unsupported = 1;
    assert(!strcmp(fsr_evidence_result(&e), "output-unsupported"));
    e.output_unsupported = 0;
    assert(!strcmp(fsr_evidence_result(&e), "setup-failed"));
    e.initialized = 1;
    assert(!strcmp(fsr_evidence_result(&e), "not-dispatched"));
    e.dispatched = 1;
    assert(!strcmp(fsr_evidence_result(&e), "output-not-consumed"));
    e.output_bound = 1;
    assert(!strcmp(fsr_evidence_result(&e), "output-not-consumed"));
    e.draw_recorded = 1;
    assert(!strcmp(fsr_evidence_result(&e), "consumer-recorded"));
    e.fallback = 1;
    assert(!strcmp(fsr_evidence_result(&e), "fallback"));
    puts("FSR evidence decisions passed");
    return 0;
}
