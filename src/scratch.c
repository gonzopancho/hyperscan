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

/** \file
 * \brief Functions for allocating and manipulating scratch space.
 */

#include <stdlib.h>
#include <string.h>

#include "allocator.h"
#include "hs_internal.h"
#include "hs_runtime.h"
#include "scratch.h"
#include "state.h"
#include "ue2common.h"
#include "database.h"
#include "nfa/limex_context.h" // for NFAContext128 etc
#include "nfa/nfa_api_queue.h"
#include "sidecar/sidecar.h"
#include "rose/rose_internal.h"
#include "util/fatbit.h"
#include "util/multibit.h"

/** Used by hs_alloc_scratch and hs_clone_scratch to allocate a complete
 * scratch region from a prototype structure. */
static
hs_error_t alloc_scratch(const hs_scratch_t *proto, hs_scratch_t **scratch) {
    u32 queueCount = proto->queueCount;
    u32 deduperCount = proto->deduper.log_size;
    u32 bStateSize = proto->bStateSize;
    u32 tStateSize = proto->tStateSize;
    u32 fullStateSize = proto->fullStateSize;
    u32 anchored_region_len = proto->anchored_region_len;
    u32 anchored_region_width = proto->anchored_region_width;
    u32 anchored_literal_region_len = proto->anchored_literal_region_len;
    u32 anchored_literal_region_width = proto->anchored_literal_count;

    u32 som_store_size = proto->som_store_count * sizeof(u64a);
    u32 som_attempted_store_size = proto->som_store_count * sizeof(u64a);
    u32 som_now_size = fatbit_size(proto->som_store_count);
    u32 som_attempted_size = fatbit_size(proto->som_store_count);

    struct hs_scratch *s;
    struct hs_scratch *s_tmp;
    size_t queue_size = queueCount * sizeof(struct mq);
    size_t qmpq_size = queueCount * sizeof(struct queue_match);

    assert(anchored_region_len < 8 * sizeof(s->am_log_sum));
    assert(anchored_literal_region_len < 8 * sizeof(s->am_log_sum));

    size_t anchored_region_size = anchored_region_len
        * (mmbit_size(anchored_region_width) + sizeof(u8 *));
    anchored_region_size = ROUNDUP_N(anchored_region_size, 8);

    size_t anchored_literal_region_size = anchored_literal_region_len
        * (mmbit_size(anchored_literal_region_width) + sizeof(u8 *));
    anchored_literal_region_size = ROUNDUP_N(anchored_literal_region_size, 8);

    size_t delay_size = mmbit_size(proto->delay_count) * DELAY_SLOT_COUNT;

    size_t nfa_context_size = 2 * sizeof(struct NFAContext512) + 127;

    // the size is all the allocated stuff, not including the struct itself
    size_t size = queue_size + 63
                  + bStateSize + tStateSize
                  + fullStateSize + 63 /* cacheline padding */
                  + nfa_context_size
                  + fatbit_size(proto->roleCount) /* handled roles */
                  + fatbit_size(queueCount) /* active queue array */
                  + 2 * fatbit_size(deduperCount) /* need odd and even logs */
                  + 2 * fatbit_size(deduperCount) /* ditto som logs */
                  + 2 * sizeof(u64a) * deduperCount /* start offsets for som */
                  + anchored_region_size
                  + anchored_literal_region_size + qmpq_size + delay_size
                  + som_store_size
                  + som_now_size
                  + som_attempted_size
                  + som_attempted_store_size
                  + proto->sideScratchSize + 15;

    /* the struct plus the allocated stuff plus padding for cacheline
     * alignment */
    const size_t alloc_size = sizeof(struct hs_scratch) + size + 256;
    s_tmp = hs_scratch_alloc(alloc_size);
    hs_error_t err = hs_check_alloc(s_tmp);
    if (err != HS_SUCCESS) {
        hs_scratch_free(s_tmp);
        *scratch = NULL;
        return err;
    }

    memset(s_tmp, 0, alloc_size);
    s = ROUNDUP_PTR(s_tmp, 64);
    DEBUG_PRINTF("allocated %zu bytes at %p but realigning to %p\n", alloc_size, s_tmp, s);
    DEBUG_PRINTF("sizeof %zu\n", sizeof(struct hs_scratch));
    *s = *proto;

    s->magic = SCRATCH_MAGIC;
    s->scratchSize = alloc_size;
    s->scratch_alloc = (char *)s_tmp;

    // each of these is at an offset from the previous
    char *current = (char *)s + sizeof(*s);

    // align current so that the following arrays are naturally aligned: this
    // is accounted for in the padding allocated
    current = ROUNDUP_PTR(current, 8);

    s->queues = (struct mq *)current;
    current += queue_size;

    assert(ISALIGNED_N(current, 8));
    s->som_store = (u64a *)current;
    current += som_store_size;

    s->som_attempted_store = (u64a *)current;
    current += som_attempted_store_size;

    s->delay_slots = (u8 *)current;
    current += delay_size;

    current = ROUNDUP_PTR(current, 8);
    s->am_log = (u8 **)current;
    current += sizeof(u8 *) * anchored_region_len;
    for (u32 i = 0; i < anchored_region_len; i++) {
        s->am_log[i] = (u8 *)current;
        current += mmbit_size(anchored_region_width);
    }

    current = ROUNDUP_PTR(current, 8);
    s->al_log = (u8 **)current;
    current += sizeof(u8 *) * anchored_literal_region_len;
    for (u32 i = 0; i < anchored_literal_region_len; i++) {
        s->al_log[i] = (u8 *)current;
        current += mmbit_size(anchored_literal_region_width);
    }

    current = ROUNDUP_PTR(current, 8);
    s->catchup_pq.qm = (struct queue_match *)current;
    current += qmpq_size;

    s->bstate = (char *)current;
    s->bStateSize = bStateSize;
    current += bStateSize;

    s->tstate = (char *)current;
    s->tStateSize = tStateSize;
    current += tStateSize;

    current = ROUNDUP_PTR(current, 64);
    assert(ISALIGNED_CL(current));
    s->nfaContext = current;
    current += sizeof(struct NFAContext512);
    current = ROUNDUP_PTR(current, 64);
    assert(ISALIGNED_CL(current));
    s->nfaContextSom = current;
    current += sizeof(struct NFAContext512);

    assert(ISALIGNED_N(current, 8));
    s->deduper.som_start_log[0] = (u64a *)current;
    current += sizeof(u64a) * deduperCount;

    s->deduper.som_start_log[1] = (u64a *)current;
    current += sizeof(u64a) * deduperCount;

    assert(ISALIGNED_N(current, 8));
    s->aqa = (struct fatbit *)current;
    current += fatbit_size(queueCount);

    s->handled_roles = (struct fatbit *)current;
    current += fatbit_size(proto->roleCount);

    s->deduper.log[0] = (struct fatbit *)current;
    current += fatbit_size(deduperCount);

    s->deduper.log[1] = (struct fatbit *)current;
    current += fatbit_size(deduperCount);

    s->deduper.som_log[0] = (struct fatbit *)current;
    current += fatbit_size(deduperCount);

    s->deduper.som_log[1] = (struct fatbit *)current;
    current += fatbit_size(deduperCount);

    s->som_set_now = (struct fatbit *)current;
    current += som_now_size;

    s->som_attempted_set = (struct fatbit *)current;
    current += som_attempted_size;

    current = ROUNDUP_PTR(current, 16);
    s->side_scratch = (void *)current;
    current += proto->sideScratchSize;

    current = ROUNDUP_PTR(current, 64);
    assert(ISALIGNED_CL(current));
    s->fullState = (char *)current;
    s->fullStateSize = fullStateSize;
    current += fullStateSize;

    *scratch = s;

    // Don't get too big for your boots
    assert((size_t)(current - (char *)s) <= alloc_size);

    // Init q->scratch ptr for every queue.
    for (struct mq *qi = s->queues; qi != s->queues + queueCount; ++qi) {
        qi->scratch = s;
    }

    return HS_SUCCESS;
}

HS_PUBLIC_API
hs_error_t hs_alloc_scratch(const hs_database_t *db, hs_scratch_t **scratch) {
    if (!db || !scratch) {
        return HS_INVALID;
    }

    /* We need to do some real sanity checks on the database as some users mmap
     * in old deserialised databases, so this is the first real opportunity we
     * have to make sure it is sane.
     */
    hs_error_t rv = dbIsValid(db);
    if (rv != HS_SUCCESS) {
        return rv;
    }

    /* We can also sanity-check the scratch parameter: if it points to an
     * existing scratch area, that scratch should have valid magic bits. */
    if (*scratch != NULL) {
        /* has to be aligned before we can do anything with it */
        if (!ISALIGNED_CL(*scratch)) {
            return HS_INVALID;
        }
        if ((*scratch)->magic != SCRATCH_MAGIC) {
            return HS_INVALID;
        }
    }

    const struct RoseEngine *rose = hs_get_bytecode(db);
    int resize = 0;

    hs_scratch_t *proto;
    hs_scratch_t *proto_tmp = hs_scratch_alloc(sizeof(struct hs_scratch) + 256);
    hs_error_t proto_ret = hs_check_alloc(proto_tmp);
    if (proto_ret != HS_SUCCESS) {
        hs_scratch_free(proto_tmp);
        hs_scratch_free(*scratch);
        *scratch = NULL;
        return proto_ret;
    }

    proto = ROUNDUP_PTR(proto_tmp, 64);

    if (*scratch) {
        *proto = **scratch;
    } else {
        memset(proto, 0, sizeof(*proto));
        resize = 1;
    }
    proto->scratch_alloc = (char *)proto_tmp;

    u32 max_anchored_match = rose->anchoredDistance;
    if (max_anchored_match > rose->maxSafeAnchoredDROffset) {
        u32 anchored_region_len = max_anchored_match
            - rose->maxSafeAnchoredDROffset;
        if (anchored_region_len > proto->anchored_region_len) {
            resize = 1;
            proto->anchored_region_len = anchored_region_len;
        }
    }

    u32 anchored_region_width = rose->anchoredMatches;
    if (anchored_region_width > proto->anchored_region_width) {
        resize = 1;
        proto->anchored_region_width = anchored_region_width;
    }

    if (rose->anchoredDistance > proto->anchored_literal_region_len) {
        resize = 1;
        proto->anchored_literal_region_len = rose->anchoredDistance;
    }

    if (rose->anchored_count > proto->anchored_literal_count) {
        resize = 1;
        proto->anchored_literal_count = rose->anchored_count;
    }

    if (rose->delay_count > proto->delay_count) {
        resize = 1;
        proto->delay_count = rose->delay_count;
    }

    if (rose->roleCount > proto->roleCount) {
        resize = 1;
        proto->roleCount = rose->roleCount;
    }

    if (rose->tStateSize > proto->tStateSize) {
        resize = 1;
        proto->tStateSize = rose->tStateSize;
    }

    const struct sidecar *side = getSLiteralMatcher(rose);
    if (side && sidecarScratchSize(side) > proto->sideScratchSize) {
        resize = 1;
        proto->sideScratchSize = sidecarScratchSize(side);
    }

    u32 som_store_count = rose->somLocationCount;
    if (som_store_count > proto->som_store_count) {
        resize = 1;
        proto->som_store_count = som_store_count;
    }

    u32 queueCount = rose->queueCount;
    if (queueCount > proto->queueCount) {
        resize = 1;
        proto->queueCount = queueCount;
    }

    u32 bStateSize = 0;
    if (rose->mode == HS_MODE_BLOCK) {
        bStateSize = rose->stateOffsets.end;
    } else if (rose->mode == HS_MODE_VECTORED) {
        /* vectoring database require a full stream state (inc header) */
        bStateSize = sizeof(struct hs_stream) + rose->stateOffsets.end;
    }

    if (bStateSize > proto->bStateSize) {
        resize = 1;
        proto->bStateSize = bStateSize;
    }

    u32 fullStateSize = rose->scratchStateSize;
    if (fullStateSize > proto->fullStateSize) {
        resize = 1;
        proto->fullStateSize = fullStateSize;
    }

    if (rose->dkeyCount > proto->deduper.log_size) {
        resize = 1;
        proto->deduper.log_size = rose->dkeyCount;
    }

    if (resize) {
        if (*scratch) {
            hs_scratch_free((*scratch)->scratch_alloc);
        }

        hs_error_t alloc_ret = alloc_scratch(proto, scratch);
        hs_scratch_free(proto_tmp); /* kill off temp used for sizing */
        if (alloc_ret != HS_SUCCESS) {
            *scratch = NULL;
            return alloc_ret;
        }
    } else {
        hs_scratch_free(proto_tmp); /* kill off temp used for sizing */
    }

    return HS_SUCCESS;
}

HS_PUBLIC_API
hs_error_t hs_clone_scratch(const hs_scratch_t *src, hs_scratch_t **dest) {
    if (!dest || !src || !ISALIGNED_CL(src) || src->magic != SCRATCH_MAGIC) {
        return HS_INVALID;
    }

    *dest = NULL;
    hs_error_t ret = alloc_scratch(src, dest);
    if (ret != HS_SUCCESS) {
        *dest = NULL;
        return ret;
    }

    return HS_SUCCESS;
}

HS_PUBLIC_API
hs_error_t hs_free_scratch(hs_scratch_t *scratch) {
    if (scratch) {
        /* has to be aligned before we can do anything with it */
        if (!ISALIGNED_CL(scratch)) {
            return HS_INVALID;
        }
        if (scratch->magic != SCRATCH_MAGIC) {
            return HS_INVALID;
        }
        scratch->magic = 0;
        assert(scratch->scratch_alloc);
        DEBUG_PRINTF("scratch %p is really at %p : freeing\n", scratch,
                     scratch->scratch_alloc);
        hs_scratch_free(scratch->scratch_alloc);
    }

    return HS_SUCCESS;
}

HS_PUBLIC_API
hs_error_t hs_scratch_size(const hs_scratch_t *scratch, size_t *size) {
    if (!size || !scratch || !ISALIGNED_CL(scratch) ||
        scratch->magic != SCRATCH_MAGIC) {
        return HS_INVALID;
    }

    *size = scratch->scratchSize;

    return HS_SUCCESS;
}
