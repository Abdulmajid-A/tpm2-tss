/* SPDX-License-Identifier: BSD-2-Clause */
/*******************************************************************************
 * Copyright 2018-2019, Fraunhofer SIT sponsored by Infineon Technologies AG
 * All rights reserved.
 *******************************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdlib.h>

#include "tss2_mu.h"
#include "fapi_util.h"
#include "fapi_int.h"
#include "fapi_crypto.h"
#include "fapi_policy.h"
#include "ifapi_policy_instantiate.h"
#include "ifapi_policy_callbacks.h"
#include "ifapi_helpers.h"
#include "ifapi_json_deserialize.h"
#include "tpm_json_deserialize.h"
#include "ifapi_policy_store.h"
#define LOGMODULE fapi
#include "util/log.h"
#include "util/aux_util.h"

/** Compute policy digest for a policy tree.
 *
 * A policy or a policy path can be passed. If a policy is passed the
 * policy is computed directly from the policy otherwise the policy has to be
 * retrieved from policy store to determine the policy.
 *
 * @param[in,out] context The FAPI_CONTEXT.
 * @param[in]     policyPath The policy path for policy store.
 * @param[in]     policy The result of policy deserialization.
 * @param[in]     hash_alg The used hash alg for policy digest computations.
 * @param[out]    digest_idx The index of the current digest. The policy digest can be
 *                computed for several hash algorithms the digets index is a reverence
 *                to the current digest values.
 * @param[out]    hash_size The size of the current policy digest.
 *
 * @retval TSS2_FAPI_RC_MEMORY: if not enough memory can be allocated.
 * @retval TSS2_FAPI_RC_GENERAL_FAILURE If an internal error occurs, which is
 *         not covered by other return codes.
 * @retval TSS2_FAPI_RC_BAD_VALUE If wrong values are detected during policy calculation.
 * @retval TSS2_FAPI_RC_IO_ERROR If an error occurs during access to the policy
 *         store.
 * @retval TSS2_FAPI_RC_PATH_NOT_FOUND If an object needed for policy calculation was
 *         not found.
 * @retval TSS2_FAPI_RC_POLICY_UNKNOWN If policy search for a certain policy digest was
 *         not successful.
 * @retval TSS2_FAPI_RC_TRY_AGAIN if an I/O operation is not finished yet and
 *         this function needs to be called again.
 * @retval TSS2_FAPI_RC_BAD_SEQUENCE if the context has an asynchronous
 *         operation already pending.
 * @retval TSS2_FAPI_RC_BAD_REFERENCE a invalid null pointer is passed.
 * @retval TSS2_FAPI_RC_KEY_NOT_FOUND if a key was not found.
 * @retval TSS2_ESYS_RC_* possible error codes of ESAPI.
 * @retval TSS2_FAPI_RC_NOT_PROVISIONED FAPI was not provisioned.
 * @retval TSS2_FAPI_RC_BAD_PATH if the path is used in inappropriate context
 *         or contains illegal characters.
 */
TSS2_RC
ifapi_calculate_tree_ex(
    IFAPI_POLICY_CTX *context,
    IFAPI_POLICY_STORE *pstore,
    IFAPI_IO *io,
    const char *policyPath,
    TPMS_POLICY *policy,
    TPMI_ALG_HASH hash_alg,
    size_t *digest_idx,
    size_t *hash_size)
{
    size_t i;
    TSS2_RC r = TSS2_RC_SUCCESS;
    bool already_computed = false;

    if (context->state == POLICY_INIT && !policyPath)
        /* Skip policy reading */
        context->state = POLICY_INSTANTIATE_PREPARE;

    switch (context->state) {
    statecase(context->state, POLICY_INIT);
        fallthrough;

    statecase(context->state, POLICY_READ);
        r = ifapi_policy_store_load_async(pstore, io, policyPath);
        goto_if_error2(r, "Can't open: %s", cleanup, policyPath);
        fallthrough;

    statecase(context->state, POLICY_READ_FINISH);
        r = ifapi_policy_store_load_finish(pstore, io, policy);
        return_try_again(r);
        goto_if_error(r, "read_finish failed", cleanup);
        fallthrough;

    statecase(context->state, POLICY_INSTANTIATE_PREPARE);
        r = ifapi_policyeval_instantiate_async(&context->eval_ctx, policy);
        goto_if_error(r, "Instantiate policy.", cleanup);
        fallthrough;

    statecase(context->state, POLICY_INSTANTIATE);
        r = ifapi_policyeval_instantiate_finish(&context->eval_ctx);
        FAPI_SYNC(r, "Instantiate policy.", cleanup);
        ifapi_free_node_list(context->eval_ctx.policy_elements);
        context->eval_ctx.policy_elements = NULL;

        if (!(*hash_size = ifapi_hash_get_digest_size(hash_alg))) {
            goto_error(r, TSS2_FAPI_RC_BAD_VALUE,
                       "Unsupported hash algorithm (%" PRIu16 ")", cleanup,
                       hash_alg);
        }

        for (i = 0; i < policy->policyDigests.count; i++) {
            if (policy->policyDigests.digests[i].hashAlg == hash_alg) {
                /* Digest already computed */
                *digest_idx = i;
                already_computed = true;
            }
        }
        if (already_computed)
            break;

        if (i >= TPM2_NUM_PCR_BANKS) {
            goto_if_error(TSS2_FAPI_RC_BAD_VALUE, "Table overflow", cleanup);
        }
        *digest_idx = i;
        policy->policyDigests.count += 1;
        policy->policyDigests.digests[i].hashAlg = hash_alg;

        memset(&policy->policyDigests.digests[*digest_idx].digest, 0,
               sizeof(TPMU_HA));

        r = ifapi_calculate_policy(policy->policy,
                                   &policy->policyDigests, hash_alg,
                                   *hash_size, *digest_idx);
        goto_if_error(r, "Compute policy.", cleanup);

        break;
    statecasedefault(context->state);
    }
cleanup:
    ifapi_free_node_list(context->eval_ctx.policy_elements);
    context->eval_ctx.policy_elements = NULL;
    context->state = POLICY_INIT;
    return r;
}
