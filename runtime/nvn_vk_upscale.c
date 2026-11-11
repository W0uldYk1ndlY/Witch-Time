/* Shared scene discovery and output routing; backend histories stay backend-local. */
static void upscale_mode_changed(int before, int after) {
    if (!upscale_scene_mode_changed(&U, before, after)) return;
    sgsr_retire_images();
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
    if (!fsr_want() && !sgsr_want()) return;
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
    int fx = fs && R.prog[1] ? stage_fx(&R.prog[1]->st[1]) : 0;
    if ((fx == FX_GRAIN || fx == FX_DOWN) && U.depth && !U.depth->dead)
        for (int i = 0; i < fs->nsmp; i++) {
            unsigned un = fs->smp[i].unit;
            Tex *t = NULL; View view; SamplerParams sp;
            if (un >= 32 || !(nvn_front_handle(R.tpool, R.spool, R.nvn_tex[1][un], &t, &view, &sp) & 1) || !t || t->dead || !t->vk) continue;
            if (t != c && t->b.width == U.depth->b.width && t->b.height == U.depth->b.height && fsr_hdr(t->vk) && ((VTex *)t->vk)->samples == 1 && upscale_scene_matches(&U, t, t->vk, U.depth, U.depth->vk)) {
                if (sgsr_want()) sgsr_run(t, U.depth, fx); else fsr_run(t, U.depth, fx);
                break;
            }
        }
    if (fx == FX_GRAIN && c && !c->dead && c->vk && !((VTex *)c->vk)->post && U.col && U.output_larger && upscale_reads(fs, U.col)) rs_promote(c);
}
