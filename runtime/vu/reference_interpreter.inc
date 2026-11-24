// --------------------------------------------------------------- micro mode
// Interprets from `pc` until the program ends.
static void vu_interp(VUState *vu, uint32_t pc) {
    uint32_t start_pc = pc;
    Ctx c;
    c.vu = vu;
    bool in_delay = false;
    uint32_t delay_target = 0;
    int end_countdown = -1;
    uint64_t guard = 0;
    vu->running = 1;
    vu->flag_pipe = 1;
    LOG(LOG_VU, "VU%d start at %04x", vu->index, pc);
    for (;;) {
        if (++guard > 50000000) {
            WARN("VU%d: program at %04x does not terminate", vu->index, start_pc);
            break;
        }
        uint32_t lo, hi;
        memcpy(&lo, vu->micro + pc, 4);
        memcpy(&hi, vu->micro + pc + 4, 4);
        DecPair &d = g_dec[vu->index][pc >> 3];
        if (!d.valid || d.lo != lo || d.hi != hi) decode_pair(d, lo, hi);
        // stall until every register this pair reads has been written
        Scoreboard &sb = g_score[vu->index];
        if (d.use_lo | d.use_hi) {
            for (uint64_t dd = 1; dd <= 3; dd++) {
                uint64_t ic = vu->cycle - dd;
                int sl = (int)(ic & 3);
                if (sb.stamp[sl] == ic && ((sb.lo[sl] & d.use_lo) | (sb.hi[sl] & d.use_hi))) {
                    vu->cycle = ic + 4;
                    break;
                }
            }
        }
        if (d.waitq) flush_q(vu);
        sync_pipes(vu);
        vu_flag_sync(vu);
        const uint64_t lw_lo = d.lw_lo, lw_hi = d.lw_hi;
        // upper first (computed against old state), committed after lower
        c.up_write = false;
        c.mac_set = false;
        c.branch = false;
        d.up(c, hi);
        bool up_vf = c.up_write && !c.up_acc;
        int up_reg = c.up_reg, up_dest = c.up_dest;
        if (hi & 0x80000000u) {
            // I bit: lower word is a float immediate for the I register
            commit_upper(c);
            memcpy(&vu->i, &lo, 4);
        } else {
            lower(c, lo, pc);
            commit_upper(c);
        }
        {
            uint64_t wlo = lw_lo, whi = lw_hi;
            if (up_vf) reg_bits(up_reg, up_dest, wlo, whi);
            if (wlo | whi) {
                int sl = (int)(vu->cycle & 3);
                if (sb.stamp[sl] != vu->cycle) {
                    sb.stamp[sl] = vu->cycle;
                    sb.lo[sl] = sb.hi[sl] = 0;
                }
                sb.lo[sl] |= wlo;
                sb.hi[sl] |= whi;
            }
        }
        vu->cycle++;
        if (hi & 0x40000000u) end_countdown = 1;  // E bit: one more pair
        if (hi & 0x08000000u) { /* T bit: debug */ }
        if (hi & 0x10000000u) { /* D bit: debug */ }
        uint32_t next = (pc + 8) & vu->micro_mask;
        if (in_delay) {
            next = delay_target & vu->micro_mask;
            in_delay = false;
        }
        if (c.branch) {
            in_delay = true;
            delay_target = c.branch_target;
        }
        if (end_countdown == 0) break;
        if (end_countdown > 0) end_countdown--;
        pc = next;
    }
    flush_q(vu);
    flush_p(vu);
    vu_flag_flush(vu);
    vu->flag_pipe = 0;
    vu->tpc = pc;
    vu->running = 0;
}

