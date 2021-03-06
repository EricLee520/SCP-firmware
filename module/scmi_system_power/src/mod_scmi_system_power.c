/*
 * Arm SCP/MCP Software
 * Copyright (c) 2015-2020, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <internal/scmi.h>
#include <internal/scmi_system_power.h>

#include <mod_power_domain.h>
#include <mod_scmi.h>
#include <mod_scmi_system_power.h>

#include <fwk_assert.h>
#include <fwk_id.h>
#include <fwk_macros.h>
#include <fwk_mm.h>
#include <fwk_module.h>
#include <fwk_module_idx.h>
#include <fwk_status.h>

#include <stddef.h>

struct scmi_sys_power_ctx {
    const struct mod_scmi_system_power_config *config;
    const struct mod_scmi_from_protocol_api *scmi_api;
    const struct mod_pd_restricted_api *pd_api;
    fwk_id_t system_power_domain_id;
#ifdef BUILD_HAS_SCMI_NOTIFICATIONS
    int agent_count;
    fwk_id_t *system_power_notifications;
#endif
};

static int scmi_sys_power_version_handler(fwk_id_t service_id,
    const uint32_t *payload);
static int scmi_sys_power_attributes_handler(fwk_id_t service_id,
    const uint32_t *payload);
static int scmi_sys_power_msg_attributes_handler(fwk_id_t service_id,
    const uint32_t *payload);
static int scmi_sys_power_state_set_handler(fwk_id_t service_id,
    const uint32_t *payload);
static int scmi_sys_power_state_get_handler(fwk_id_t service_id,
    const uint32_t *payload);
#ifdef BUILD_HAS_SCMI_NOTIFICATIONS
static int scmi_sys_power_state_notify_handler(fwk_id_t service_id,
    const uint32_t *payload);
#endif

/*
 * Internal variables
 */
static struct scmi_sys_power_ctx scmi_sys_power_ctx;

static int (* const handler_table[])(fwk_id_t, const uint32_t *) = {
    [SCMI_PROTOCOL_VERSION] = scmi_sys_power_version_handler,
    [SCMI_PROTOCOL_ATTRIBUTES] = scmi_sys_power_attributes_handler,
    [SCMI_PROTOCOL_MESSAGE_ATTRIBUTES] = scmi_sys_power_msg_attributes_handler,
    [SCMI_SYS_POWER_STATE_SET] = scmi_sys_power_state_set_handler,
    [SCMI_SYS_POWER_STATE_GET] = scmi_sys_power_state_get_handler,
#ifdef BUILD_HAS_SCMI_NOTIFICATIONS
    [SCMI_SYS_POWER_STATE_NOTIFY] = scmi_sys_power_state_notify_handler,
#endif
};

static const unsigned int payload_size_table[] = {
    [SCMI_PROTOCOL_VERSION] = 0,
    [SCMI_PROTOCOL_ATTRIBUTES] = 0,
    [SCMI_PROTOCOL_MESSAGE_ATTRIBUTES] =
                       sizeof(struct scmi_protocol_message_attributes_a2p),
    [SCMI_SYS_POWER_STATE_SET] =
                       sizeof(struct scmi_sys_power_state_set_a2p),
    [SCMI_SYS_POWER_STATE_GET] = 0,
#ifdef BUILD_HAS_SCMI_NOTIFICATIONS
    [SCMI_SYS_POWER_STATE_NOTIFY] =
                       sizeof(struct scmi_sys_power_state_notify_a2p),
#endif
};

static enum mod_pd_system_shutdown system_state2system_shutdown[] = {
    [SCMI_SYSTEM_STATE_SHUTDOWN] = MOD_PD_SYSTEM_SHUTDOWN,
    [SCMI_SYSTEM_STATE_COLD_RESET] = MOD_PD_SYSTEM_COLD_RESET,
    [SCMI_SYSTEM_STATE_WARM_RESET] = MOD_PD_SYSTEM_WARM_RESET,
};

static int system_state_get(enum scmi_system_state *system_state)
{
    int status;
    unsigned int state;

    status = scmi_sys_power_ctx.pd_api->get_state(
        scmi_sys_power_ctx.system_power_domain_id, &state);
    if (status != FWK_SUCCESS)
        return status;

    switch (state) {
    case MOD_PD_STATE_OFF:
        *system_state = SCMI_SYSTEM_STATE_SHUTDOWN;
        break;

    case MOD_PD_STATE_ON:
        *system_state = SCMI_SYSTEM_STATE_POWER_UP;
        break;

    default:
        *system_state = SCMI_SYSTEM_STATE_SUSPEND;
    }

    return FWK_SUCCESS;
}

#ifdef BUILD_HAS_SCMI_NOTIFICATIONS
static void scmi_sys_power_state_notify(fwk_id_t service_id,
    uint32_t system_state, bool forceful)
{
    unsigned int agent_id;
    fwk_id_t id;
    struct scmi_sys_power_state_notifier return_values;
    int status, i;

    status = scmi_sys_power_ctx.scmi_api->get_agent_id(service_id, &agent_id);
    if (status != FWK_SUCCESS)
        return;

    return_values.agent_id = (uint32_t)agent_id;
    return_values.system_state = system_state;
    if (forceful)
        return_values.flags = 0;
    else
        return_values.flags = 1;

    for (i = 0; i < SCMI_AGENT_ID_MAX; i++) {
        id =  scmi_sys_power_ctx.system_power_notifications[i];
        if (fwk_id_is_equal(id, FWK_ID_NONE))
            continue;

        scmi_sys_power_ctx.scmi_api->notify(id,
            SCMI_PROTOCOL_ID_SYS_POWER, SCMI_SYS_POWER_STATE_SET_NOTIFY,
            &return_values, sizeof(return_values));
    }
}
#endif

/*
 * PROTOCOL_VERSION
 */
static int scmi_sys_power_version_handler(fwk_id_t service_id,
                                          const uint32_t *payload)
{
    struct scmi_protocol_version_p2a return_values = {
        .status = SCMI_SUCCESS,
        .version = SCMI_PROTOCOL_VERSION_SYS_POWER,
    };

    scmi_sys_power_ctx.scmi_api->respond(service_id, &return_values,
                                         sizeof(return_values));
    return FWK_SUCCESS;
}

/*
 * PROTOCOL_ATTRIBUTES
 */
static int scmi_sys_power_attributes_handler(fwk_id_t service_id,
                                             const uint32_t *payload)
{
    struct scmi_protocol_attributes_p2a return_values = {
        .status = SCMI_SUCCESS,
        .attributes = 0,
    };

    scmi_sys_power_ctx.scmi_api->respond(service_id, &return_values,
                                         sizeof(return_values));
    return FWK_SUCCESS;
}

/*
 * PROTOCOL_MESSAGE_ATTRIBUTES
 */
static int scmi_sys_power_msg_attributes_handler(fwk_id_t service_id,
                                                 const uint32_t *payload)
{
    const struct scmi_protocol_message_attributes_a2p *parameters;
    unsigned int message_id;
    struct scmi_protocol_message_attributes_p2a return_values;

    parameters = (const struct scmi_protocol_message_attributes_a2p*)payload;
    message_id = parameters->message_id;

    if ((message_id >= FWK_ARRAY_SIZE(handler_table)) ||
        (handler_table[message_id] == NULL)) {

        return_values.status = SCMI_NOT_FOUND;
        goto exit;
    }

    return_values = (struct scmi_protocol_message_attributes_p2a) {
        .status = SCMI_SUCCESS,
        .attributes = 0,
    };

    if (message_id == SCMI_SYS_POWER_STATE_SET) {
        return_values.attributes |= SYS_POWER_STATE_SET_ATTRIBUTES_SUSPEND |
                                    SYS_POWER_STATE_SET_ATTRIBUTES_WARM_RESET;
    }

exit:
    scmi_sys_power_ctx.scmi_api->respond(service_id, &return_values,
        (return_values.status == SCMI_SUCCESS) ? sizeof(return_values) :
        sizeof(return_values.status));

    return FWK_SUCCESS;
}

/*
 * SYSTEM_POWER_STATE_SET
 */
static int scmi_sys_power_state_set_handler(fwk_id_t service_id,
                                            const uint32_t *payload)
{
    int status = FWK_SUCCESS;
    const struct scmi_sys_power_state_set_a2p *parameters;
    struct scmi_sys_power_state_set_p2a return_values = {
        .status = SCMI_GENERIC_ERROR,
    };
    unsigned int agent_id;
    enum scmi_agent_type agent_type;
    enum mod_pd_system_shutdown system_shutdown;
    uint32_t scmi_system_state;
    enum mod_scmi_sys_power_policy_status policy_status;

    parameters = (const struct scmi_sys_power_state_set_a2p *)payload;

    if (parameters->flags & (~STATE_SET_FLAGS_MASK)) {
        return_values.status = SCMI_INVALID_PARAMETERS;
        goto exit;
    }

    status = scmi_sys_power_ctx.scmi_api->get_agent_id(service_id, &agent_id);
    if (status != FWK_SUCCESS)
        goto exit;

    status = scmi_sys_power_ctx.scmi_api->get_agent_type(agent_id, &agent_type);
    if (status != FWK_SUCCESS)
        goto exit;

    if ((agent_type != SCMI_AGENT_TYPE_PSCI) &&
        (agent_type != SCMI_AGENT_TYPE_MANAGEMENT)) {

        return_values.status = SCMI_NOT_SUPPORTED;
        goto exit;
    }

    /*
     * Graceful request from MCP not supported yet as we need the
     * notifications to do that.
     */
    if (parameters->flags & STATE_SET_FLAGS_GRACEFUL_REQUEST) {
        return_values.status = SCMI_NOT_SUPPORTED;
        goto exit;
    }

    /*
     * Note that the scmi_system_state value may be changed by the policy
     * handler.
     */
    scmi_system_state = parameters->system_state;
    status = scmi_sys_power_state_set_policy(&policy_status, &scmi_system_state,
        agent_id);

    if (status != FWK_SUCCESS) {
        return_values.status = SCMI_GENERIC_ERROR;
        goto exit;
    }
    if (policy_status == MOD_SCMI_SYS_POWER_SKIP_MESSAGE_HANDLER) {
        return_values.status = SCMI_SUCCESS;
        goto exit;
    }


    switch (scmi_system_state) {
    case SCMI_SYSTEM_STATE_SHUTDOWN:
    case SCMI_SYSTEM_STATE_COLD_RESET:
    case SCMI_SYSTEM_STATE_WARM_RESET:
        system_shutdown =
            system_state2system_shutdown[scmi_system_state];
        status = scmi_sys_power_ctx.pd_api->system_shutdown(system_shutdown);
        if (status == FWK_PENDING) {
            /*
             * The request has been acknowledged but we don't respond back to
             * the calling agent. This is a fire-and-forget situation.
             */
            return FWK_SUCCESS;
        } else
            goto exit;
        break;

    case SCMI_SYSTEM_STATE_SUSPEND:
        status = scmi_sys_power_ctx.pd_api->system_suspend(
            scmi_sys_power_ctx.config->system_suspend_state);
        if (status != FWK_SUCCESS) {
            if (status == FWK_E_STATE) {
                status = FWK_SUCCESS;
                return_values.status = SCMI_DENIED;
            }
            goto exit;
        }
        break;

    case SCMI_SYSTEM_STATE_POWER_UP:
        if ((agent_type != SCMI_AGENT_TYPE_MANAGEMENT) ||
            (scmi_sys_power_ctx.config->system_view !=
             MOD_SCMI_SYSTEM_VIEW_OSPM)) {

            return_values.status = SCMI_NOT_SUPPORTED;
            goto exit;
        }

        status = system_state_get((enum scmi_system_state *)&scmi_system_state);
        if (status != FWK_SUCCESS)
            goto exit;

        if ((scmi_system_state != SCMI_SYSTEM_STATE_SHUTDOWN) &&
            (scmi_system_state != SCMI_SYSTEM_STATE_SUSPEND)) {

            return_values.status = SCMI_DENIED;
            goto exit;
        }

        status = scmi_sys_power_ctx.pd_api->set_composite_state(
                     scmi_sys_power_ctx.config->wakeup_power_domain_id,
                     scmi_sys_power_ctx.config->wakeup_composite_state);
        if (status != FWK_SUCCESS)
            goto exit;
        break;

    default:
        return_values.status = SCMI_INVALID_PARAMETERS;
        goto exit;
    };

#ifdef BUILD_HAS_SCMI_NOTIFICATIONS
    scmi_sys_power_state_notify(service_id, scmi_system_state,
        ((parameters->flags & STATE_SET_FLAGS_GRACEFUL_REQUEST) ?
            true : false));
#endif

    return_values.status = SCMI_SUCCESS;

exit:
    scmi_sys_power_ctx.scmi_api->respond(service_id, &return_values,
        (return_values.status == SCMI_SUCCESS) ? sizeof(return_values) :
        sizeof(return_values.status));

    return status;
}

/*
 * SYSTEM_POWER_STATE_GET
 */
static int scmi_sys_power_state_get_handler(fwk_id_t service_id,
                                            const uint32_t *payload)
{
    int status = FWK_SUCCESS;
    struct scmi_sys_power_state_get_p2a return_values = {
        .status = SCMI_GENERIC_ERROR,
    };
    enum scmi_system_state system_state;
    unsigned int agent_id;
    enum scmi_agent_type agent_type;

    status = scmi_sys_power_ctx.scmi_api->get_agent_id(service_id, &agent_id);
    if (status != FWK_SUCCESS)
        goto exit;

    status = scmi_sys_power_ctx.scmi_api->get_agent_type(agent_id, &agent_type);
    if (status != FWK_SUCCESS)
        goto exit;

    return_values.status = SCMI_NOT_SUPPORTED;
    if (scmi_sys_power_ctx.config->system_view == MOD_SCMI_SYSTEM_VIEW_FULL)
        goto exit;
    else {
        if (agent_type == SCMI_AGENT_TYPE_PSCI)
            goto exit;

        status = system_state_get(&system_state);
        if (status != FWK_SUCCESS)
            goto exit;

        return_values = (struct scmi_sys_power_state_get_p2a) {
            .status = SCMI_SUCCESS,
            .system_state = system_state,
        };
    }

exit:
    scmi_sys_power_ctx.scmi_api->respond(service_id, &return_values,
        (return_values.status == SCMI_SUCCESS) ? sizeof(return_values) :
        sizeof(return_values.status));

    return status;
}

#ifdef BUILD_HAS_SCMI_NOTIFICATIONS
/*
 * SYSTEM_POWER_STATE_NOTIFY
 */
static int scmi_sys_power_state_notify_handler(fwk_id_t service_id,
                                               const uint32_t *payload)
{
    unsigned int agent_id;
    const struct scmi_sys_power_state_notify_a2p *parameters;
    struct scmi_sys_power_state_notify_p2a return_values = {
        .status = SCMI_GENERIC_ERROR,
    };
    int status;

    status = scmi_sys_power_ctx.scmi_api->get_agent_id(service_id, &agent_id);
    if (status != FWK_SUCCESS)
        goto exit;

    parameters = (const struct scmi_sys_power_state_notify_a2p *)payload;

    if (parameters->flags & (~STATE_NOTIFY_FLAGS_MASK)) {
        return_values.status = SCMI_INVALID_PARAMETERS;
        goto exit;
    }

    if (parameters->flags & STATE_NOTIFY_FLAGS_MASK)
        scmi_sys_power_ctx.system_power_notifications[agent_id] = service_id;
    else
        scmi_sys_power_ctx.system_power_notifications[agent_id] = FWK_ID_NONE;

    return_values.status = SCMI_SUCCESS;

exit:
    scmi_sys_power_ctx.scmi_api->respond(service_id, &return_values,
        (return_values.status == SCMI_SUCCESS) ? sizeof(return_values) :
        sizeof(return_values.status));

    return FWK_SUCCESS;
}
#endif

/*
 * SCMI System Power Policy Handlers
 *
 * The system_state value may be modified by the policy handler
 */
__attribute((weak)) int scmi_sys_power_state_set_policy(
    enum mod_scmi_sys_power_policy_status *policy_status,
    uint32_t *state,
    unsigned int agent_id)
{
    *policy_status = MOD_SCMI_SYS_POWER_EXECUTE_MESSAGE_HANDLER;

    return FWK_SUCCESS;
}

/*
 * SCMI module -> SCMI system power module interface
 */
static int scmi_sys_power_get_scmi_protocol_id(fwk_id_t protocol_id,
                                               uint8_t *scmi_protocol_id)
{
    *scmi_protocol_id = SCMI_PROTOCOL_ID_SYS_POWER;

    return FWK_SUCCESS;
}

static int scmi_sys_power_handler(fwk_id_t protocol_id,
                                  fwk_id_t service_id,
                                  const uint32_t *payload,
                                  size_t payload_size,
                                  unsigned int message_id)
{
    int32_t return_value;

    static_assert(FWK_ARRAY_SIZE(handler_table) ==
                  FWK_ARRAY_SIZE(payload_size_table),
                  "[SCMI] System power protocol table sizes not consistent");

    fwk_assert(payload != NULL);

    if (message_id >= FWK_ARRAY_SIZE(handler_table)) {
        return_value = SCMI_NOT_SUPPORTED;
        goto error;
    }

    if (payload_size != payload_size_table[message_id]) {
        /* Incorrect payload size or message is not supported */
        return_value = SCMI_PROTOCOL_ERROR;
        goto error;
    }

    return handler_table[message_id](service_id, payload);

error:
    scmi_sys_power_ctx.scmi_api->respond(service_id, &return_value,
                                         sizeof(return_value));
    return FWK_SUCCESS;
}

static struct mod_scmi_to_protocol_api scmi_sys_power_mod_scmi_to_protocol = {
    .get_scmi_protocol_id = scmi_sys_power_get_scmi_protocol_id,
    .message_handler = scmi_sys_power_handler,
};

/*
 * Framework handlers
 */
static int scmi_sys_power_init(fwk_id_t module_id, unsigned int element_count,
                               const void *data)
{
    scmi_sys_power_ctx.config = data;

    return FWK_SUCCESS;
}

#ifdef BUILD_HAS_SCMI_NOTIFICATIONS
static int scmi_sys_power_init_notifications(void)
{
    int status;
    int i;


    status = scmi_sys_power_ctx.scmi_api->get_agent_count(
        &scmi_sys_power_ctx.agent_count);
    if (status != FWK_SUCCESS)
        return status;
    fwk_assert(scmi_sys_power_ctx.agent_count != 0);

    scmi_sys_power_ctx.system_power_notifications = fwk_mm_calloc(
        scmi_sys_power_ctx.agent_count, sizeof(fwk_id_t));

    for (i = 0; i < scmi_sys_power_ctx.agent_count; i++)
        scmi_sys_power_ctx.system_power_notifications[i] = FWK_ID_NONE;

    return FWK_SUCCESS;

}
#endif

static int scmi_sys_power_bind(fwk_id_t id, unsigned int round)
{
    int status;
    int pd_count;

    if (round != 0)
        return FWK_SUCCESS;

    /* Bind to SCMI module */
    status = fwk_module_bind(FWK_ID_MODULE(FWK_MODULE_IDX_SCMI),
                             FWK_ID_API(FWK_MODULE_IDX_SCMI,
                                        MOD_SCMI_API_IDX_PROTOCOL),
                             &scmi_sys_power_ctx.scmi_api);
    if (status != FWK_SUCCESS)
        return status;

    /* Bind to POWER DOMAIN module */
    status = fwk_module_bind(fwk_module_id_power_domain,
        mod_pd_api_id_restricted, &scmi_sys_power_ctx.pd_api);
    if (status != FWK_SUCCESS)
        return status;

    pd_count = fwk_module_get_element_count(fwk_module_id_power_domain);
    if (pd_count <= 0)
        return FWK_E_SUPPORT;

    scmi_sys_power_ctx.system_power_domain_id =
        FWK_ID_ELEMENT(FWK_MODULE_IDX_POWER_DOMAIN, pd_count - 1);

#ifdef BUILD_HAS_SCMI_NOTIFICATIONS
    return scmi_sys_power_init_notifications();
#else
    return FWK_SUCCESS;
#endif
}

static int scmi_sys_power_process_bind_request(fwk_id_t source_id,
    fwk_id_t _target_id, fwk_id_t api_id, const void **api)
{
    if (!fwk_id_is_equal(source_id, FWK_ID_MODULE(FWK_MODULE_IDX_SCMI)))
        return FWK_E_ACCESS;

    *api = &scmi_sys_power_mod_scmi_to_protocol;

    return FWK_SUCCESS;
}

const struct fwk_module module_scmi_system_power = {
    .name = "SCMI System Power Management Protocol",
    .api_count = 1,
    .type = FWK_MODULE_TYPE_PROTOCOL,
    .init = scmi_sys_power_init,
    .bind = scmi_sys_power_bind,
    .process_bind_request = scmi_sys_power_process_bind_request,
};
