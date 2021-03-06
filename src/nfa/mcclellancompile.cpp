/*
 * Copyright (c) 2015, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "mcclellancompile.h"

#include "accel.h"
#include "grey.h"
#include "mcclellan_internal.h"
#include "nfa_internal.h"
#include "shufticompile.h"
#include "trufflecompile.h"
#include "ue2common.h"
#include "util/alloc.h"
#include "util/bitutils.h"
#include "util/charreach.h"
#include "util/compare.h"
#include "util/compile_context.h"
#include "util/container.h"
#include "util/make_unique.h"
#include "util/order_check.h"
#include "util/ue2_containers.h"
#include "util/unaligned.h"
#include "util/verify_types.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <vector>

using namespace std;

namespace ue2 {

/* compile time accel defs */
#define ACCEL_MAX_STOP_CHAR 160 /* larger than nfa, as we don't have a budget
                                   and the nfa cheats on stop characters for
                                   sets of states */
#define ACCEL_MAX_FLOATING_STOP_CHAR 192 /* accelerating sds is important */


namespace /* anon */ {

struct dstate_extra {
    u16 daddytaken;
    bool shermanState;
    bool accelerable;
    dstate_extra(void) : daddytaken(0), shermanState(false),
                         accelerable(false) {}
};

struct dfa_info {
    dfa_build_strat &strat;
    raw_dfa &raw;
    vector<dstate> &states;
    vector<dstate_extra> extra;
    const u16 alpha_size; /* including special symbols */
    const array<u16, ALPHABET_SIZE> &alpha_remap;
    const u16 impl_alpha_size;

    u8 getAlphaShift() const;

    explicit dfa_info(dfa_build_strat &s)
                                : strat(s),
                                  raw(s.get_raw()),
                                  states(raw.states),
                                  extra(raw.states.size()),
                                  alpha_size(raw.alpha_size),
                                  alpha_remap(raw.alpha_remap),
                                  impl_alpha_size(raw.getImplAlphaSize()) {}

    dstate_id_t implId(dstate_id_t raw_id) const {
        return states[raw_id].impl_id;
    }

    bool is_sherman(dstate_id_t raw_id) const {
        return extra[raw_id].shermanState;
    }

    bool is_accel(dstate_id_t raw_id) const {
        return extra[raw_id].accelerable;
    }

    size_t size(void) const { return states.size(); }
};

u8 dfa_info::getAlphaShift() const {
    if (impl_alpha_size < 2) {
        return 1;
    } else {
        /* log2 round up */
        return 32 - clz32(impl_alpha_size - 1);
    }
}

} // namespace

static
mstate_aux *getAux(NFA *n, dstate_id_t i) {
    assert(isMcClellanType(n->type));

    mcclellan *m = (mcclellan *)getMutableImplNfa(n);
    mstate_aux *aux_base = (mstate_aux *)((char *)n + m->aux_offset);

    mstate_aux *aux = aux_base + i;
    assert((const char *)aux < (const char *)n + m->length);
    return aux;
}

static
void markEdges(NFA *n, u16 *succ_table, const dfa_info &info) {
    assert((size_t)succ_table % 2 == 0);
    assert(n->type == MCCLELLAN_NFA_16);
    u8 alphaShift = info.getAlphaShift();
    u16 alphaSize = info.impl_alpha_size;
    mcclellan *m = (mcclellan *)getMutableImplNfa(n);

    /* handle the normal states */
    for (u32 i = 0; i < m->sherman_limit; i++) {
        for (size_t j = 0; j < alphaSize; j++) {
            size_t c_prime = (i << alphaShift) + j;

            mstate_aux *aux = getAux(n, succ_table[c_prime]);

            if (aux->accept) {
                succ_table[c_prime] |= ACCEPT_FLAG;
            }

            if (aux->accel_offset) {
                succ_table[c_prime] |= ACCEL_FLAG;
            }
        }
    }

    /* handle the sherman states */
    char *sherman_base_offset = (char *)n + m->sherman_offset;
    for (u16 j = m->sherman_limit; j < m->state_count; j++) {
        char *sherman_cur
            = findMutableShermanState(sherman_base_offset, m->sherman_limit, j);
        assert(*(sherman_cur + SHERMAN_TYPE_OFFSET) == SHERMAN_STATE);
        u8 len = *(u8 *)(sherman_cur + SHERMAN_LEN_OFFSET);
        u16 *succs = (u16 *)(sherman_cur + SHERMAN_STATES_OFFSET(len));

        for (u8 i = 0; i < len; i++) {
            u16 succ_i = unaligned_load_u16((u8 *)&succs[i]);
            mstate_aux *aux = getAux(n, succ_i);

            if (aux->accept) {
                succ_i |= ACCEPT_FLAG;
            }

            if (aux->accel_offset) {
                succ_i |= ACCEL_FLAG;
            }

            unaligned_store_u16((u8 *)&succs[i], succ_i);
        }
    }
}

void mcclellan_build_strat::find_escape_strings(dstate_id_t this_idx,
                                                escape_info *out) const {
    const dstate &raw = rdfa.states[this_idx];
    const auto &alpha_remap = rdfa.alpha_remap;

    flat_set<pair<u8, u8>> outs2_local;
    for (unsigned i = 0; i < N_CHARS; i++) {
        outs2_local.clear();

        if (raw.next[alpha_remap[i]] != this_idx) {
            out->outs.set(i);

            DEBUG_PRINTF("next is %hu\n", raw.next[alpha_remap[i]]);
            const dstate &raw_next = rdfa.states[raw.next[alpha_remap[i]]];

            if (!raw_next.reports.empty() && generates_callbacks(rdfa.kind)) {
                DEBUG_PRINTF("leads to report\n");
                out->outs2_broken = true;  /* cannot accelerate over reports */
            }

            for (unsigned j = 0; !out->outs2_broken && j < N_CHARS; j++) {
                if (raw_next.next[alpha_remap[j]] == raw.next[alpha_remap[j]]) {
                    continue;
                }

                DEBUG_PRINTF("adding %02x %02x -> %hu to 2 \n", i, j,
                             raw_next.next[alpha_remap[j]]);
                outs2_local.emplace((u8)i, (u8)j);
            }

            if (outs2_local.size() > 8) {
                DEBUG_PRINTF("adding %02x to outs2_single\n", i);
                out->outs2_single.set(i);
            } else {
                insert(&out->outs2, outs2_local);
            }
            if (out->outs2.size() > 8) {
                DEBUG_PRINTF("outs2 too big\n");
                out->outs2_broken = true;
            }
        }
    }
}

/** builds acceleration schemes for states */
void mcclellan_build_strat::buildAccel(dstate_id_t this_idx, void *accel_out) {
    AccelAux *accel = (AccelAux *)accel_out;
    escape_info out;

    find_escape_strings(this_idx, &out);

    if (!out.outs2_broken && out.outs2_single.none()
        && out.outs2.size() == 1) {
        accel->accel_type = ACCEL_DVERM;
        accel->dverm.c1 = out.outs2.begin()->first;
        accel->dverm.c2 = out.outs2.begin()->second;
        DEBUG_PRINTF("state %hu is double vermicelli\n", this_idx);
        return;
    }

    if (!out.outs2_broken && out.outs2_single.none()
        && (out.outs2.size() == 2 || out.outs2.size() == 4)) {
        bool ok = true;

        assert(!out.outs2.empty());
        u8 firstC = out.outs2.begin()->first & CASE_CLEAR;
        u8 secondC = out.outs2.begin()->second & CASE_CLEAR;

        for (const pair<u8, u8> &p : out.outs2) {
            if ((p.first & CASE_CLEAR) != firstC
             || (p.second & CASE_CLEAR) != secondC) {
                ok = false;
                break;
            }
        }

        if (ok) {
            accel->accel_type = ACCEL_DVERM_NOCASE;
            accel->dverm.c1 = firstC;
            accel->dverm.c2 = secondC;
            DEBUG_PRINTF("state %hu is nc double vermicelli\n", this_idx);
            return;
        }
    }

    if (!out.outs2_broken &&
        (out.outs2_single.count() + out.outs2.size()) <= 8 &&
        out.outs2_single.count() < out.outs2.size() &&
        out.outs2_single.count() <= 2 && !out.outs2.empty()) {
        accel->accel_type = ACCEL_DSHUFTI;
        shuftiBuildDoubleMasks(out.outs2_single, out.outs2,
                               &accel->dshufti.lo1,
                               &accel->dshufti.hi1,
                               &accel->dshufti.lo2,
                               &accel->dshufti.hi2);
        DEBUG_PRINTF("state %hu is double shufti\n", this_idx);
        return;
    }

    if (out.outs.none()) {
        accel->accel_type = ACCEL_RED_TAPE;
        DEBUG_PRINTF("state %hu is a dead end full of bureaucratic red tape"
                     " from which there is no escape\n", this_idx);
        return;
    }

    if (out.outs.count() == 1) {
        accel->accel_type = ACCEL_VERM;
        accel->verm.c = out.outs.find_first();
        DEBUG_PRINTF("state %hu is vermicelli\n", this_idx);
        return;
    }

    if (out.outs.count() == 2 && out.outs.isCaselessChar()) {
        accel->accel_type = ACCEL_VERM_NOCASE;
        accel->verm.c = out.outs.find_first() & CASE_CLEAR;
        DEBUG_PRINTF("state %hu is caseless vermicelli\n", this_idx);
        return;
    }

    if (out.outs.count() > ACCEL_MAX_FLOATING_STOP_CHAR) {
        accel->accel_type = ACCEL_NONE;
        DEBUG_PRINTF("state %hu is too broad\n", this_idx);
        return;
    }

    accel->accel_type = ACCEL_SHUFTI;
    if (-1 != shuftiBuildMasks(out.outs, &accel->shufti.lo,
                               &accel->shufti.hi)) {
        DEBUG_PRINTF("state %hu is shufti\n", this_idx);
        return;
    }

    assert(!out.outs.none());
    accel->accel_type = ACCEL_TRUFFLE;
    truffleBuildMasks(out.outs, &accel->truffle.mask1, &accel->truffle.mask2);
    DEBUG_PRINTF("state %hu is truffle\n", this_idx);
}

static
bool is_accel(const raw_dfa &raw, dstate_id_t sds_or_proxy,
              dstate_id_t this_idx) {
    if (!this_idx /* dead state is not accelerable */) {
        return false;
    }

    /* Note on report acceleration states: While we can't accelerate while we
     * are spamming out callbacks, the QR code paths don't raise reports
     * during scanning so they can accelerate report states. */

    if (generates_callbacks(raw.kind)
        && !raw.states[this_idx].reports.empty()) {
        return false;
    }

    size_t single_limit = this_idx == sds_or_proxy ?
                             ACCEL_MAX_FLOATING_STOP_CHAR : ACCEL_MAX_STOP_CHAR;
    DEBUG_PRINTF("inspecting %hu/%hu: %zu\n", this_idx, sds_or_proxy,
                  single_limit);

    CharReach out;
    for (u32 i = 0; i < N_CHARS; i++) {
        if (raw.states[this_idx].next[raw.alpha_remap[i]] != this_idx) {
            out.set(i);
        }
    }

    if (out.count() <= single_limit) {
        DEBUG_PRINTF("state %hu should be accelerable %zu\n", this_idx,
                     out.count());
        return true;
    }

    DEBUG_PRINTF("state %hu is not accelerable has %zu\n", this_idx,
                  out.count());

    return false;
}

static
bool has_self_loop(dstate_id_t s, const raw_dfa &raw) {
    u16 top_remap = raw.alpha_remap[TOP];
    for (u32 i = 0; i < raw.states[s].next.size(); i++) {
        if (i != top_remap && raw.states[s].next[i] == s) {
            return true;
        }
    }
    return false;
}

static
dstate_id_t get_sds_or_proxy(const raw_dfa &raw) {
    if (raw.start_floating != DEAD_STATE) {
        DEBUG_PRINTF("has floating start\n");
        return raw.start_floating;
    }

    DEBUG_PRINTF("looking for SDS proxy\n");

    dstate_id_t s = raw.start_anchored;

    if (has_self_loop(s, raw)) {
        return s;
    }

    u16 top_remap = raw.alpha_remap[TOP];

    ue2::unordered_set<dstate_id_t> seen;
    while (true) {
        seen.insert(s);
        DEBUG_PRINTF("basis %hu\n", s);

        /* check if we are connected to a state with a self loop */
        for (u32 i = 0; i < raw.states[s].next.size(); i++) {
            dstate_id_t t = raw.states[s].next[i];
            if (i != top_remap && t != DEAD_STATE && has_self_loop(t, raw)) {
                return t;
            }
        }

        /* find a neighbour to use as a basis for looking for the sds proxy */
        dstate_id_t t = DEAD_STATE;
        for (u32 i = 0; i < raw.states[s].next.size(); i++) {
            dstate_id_t tt = raw.states[s].next[i];
            if (i != top_remap && tt != DEAD_STATE && !contains(seen, tt)) {
                t = tt;
                break;
            }
        }

        if (t == DEAD_STATE) {
            /* we were unable to find a state to use as a SDS proxy */
            return DEAD_STATE;
        }

        s = t;
        seen.insert(t);
    }
}

static
void populateAccelerationInfo(dfa_info &info, u32 *ac, const Grey &grey) {
    *ac = 0; /* number of accelerable states */

    if (!grey.accelerateDFA) {
        return;
    }

    dstate_id_t sds_proxy = get_sds_or_proxy(info.raw);
    DEBUG_PRINTF("sds %hu\n", sds_proxy);

    for (size_t i = 0; i < info.size(); i++) {
        if (is_accel(info.raw, sds_proxy, i)) {
            ++*ac;
            info.extra[i].accelerable = true;
        }
    }
}

static
void populateBasicInfo(size_t state_size, const dfa_info &info,
                       u32 total_size, u32 aux_offset, u32 accel_offset,
                       u32 accel_count, ReportID arb, bool single, NFA *nfa) {
    assert(state_size == sizeof(u16) || state_size == sizeof(u8));

    nfa->length = total_size;
    nfa->nPositions = info.states.size();

    nfa->scratchStateSize = verify_u32(state_size);
    nfa->streamStateSize = verify_u32(state_size);

    if (state_size == sizeof(u8)) {
        nfa->type = MCCLELLAN_NFA_8;
    } else {
        nfa->type = MCCLELLAN_NFA_16;
    }

    mcclellan *m = (mcclellan *)getMutableImplNfa(nfa);
    for (u32 i = 0; i < 256; i++) {
        m->remap[i] = verify_u8(info.alpha_remap[i]);
    }
    m->alphaShift = info.getAlphaShift();
    m->length = total_size;
    m->aux_offset = aux_offset;
    m->accel_offset = accel_offset;
    m->arb_report = arb;
    m->state_count = verify_u16(info.size());
    m->start_anchored = info.implId(info.raw.start_anchored);
    m->start_floating = info.implId(info.raw.start_floating);
    m->has_accel = accel_count ? 1 : 0;

    if (single) {
        m->flags |= MCCLELLAN_FLAG_SINGLE;
    }
}

raw_dfa::~raw_dfa() {
}

raw_report_info::raw_report_info() {
}

raw_report_info::~raw_report_info() {
}

namespace {

struct raw_report_list {
    flat_set<ReportID> reports;

    explicit raw_report_list(const flat_set<ReportID> &reports_in)
        : reports(reports_in) {}

    bool operator<(const raw_report_list &b) const {
        return reports < b.reports;
    }
};

struct raw_report_info_impl : public raw_report_info {
    vector<raw_report_list> rl;
    u32 getReportListSize() const override;
    size_t size() const override;
    void fillReportLists(NFA *n, size_t base_offset,
                         std::vector<u32> &ro /* out */) const override;
};
}

unique_ptr<raw_report_info> mcclellan_build_strat::gatherReports(
                                                  vector<u32> &reports,
                                                  vector<u32> &reports_eod,
                                                  u8 *isSingleReport,
                                                  ReportID *arbReport) const {
    DEBUG_PRINTF("gathering reports\n");

    auto ri = ue2::make_unique<raw_report_info_impl>();
    map<raw_report_list, u32> rev;

    for (const dstate &s : rdfa.states) {
        if (s.reports.empty()) {
            reports.push_back(MO_INVALID_IDX);
            continue;
        }

        raw_report_list rrl(s.reports);
        DEBUG_PRINTF("non empty r\n");
        if (rev.find(rrl) != rev.end()) {
            reports.push_back(rev[rrl]);
        } else {
            DEBUG_PRINTF("adding to rl %zu\n", ri->size());
            rev[rrl] = ri->size();
            reports.push_back(ri->size());
            ri->rl.push_back(rrl);
        }
    }

    for (const dstate &s : rdfa.states) {
        if (s.reports_eod.empty()) {
            reports_eod.push_back(MO_INVALID_IDX);
            continue;
        }

        DEBUG_PRINTF("non empty r eod\n");
        raw_report_list rrl(s.reports_eod);
        if (rev.find(rrl) != rev.end()) {
            reports_eod.push_back(rev[rrl]);
            continue;
        }

        DEBUG_PRINTF("adding to rl eod %zu\n", s.reports_eod.size());
        rev[rrl] = ri->size();
        reports_eod.push_back(ri->size());
        ri->rl.push_back(rrl);
    }

    assert(!ri->rl.empty()); /* all components should be able to generate
                                reports */
    if (!ri->rl.empty()) {
        *arbReport = *ri->rl.begin()->reports.begin();
    } else {
        *arbReport = 0;
    }


    /* if we have only a single report id generated from all accepts (not eod)
     * we can take some short cuts */
    set<ReportID> reps;

    for (u32 rl_index : reports) {
        if (rl_index == MO_INVALID_IDX) {
            continue;
        }
        assert(rl_index < ri->size());
        insert(&reps, ri->rl[rl_index].reports);
    }

    if (reps.size() == 1) {
        *isSingleReport = 1;
        *arbReport = *reps.begin();
        DEBUG_PRINTF("single -- %u\n",  *arbReport);
    } else {
        *isSingleReport = 0;
    }

    return move(ri);
}

u32 raw_report_info_impl::getReportListSize() const {
    u32 rv = 0;

    for (const auto &reps : rl) {
        rv += sizeof(report_list);
        rv += sizeof(ReportID) * reps.reports.size();
    }

    return rv;
}

size_t raw_report_info_impl::size() const {
    return rl.size();
}

void raw_report_info_impl::fillReportLists(NFA *n, size_t base_offset,
                                           vector<u32> &ro) const {
    for (const auto &reps : rl) {
        ro.push_back(base_offset);

        report_list *p = (report_list *)((char *)n + base_offset);

        u32 i = 0;
        for (const ReportID report : reps.reports) {
            p->report[i++] = report;
        }
        p->count = verify_u32(reps.reports.size());

        base_offset += sizeof(report_list);
        base_offset += sizeof(ReportID) * reps.reports.size();
    }
}

static
size_t calcShermanRegionSize(const dfa_info &info) {
    size_t rv = 0;

    for (size_t i = 0; i < info.size(); i++) {
        if (info.is_sherman(i)) {
            rv += SHERMAN_FIXED_SIZE;
        }
    }

    return ROUNDUP_16(rv);
}

static
void fillInAux(mstate_aux *aux, dstate_id_t i, const dfa_info &info,
               const vector<u32> &reports, const vector<u32> &reports_eod,
               vector<u32> &reportOffsets) {
    const dstate &raw_state = info.states[i];
    aux->accept = raw_state.reports.empty() ? 0 : reportOffsets[reports[i]];
    aux->accept_eod = raw_state.reports_eod.empty() ? 0
                                              : reportOffsets[reports_eod[i]];
    aux->top = info.implId(i ? raw_state.next[info.alpha_remap[TOP]]
                             : info.raw.start_floating);
}

/* returns non-zero on error */
static
int allocateFSN16(dfa_info &info, dstate_id_t *sherman_base) {
    info.states[0].impl_id = 0; /* dead is always 0 */

    vector<dstate_id_t> norm;
    vector<dstate_id_t> sherm;

    if (info.size() > (1 << 16)) {
        DEBUG_PRINTF("too many states\n");
        *sherman_base = 0;
        return 1;
    }

    for (u32 i = 1; i < info.size(); i++) {
        if (info.is_sherman(i)) {
            sherm.push_back(i);
        } else {
            norm.push_back(i);
        }
    }

    dstate_id_t next_norm = 1;
    for (const dstate_id_t &s : norm) {
        info.states[s].impl_id = next_norm++;
    }

    *sherman_base = next_norm;
    dstate_id_t next_sherman = next_norm;

    for (const dstate_id_t &s : sherm) {
        info.states[s].impl_id = next_sherman++;
    }

    /* Check to see if we haven't over allocated our states */
    DEBUG_PRINTF("next sherman %u masked %u\n", next_sherman,
                 (dstate_id_t)(next_sherman & STATE_MASK));
    return (next_sherman - 1) != ((next_sherman - 1) & STATE_MASK);
}

static
aligned_unique_ptr<NFA> mcclellanCompile16(dfa_info &info,
                                           const CompileContext &cc) {
    DEBUG_PRINTF("building mcclellan 16\n");

    vector<u32> reports; /* index in ri for the appropriate report list */
    vector<u32> reports_eod; /* as above */
    ReportID arb;
    u8 single;
    u32 accelCount;

    u8 alphaShift = info.getAlphaShift();
    assert(alphaShift <= 8);

    u16 count_real_states;
    if (allocateFSN16(info, &count_real_states)) {
        DEBUG_PRINTF("failed to allocate state numbers, %zu states total\n",
                     info.size());
        return nullptr;
    }

    unique_ptr<raw_report_info> ri
        = info.strat.gatherReports(reports, reports_eod, &single, &arb);
    populateAccelerationInfo(info, &accelCount, cc.grey);

    size_t tran_size = (1 << info.getAlphaShift())
        * sizeof(u16) * count_real_states;

    size_t aux_size = sizeof(mstate_aux) * info.size();

    size_t aux_offset = ROUNDUP_16(sizeof(NFA) + sizeof(mcclellan) + tran_size);
    size_t accel_size = info.strat.accelSize() * accelCount;
    size_t accel_offset = ROUNDUP_N(aux_offset + aux_size
                                    + ri->getReportListSize(), 32);
    size_t sherman_offset = ROUNDUP_16(accel_offset + accel_size);
    size_t sherman_size = calcShermanRegionSize(info);

    size_t total_size = sherman_offset + sherman_size;

    accel_offset -= sizeof(NFA); /* adj accel offset to be relative to m */
    assert(ISALIGNED_N(accel_offset, alignof(union AccelAux)));

    aligned_unique_ptr<NFA> nfa = aligned_zmalloc_unique<NFA>(total_size);
    char *nfa_base = (char *)nfa.get();

    populateBasicInfo(sizeof(u16), info, total_size, aux_offset, accel_offset,
                      accelCount, arb, single, nfa.get());

    vector<u32> reportOffsets;

    ri->fillReportLists(nfa.get(), aux_offset + aux_size, reportOffsets);

    u16 *succ_table = (u16 *)(nfa_base + sizeof(NFA) + sizeof(mcclellan));
    mstate_aux *aux = (mstate_aux *)(nfa_base + aux_offset);
    mcclellan *m = (mcclellan *)getMutableImplNfa(nfa.get());

    /* copy in the mc header information */
    m->sherman_offset = sherman_offset;
    m->sherman_end = total_size;
    m->sherman_limit = count_real_states;

    /* do normal states */
    for (size_t i = 0; i < info.size(); i++) {
        if (info.is_sherman(i)) {
            continue;
        }

        u16 fs = info.implId(i);
        mstate_aux *this_aux = getAux(nfa.get(), fs);

        assert(fs < count_real_states);

        for (size_t j = 0; j < info.impl_alpha_size; j++) {
            succ_table[(fs << alphaShift) + j] =
                info.implId(info.states[i].next[j]);
        }

        fillInAux(&aux[fs], i, info, reports, reports_eod, reportOffsets);

        if (info.is_accel(i)) {
            this_aux->accel_offset = accel_offset;
            accel_offset += info.strat.accelSize();
            assert(accel_offset + sizeof(NFA) <= sherman_offset);
            assert(ISALIGNED_N(accel_offset, alignof(union AccelAux)));
            info.strat.buildAccel(i,
                                  (void *)((char *)m + this_aux->accel_offset));
        }
    }

    /* do sherman states */
    char *sherman_table = nfa_base + m->sherman_offset;
    assert(ISALIGNED_16(sherman_table));
    for (size_t i = 0; i < info.size(); i++) {
        if (!info.is_sherman(i)) {
            continue;
        }

        u16 fs = verify_u16(info.implId(i));
        mstate_aux *this_aux = getAux(nfa.get(), fs);

        assert(fs >= count_real_states);

        char *curr_sherman_entry
            = sherman_table + (fs - m->sherman_limit) * SHERMAN_FIXED_SIZE;
        assert(curr_sherman_entry <= nfa_base + m->length);

        fillInAux(this_aux, i, info, reports, reports_eod, reportOffsets);

        if (info.is_accel(i)) {
            this_aux->accel_offset = accel_offset;
            accel_offset += info.strat.accelSize();
            assert(accel_offset + sizeof(NFA) <= sherman_offset);
            assert(ISALIGNED_N(accel_offset, alignof(union AccelAux)));
            info.strat.buildAccel(i,
                                  (void *)((char *)m + this_aux->accel_offset));
        }

        u8 len = verify_u8(info.impl_alpha_size - info.extra[i].daddytaken);
        assert(len <= 9);
        dstate_id_t d = info.states[i].daddy;

        *(u8 *)(curr_sherman_entry + SHERMAN_TYPE_OFFSET) = SHERMAN_STATE;
        *(u8 *)(curr_sherman_entry + SHERMAN_LEN_OFFSET) = len;
        *(u16 *)(curr_sherman_entry + SHERMAN_DADDY_OFFSET) = info.implId(d);
        u8 *chars = (u8 *)(curr_sherman_entry + SHERMAN_CHARS_OFFSET);

        for (u16 s = 0; s < info.impl_alpha_size; s++) {
            if (info.states[i].next[s] != info.states[d].next[s]) {
                *(chars++) = (u8)s;
            }
        }

        u16 *states = (u16 *)(curr_sherman_entry + SHERMAN_STATES_OFFSET(len));
        for (u16 s = 0; s < info.impl_alpha_size; s++) {
            if (info.states[i].next[s] != info.states[d].next[s]) {
                DEBUG_PRINTF("s overrider %hu dad %hu char next %hu\n",
                             fs, info.implId(d),
                             info.implId(info.states[i].next[s]));
                unaligned_store_u16((u8 *)states++,
                                    info.implId(info.states[i].next[s]));
            }
        }
    }

    markEdges(nfa.get(), succ_table, info);

    return nfa;
}

static
void fillInBasicState8(const dfa_info &info, mstate_aux *aux, u8 *succ_table,
                       const vector<u32> &reportOffsets,
                       const vector<u32> &reports,
                       const vector<u32> &reports_eod, u32 i) {
    dstate_id_t j = info.implId(i);
    u8 alphaShift = info.getAlphaShift();
    assert(alphaShift <= 8);

    for (size_t s = 0; s < info.impl_alpha_size; s++) {
        dstate_id_t raw_succ = info.states[i].next[s];
        succ_table[(j << alphaShift) + s] = info.implId(raw_succ);
    }

    aux[j].accept = 0;
    aux[j].accept_eod = 0;

    if (!info.states[i].reports.empty()) {
        DEBUG_PRINTF("i=%u r[i]=%u\n", i, reports[i]);
        assert(reports[i] != MO_INVALID_IDX);
        aux[j].accept = reportOffsets[reports[i]];
    }

    if (!info.states[i].reports_eod.empty()) {
        DEBUG_PRINTF("i=%u re[i]=%u\n", i, reports_eod[i]);
        aux[j].accept_eod = reportOffsets[reports_eod[i]];
    }

    dstate_id_t raw_top = i ? info.states[i].next[info.alpha_remap[TOP]]
                            : info.raw.start_floating;

    aux[j].top = info.implId(raw_top);
}

static
void allocateFSN8(dfa_info &info, u16 *accel_limit, u16 *accept_limit) {
    info.states[0].impl_id = 0; /* dead is always 0 */

    vector<dstate_id_t> norm;
    vector<dstate_id_t> accel;
    vector<dstate_id_t> accept;

    assert(info.size() <= (1 << 8));

    for (u32 i = 1; i < info.size(); i++) {
        if (!info.states[i].reports.empty()) {
            accept.push_back(i);
        } else if (info.is_accel(i)) {
            accel.push_back(i);
        } else {
            norm.push_back(i);
        }
    }

    u32 j = 1; /* dead is already at 0 */
    for (const dstate_id_t &s : norm) {
        assert(j <= 256);
        DEBUG_PRINTF("mapping state %u to %u\n", s, j);
        info.states[s].impl_id = j++;
    }
    *accel_limit = j;
    for (const dstate_id_t &s : accel) {
        assert(j <= 256);
        DEBUG_PRINTF("mapping state %u to %u\n", s, j);
        info.states[s].impl_id = j++;
    }
    *accept_limit = j;
    for (const dstate_id_t &s : accept) {
        assert(j <= 256);
        DEBUG_PRINTF("mapping state %u to %u\n",  s, j);
        info.states[s].impl_id = j++;
    }
}

static
aligned_unique_ptr<NFA> mcclellanCompile8(dfa_info &info,
                                          const CompileContext &cc) {
    DEBUG_PRINTF("building mcclellan 8\n");

    vector<u32> reports;
    vector<u32> reports_eod;
    ReportID arb;
    u8 single;
    u32 accelCount;

    unique_ptr<raw_report_info> ri
        = info.strat.gatherReports(reports, reports_eod, &single, &arb);
    populateAccelerationInfo(info, &accelCount, cc.grey);

    size_t tran_size = sizeof(u8) * (1 << info.getAlphaShift()) * info.size();
    size_t aux_size = sizeof(mstate_aux) * info.size();
    size_t aux_offset = ROUNDUP_16(sizeof(NFA) + sizeof(mcclellan) + tran_size);
    size_t accel_size = info.strat.accelSize() * accelCount;
    size_t accel_offset = ROUNDUP_N(aux_offset + aux_size
                                     + ri->getReportListSize(), 32);
    size_t total_size = accel_offset + accel_size;

    DEBUG_PRINTF("aux_size %zu\n", aux_size);
    DEBUG_PRINTF("aux_offset %zu\n", aux_offset);
    DEBUG_PRINTF("rl size %u\n", ri->getReportListSize());
    DEBUG_PRINTF("accel_size %zu\n", accel_size);
    DEBUG_PRINTF("accel_offset %zu\n", accel_offset);
    DEBUG_PRINTF("total_size %zu\n", total_size);

    accel_offset -= sizeof(NFA); /* adj accel offset to be relative to m */
    assert(ISALIGNED_N(accel_offset, alignof(union AccelAux)));

    aligned_unique_ptr<NFA> nfa = aligned_zmalloc_unique<NFA>(total_size);
    char *nfa_base = (char *)nfa.get();

    mcclellan *m = (mcclellan *)getMutableImplNfa(nfa.get());

    allocateFSN8(info, &m->accel_limit_8, &m->accept_limit_8);
    populateBasicInfo(sizeof(u8), info, total_size, aux_offset, accel_offset,
                      accelCount, arb, single, nfa.get());

    vector<u32> reportOffsets;

    ri->fillReportLists(nfa.get(), aux_offset + aux_size, reportOffsets);

    /* copy in the state information */
    u8 *succ_table = (u8 *)(nfa_base + sizeof(NFA) + sizeof(mcclellan));
    mstate_aux *aux = (mstate_aux *)(nfa_base + aux_offset);

    for (size_t i = 0; i < info.size(); i++) {
        if (info.is_accel(i)) {
            u32 j = info.implId(i);

            aux[j].accel_offset = accel_offset;
            accel_offset += info.strat.accelSize();

            info.strat.buildAccel(i, (void *)((char *)m + aux[j].accel_offset));
        }

        fillInBasicState8(info, aux, succ_table, reportOffsets, reports,
                          reports_eod, i);
    }

    assert(accel_offset + sizeof(NFA) <= total_size);

    DEBUG_PRINTF("rl size %zu\n", ri->size());

    return nfa;
}

#define MAX_SHERMAN_LIST_LEN 8

static
void addIfEarlier(set<dstate_id_t> &dest, dstate_id_t candidate,
                  dstate_id_t max) {
    if (candidate < max) {
        dest.insert(candidate);
    }
}

static
void addSuccessors(set<dstate_id_t> &dest, const dstate &source,
                   u16 alphasize, dstate_id_t curr_id) {
    for (symbol_t s = 0; s < alphasize; s++) {
        addIfEarlier(dest, source.next[s], curr_id);
    }
}

#define MAX_SHERMAN_SELF_LOOP 20

static
void find_better_daddy(dfa_info &info, dstate_id_t curr_id,
                       bool using8bit, bool any_cyclic_near_anchored_state,
                       const Grey &grey) {
    if (!grey.allowShermanStates) {
        return;
    }

    const u16 width = using8bit ? sizeof(u8) : sizeof(u16);
    const u16 alphasize = info.impl_alpha_size;

    if (info.raw.start_anchored != DEAD_STATE
        && any_cyclic_near_anchored_state
        && curr_id < alphasize * 3) {
        /* crude attempt to prevent frequent states from being sherman'ed
         * depends on the fact that states are numbers are currently in bfs
         * order */
        DEBUG_PRINTF("%hu is banned\n", curr_id);
        return;
    }

    if (info.raw.start_floating != DEAD_STATE
        && curr_id >= info.raw.start_floating
        && curr_id < info.raw.start_floating + alphasize * 3) {
        /* crude attempt to prevent frequent states from being sherman'ed
         * depends on the fact that states are numbers are currently in bfs
         * order */
        DEBUG_PRINTF("%hu is banned (%hu)\n", curr_id, info.raw.start_floating);
        return;
    }

    const u16 full_state_size = width * alphasize;
    const u16 max_list_len = MIN(MAX_SHERMAN_LIST_LEN,
                           (full_state_size - 2)/(width + 1));
    u16 best_score = 0;
    dstate_id_t best_daddy = 0;
    dstate &currState = info.states[curr_id];

    set<dstate_id_t> hinted; /* set of states to search for a better daddy */
    addIfEarlier(hinted, 0, curr_id);
    addIfEarlier(hinted, info.raw.start_anchored, curr_id);
    addIfEarlier(hinted, info.raw.start_floating, curr_id);

    dstate_id_t mydaddy = currState.daddy;
    if (mydaddy) {
        addIfEarlier(hinted, mydaddy, curr_id);
        addSuccessors(hinted, info.states[mydaddy], alphasize, curr_id);
        dstate_id_t mygranddaddy = info.states[mydaddy].daddy;
        if (mygranddaddy) {
            addIfEarlier(hinted, mygranddaddy, curr_id);
            addSuccessors(hinted, info.states[mygranddaddy], alphasize,
                          curr_id);
        }
    }

    for (const dstate_id_t &donor : hinted) {
        assert(donor < curr_id);
        u32 score = 0;

        if (info.is_sherman(donor)) {
            continue;
        }

        const dstate &donorState = info.states[donor];
        for (symbol_t s = 0; s < alphasize; s++) {
            if (currState.next[s] == donorState.next[s]) {
                score++;
            }
        }

        /* prefer lower ids to provide some stability amongst potential
         * siblings */
        if (score > best_score || (score == best_score && donor < best_daddy)) {
            best_daddy = donor;
            best_score = score;

            if (score == alphasize) {
                break;
            }
        }
    }

    currState.daddy = best_daddy;
    info.extra[curr_id].daddytaken = best_score;
    DEBUG_PRINTF("%hu -> daddy %hu: %u/%u BF\n", curr_id, best_daddy,
                 best_score, alphasize);

    if (best_score + max_list_len < alphasize) {
        return; /* ??? */
    }

    if (info.is_sherman(currState.daddy)) {
        return;
    }

    u32 self_loop_width = 0;
    const dstate curr_raw = info.states[curr_id];
    for (unsigned i = 0; i < N_CHARS; i++) {
        if (curr_raw.next[info.alpha_remap[i]] == curr_id) {
            self_loop_width++;
        }
    }

    if (self_loop_width > MAX_SHERMAN_SELF_LOOP) {
        DEBUG_PRINTF("%hu is banned wide self loop (%u)\n", curr_id,
                      self_loop_width);
        return;
    }

    DEBUG_PRINTF("%hu is sherman\n", curr_id);
    info.extra[curr_id].shermanState = true;
}

/*
 * Calls accessible outside this module.
 */

u16 raw_dfa::getImplAlphaSize() const {
    return alpha_size - N_SPECIAL_SYMBOL;
}

void raw_dfa::stripExtraEodReports(void) {
    /* if a state generates a given report as a normal accept - then it does
     * not also need to generate an eod report for it */
    for (dstate &ds : states) {
        for (const ReportID &report : ds.reports) {
            ds.reports_eod.erase(report);
        }
    }
}

bool raw_dfa::hasEodReports(void) const {
    for (const dstate &ds : states) {
        if (!ds.reports_eod.empty()) {
            return true;
        }
    }
    return false;
}

static
bool is_cyclic_near(const raw_dfa &raw, dstate_id_t root) {
    symbol_t alphasize = raw.getImplAlphaSize();
    for (symbol_t s = 0; s < alphasize; s++) {
        dstate_id_t succ_id = raw.states[root].next[s];
        if (succ_id == DEAD_STATE) {
            continue;
        }

        const dstate &succ = raw.states[succ_id];
        for (symbol_t t = 0; t < alphasize; t++) {
            if (succ.next[t] == root || succ.next[t] == succ_id) {
                return true;
            }
        }
    }
    return false;
}

static
void fillAccelOut(const dfa_info &info, set<dstate_id_t> *accel_states) {
    for (size_t i = 0; i < info.size(); i++) {
        if (info.is_accel(i)) {
            accel_states->insert(i);
        }
    }
}

aligned_unique_ptr<NFA> mcclellanCompile_i(raw_dfa &raw, dfa_build_strat &strat,
                                           const CompileContext &cc,
                                           set<dstate_id_t> *accel_states) {
    u16 total_daddy = 0;
    dfa_info info(strat);
    bool using8bit = cc.grey.allowMcClellan8 && info.size() <= 256;

    if (!cc.streaming) { /* TODO: work out if we can do the strip in streaming
                          * mode with our semantics */
        raw.stripExtraEodReports();
    }

    bool has_eod_reports = raw.hasEodReports();
    bool any_cyclic_near_anchored_state = is_cyclic_near(raw,
                                                         raw.start_anchored);

    for (u32 i = 0; i < info.size(); i++) {
        find_better_daddy(info, i, using8bit, any_cyclic_near_anchored_state,
                          cc.grey);
        total_daddy += info.extra[i].daddytaken;
    }

    DEBUG_PRINTF("daddy %hu/%zu states=%zu alpha=%hu\n", total_daddy,
                 info.size() * info.impl_alpha_size, info.size(),
                 info.impl_alpha_size);

    aligned_unique_ptr<NFA> nfa;
    if (!using8bit) {
        nfa = mcclellanCompile16(info, cc);
    } else {
        nfa = mcclellanCompile8(info, cc);
    }

    if (has_eod_reports) {
        nfa->flags |= NFA_ACCEPTS_EOD;
    }

    if (accel_states && nfa) {
        fillAccelOut(info, accel_states);
    }

    DEBUG_PRINTF("compile done\n");
    return nfa;
}

aligned_unique_ptr<NFA> mcclellanCompile(raw_dfa &raw, const CompileContext &cc,
                                         set<dstate_id_t> *accel_states) {
    mcclellan_build_strat mbs(raw);
    return mcclellanCompile_i(raw, mbs, cc, accel_states);
}

size_t mcclellan_build_strat::accelSize(void) const {
    return sizeof(AccelAux); /* McClellan accel structures are just bare
                              * accelaux */
}

u32 mcclellanStartReachSize(const raw_dfa *raw) {
    if (raw->states.size() < 2) {
        return 0;
    }

    const dstate &ds = raw->states[raw->start_anchored];

    CharReach out;
    for (unsigned i = 0; i < N_CHARS; i++) {
        if (ds.next[raw->alpha_remap[i]] != DEAD_STATE) {
            out.set(i);
        }
    }

    return out.count();
}

bool has_accel_dfa(const NFA *nfa) {
    const mcclellan *m = (const mcclellan *)getImplNfa(nfa);
    return m->has_accel;
}

dfa_build_strat::~dfa_build_strat() {
}

} // namespace ue2
