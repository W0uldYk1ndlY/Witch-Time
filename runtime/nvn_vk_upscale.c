/* Shared scene discovery and output routing; backend histories stay backend-local. */
static long long upscale_diag_run_frame[2] = {-1, -1};
static int upscale_diag_pending, upscale_diag_bound, upscale_diag_consumer;
static uint32_t upscale_diag_native_pending_w, upscale_diag_native_pending_h, upscale_diag_native_w, upscale_diag_native_h;
static void upscale_mode_changed(int before, int after) {
    static int previous_variant = -1;
    int variant = gfx_sgsr_variant();
    int variant_changed = previous_variant >= 0 && previous_variant != variant && after >= 6 && after <= 8;
    previous_variant = variant;
    int changed = upscale_scene_mode_changed(&U, before, after);
    if (!changed && !variant_changed) return;
    upscale_diag_run_frame[0] = upscale_diag_run_frame[1] = -1;
    upscale_diag_pending = upscale_diag_bound = upscale_diag_consumer = 0;
    upscale_diag_native_pending_w = upscale_diag_native_pending_h = upscale_diag_native_w = upscale_diag_native_h = 0;
    if (!changed) upscale_scene_mode_changed(&U, -1, after);
    sgsr_retire_images();
    sgsr_cs_retire_images();
    for (int i = 0; i < FI_N; i++) if (F.img[i].vt) vt_garbage(F.img[i].vt);
    memset(F.img, 0, sizeof F.img);
    F.w = F.h = F.uw = F.uh = 0;
    F.have_prev = 0; F.t_last = 0; F.res_index = 0;
    game_fsr_w = game_fsr_h = 0;
}
static int upscale_reads(VStage *fs, Tex *want) {
    for (int i = 0; fs && i < fs->nsmp; i++) {
        Tex *t = NULL; View view; SamplerParams sp;
        if (fs->smp[i].unit < 32 && (nvn_front_handle(R.tpool, R.spool, R.nvn_tex[1][fs->smp[i].unit], &t, &view, &sp) & 1) && t == want) return 1;
    }
    return 0;
}

static void upscale_draw(VStage *fs) {
    rs_scale();
    upscale_mode_changed(g_rs_mode, g_rs_mode);
    if (!fsr_want() && !sgsr_want() && !sgsr_cs_want() && !diag_is_enabled()) return;
    Tex *c = R.ncol ? R.col[0].tex : NULL, *d = R.depth.tex;
    if (c && d && !c->dead && !d->dead && c->vk && d->vk && c->b.width == d->b.width && c->b.height == d->b.height && c->b.width >= 640 && !wintex_is(c)) {
        VTex *vc = c->vk, *vd = d->vk;
        if (fsr_hdr(vc) && (vd->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && vc->samples == 1 && vd->samples == 1) {
            const Range *u = &R.ubo[0][0];
            const float *q = u->addr && u->size >= 64 * 4 ? (const float *)P(u->addr) : NULL;
            upscale_scene_observe(&U, c, vc, d, vd, q);
            return;
        }
    }
    if (!fsr_want() && !sgsr_want() && !sgsr_cs_want()) return;
    int fx = fs && R.prog[1] ? stage_fx(&R.prog[1]->st[1]) : 0;
    if ((fx == FX_GRAIN || fx == FX_DOWN) && U.depth && !U.depth->dead)
        for (int i = 0; i < fs->nsmp; i++) {
            unsigned un = fs->smp[i].unit;
            Tex *t = NULL; View view; SamplerParams sp;
            if (un >= 32 || !(nvn_front_handle(R.tpool, R.spool, R.nvn_tex[1][un], &t, &view, &sp) & 1) || !t || t->dead || !t->vk) continue;
            if (t != c && t->b.width == U.depth->b.width && t->b.height == U.depth->b.height && fsr_hdr(t->vk) && ((VTex *)t->vk)->samples == 1 && upscale_scene_matches(&U, t, t->vk, U.depth, U.depth->vk)) {
                if (sgsr_cs_want()) sgsr_cs_run(t, U.depth, fx);
                else if (sgsr_want()) sgsr_run(t, U.depth, fx);
                else fsr_run(t, U.depth, fx);
                break;
            }
        }
    if (fx == FX_GRAIN && c && !c->dead && c->vk && !((VTex *)c->vk)->post && U.col && U.output_larger && upscale_reads(fs, U.col)) rs_promote(c);
}

static void upscale_diag_dispatched(int backend) { upscale_diag_run_frame[backend] = U.frame; }
static void upscale_diag_prepare(void) { upscale_diag_pending = 0; upscale_diag_native_pending_w = upscale_diag_native_pending_h = 0; }
static void upscale_diag_sample(int stage, Tex *tex, VkImageView view) {
    if (stage != 1 || !R.prog[1] || stage_fx(&R.prog[1]->st[1]) != FX_GRAIN) return;
    if (!g_rs_mode) {
        Tex *target = R.ncol ? R.col[0].tex : NULL;
        if (U.scene_frame == U.frame && tex == U.scene_color && target && !target->dead && target->vk) {
            VTex *image = target->vk;
            upscale_diag_native_pending_w = image->w; upscale_diag_native_pending_h = image->h;
        }
        return;
    }
    if (U.ran != U.frame || tex != U.col || !U.output_larger) return;
    int variant = gfx_sgsr_variant();
    VkImageView output = VK_NULL_HANDLE;
    if (g_rs_mode >= 6 && g_rs_mode <= 8) {
        if (variant == 0 && upscale_diag_run_frame[0] == U.frame && SG.ok == 1 && SG.have_history) output = SG.image[SG_HISTORY0 + SG.history].all;
        else if (variant == 1 && upscale_diag_run_frame[1] == U.frame && SC.ok == 1 && SC.have_history) output = SC.image[SC_OUTPUT].all;
    }
    if (output && view == output) upscale_diag_bound = upscale_diag_pending = 1;
}
static void upscale_diag_draw(void) {
    if (upscale_diag_pending) upscale_diag_consumer = 1;
    if (upscale_diag_native_pending_w && upscale_diag_native_pending_h) {
        upscale_diag_native_w = upscale_diag_native_pending_w; upscale_diag_native_h = upscale_diag_native_pending_h;
    }
}

static void upscale_diag_present(void) {
    if (!diag_is_enabled()) {
        upscale_diag_pending = upscale_diag_bound = upscale_diag_consumer = 0;
        upscale_diag_native_pending_w = upscale_diag_native_pending_h = upscale_diag_native_w = upscale_diag_native_h = 0;
        return;
    }
    diag_temporal_observation_t o = {0};
    o.scene_frame = (uint64_t)nvn_front_frame_next();
    o.surface_generation = (uint32_t)g_diag_surface_gen;
    o.selected_mode = gfx_options()->upscaler;
    o.variant = g_rs_mode >= 6 && g_rs_mode <= 8 ? gfx_sgsr_variant() : -1;
    o.failing_pass = o.error_code = -1; /* Existing backend logs retain precise errors; unavailable here. */
    int eligible = U.scene_frame == U.frame && U.camera_generation == U.generation && U.generation != 0;
    if (U.scene_frame == U.frame && U.scene_color && !U.scene_color->dead && U.scene_color->vk) {
        VTex *image = U.scene_color->vk;
        o.scene_width = image->w; o.scene_height = image->h;
        if (!g_rs_mode) { o.output_width = upscale_diag_native_w; o.output_height = upscale_diag_native_h; }
    }
    int bound = 0, consumer = 0;
    if (g_rs_mode >= 1 && g_rs_mode <= 5) {
        eligible = F.evidence.eligible;
        if (F.evidence.supported) o.flags |= DIAG_TEMPORAL_SUPPORTED;
        if (F.evidence.initialized) o.flags |= DIAG_TEMPORAL_INITIALIZED;
        if (F.evidence.dispatched && F.evidence_mode == g_rs_mode) {
            o.flags |= DIAG_TEMPORAL_DISPATCHED;
            o.scene_width = F.w; o.scene_height = F.h; o.output_width = F.uw; o.output_height = F.uh;
            o.motion_support = 1; o.reset_reason = F.evidence_reset ? 1 : 0;
        }
        if (F.evidence.camera_conflict) o.fallback_reason = DIAG_TEMPORAL_CAMERA_CONFLICT;
        else if (F.evidence.input_mismatch) o.fallback_reason = DIAG_TEMPORAL_INPUT_MISMATCH;
        else if (F.evidence.output_unsupported) o.fallback_reason = DIAG_TEMPORAL_OUTPUT_UNSUPPORTED;
        else if (F.evidence.fallback) o.fallback_reason = DIAG_TEMPORAL_FALLBACK;
        bound = F.evidence.output_bound; consumer = F.evidence.draw_recorded;
    } else if (g_rs_mode >= 6 && g_rs_mode <= 8) {
        int ok = o.variant == 0 ? SG.ok : o.variant == 1 ? SC.ok : -1;
        static int support[2];
        if (o.variant >= 0 && o.variant <= 1 && !support[o.variant])
            support[o.variant] = (o.variant == 0 ? sgsr_unable() : sgsr_cs_unable()) ? -1 : 1;
        if (o.variant >= 0 && o.variant <= 1 && support[o.variant] > 0) o.flags |= DIAG_TEMPORAL_SUPPORTED;
        if (ok == 1) o.flags |= DIAG_TEMPORAL_INITIALIZED;
        if (o.variant >= 0 && o.variant <= 1 && upscale_diag_run_frame[o.variant] == U.frame && ok == 1) {
            o.flags |= DIAG_TEMPORAL_DISPATCHED;
            o.scene_width = o.variant == 0 ? SG.w : SC.w; o.scene_height = o.variant == 0 ? SG.h : SC.h;
            o.output_width = o.variant == 0 ? SG.ow : SC.ow; o.output_height = o.variant == 0 ? SG.oh : SC.oh;
            o.motion_support = 1; o.reset_reason = (o.variant == 0 ? SG.frame_reset : SC.frame_reset) ? 1 : 0;
        }
        if (U.camera_conflict && U.scene_frame == U.frame) o.fallback_reason = DIAG_TEMPORAL_CAMERA_CONFLICT;
        bound = upscale_diag_bound; consumer = upscale_diag_consumer;
    }
    if ((o.flags & DIAG_TEMPORAL_DISPATCHED) && U.have_surface)
        o.surface_generation = (uint32_t)U.surface_generation;
    diag_temporal_decide(&o, g_rs_mode, eligible, bound, consumer);
    diag_hook_temporal(&o);
    upscale_diag_pending = upscale_diag_bound = upscale_diag_consumer = 0;
    upscale_diag_native_pending_w = upscale_diag_native_pending_h = upscale_diag_native_w = upscale_diag_native_h = 0;
}
