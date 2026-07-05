#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>
#include <cormoran/animation/animation.pb.h>
#include <cormoran/animation/animation_request_exec.h>
#include <cormoran/animation/control.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta animation_subsystem_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://cormoran.github.io/zmk-driver-animation/"),
    /*
     * Unsecured: every operation in this subsystem is bounded (indices
     * validated against DT-fixed lists in animation_request_exec.c,
     * brightness clamped by control.c) and nothing gives raw memory/register
     * access, unlike pmw3610's WriteRegister (which is why that subsystem is
     * SECURED instead). Revisit if a future extension adds raw pixel writes
     * or animation-parameter upload (DESIGN.md #3.6 rationale, carried
     * forward from the design doc since nothing implemented in Phase D
     * changes this: TriggerAnimationRequest/StopOverlayRequest only ever
     * start/stop DT-configured animations for a bounded duration).
     */
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

static bool animation_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                         pb_callback_t *encode_response);

ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran__animation, &animation_subsystem_meta,
                         animation_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(cormoran__animation, cormoran_animation_Response);

/* Thin decode -> exec -> encode shell (pmw3610 handler/request_exec split,
 * DESIGN.md #3.6): all actual request handling lives in
 * animation_request_exec.c, shared (transport-agnostically) in case a
 * future split relay needs it (DESIGN.md #3.8 - not implemented in Phase D
 * scope). */
static bool animation_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                         pb_callback_t *encode_response) {
    cormoran_animation_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(cormoran__animation, encode_response);

    cormoran_animation_Request req = cormoran_animation_Request_init_zero;

    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, cormoran_animation_Request_fields, &req)) {
        LOG_WRN("Failed to decode animation request: %s", PB_GET_ERROR(&req_stream));
        cormoran_animation_ErrorResponse err = cormoran_animation_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to decode request");
        resp->which_response_type = cormoran_animation_Response_error_tag;
        resp->response_type.error = err;
        return true;
    }

    if (!zmk_animation_request_exec_handle(&req, resp)) {
        cormoran_animation_ErrorResponse err = cormoran_animation_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Unsupported request type");
        resp->which_response_type = cormoran_animation_Response_error_tag;
        resp->response_type.error = err;
    }

    return true;
}

/* --- Notification.state_changed (DESIGN.md #3.6) -------------------------
 *
 * control.c raises this synchronously (zmk_animation_set_state_changed_callback(),
 * a lightweight direct callback rather than a full ZMK event type - see
 * control.c's notify_state_changed()/the callback typedef's doc comment for
 * why a single in-module RPC consumer doesn't warrant a cross-module event)
 * whenever control state changes for any reason: RPC, a keymap behavior
 * press, settings-restore, or USB plug/unplug affecting the current power
 * source. This keeps a connected web UI live without polling.
 *
 * raise_zmk_studio_custom_notification()'s encode_payload callback runs
 * *inside* the raise call (unlike CallResponse encoding, which can run again
 * later from a static buffer) - zmk/studio/custom.h's doc comment confirms
 * this explicitly - so a stack-local Notification is safe to build and pass
 * here; no static buffer needed for this path.
 */

#define ANIMATION_SUBSYSTEM_IDENTIFIER_STRING "cormoran__animation"

static int custom_subsystem_index_for_identifier(const char *identifier, uint32_t *index) {
    if (!identifier) {
        return -ENOENT;
    }

    size_t subsystem_count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &subsystem_count);

    for (size_t i = 0; i < subsystem_count; i++) {
        struct zmk_rpc_custom_subsystem *custom_subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &custom_subsys);
        if (strcmp(custom_subsys->identifier, identifier) == 0) {
            *index = i;
            return 0;
        }
    }

    return -ENOENT;
}

static int animation_subsystem_index(void) {
    static int cached_index = -1;
    if (cached_index >= 0) {
        return cached_index;
    }

    uint32_t index;
    int ret = custom_subsystem_index_for_identifier(ANIMATION_SUBSYSTEM_IDENTIFIER_STRING, &index);
    if (ret < 0) {
        return ret;
    }

    cached_index = (int)index;
    return cached_index;
}

static bool encode_notification_payload(pb_ostream_t *stream, const pb_field_t *field,
                                        void *const *arg) {
    const cormoran_animation_Notification *notification =
        (const cormoran_animation_Notification *)*arg;
    return zmk_rpc_custom_subsystem_encode_response_payload(
        stream, field, cormoran_animation_Notification_fields, notification);
}

static void animation_state_changed_callback(void) {
    int index = animation_subsystem_index();
    if (index < 0) {
        return;
    }

    cormoran_animation_Notification notification =
        (cormoran_animation_Notification)cormoran_animation_Notification_init_zero;
    notification.which_notification_type = cormoran_animation_Notification_state_changed_tag;

    struct zmk_animation_state state;
    zmk_animation_get_state(&state);
    cormoran_animation_StateResponse *state_changed = &notification.notification_type.state_changed;
    state_changed->enabled = state.enabled;
    state_changed->brightness_powered = state.brightness_powered;
    state_changed->brightness_battery = state.brightness_battery;
    state_changed->selected_powered = state.selected_powered;
    state_changed->selected_battery = state.selected_battery;
    state_changed->is_powered = state.is_powered;
    state_changed->overlay_active = state.overlay_active;
    state_changed->has_overlay_index = state.has_overlay_index;
    state_changed->overlay_index = state.overlay_index;

    pb_callback_t payload = {
        .funcs.encode = encode_notification_payload,
        .arg = (void *)&notification,
    };
    int ret = raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = (uint8_t)index,
        .encode_payload = payload,
    });
    if (ret) {
        LOG_WRN("Failed to raise animation state_changed notification: %d", ret);
    }
}

static int animation_handler_init(void) {
    zmk_animation_set_state_changed_callback(animation_state_changed_callback);
    return 0;
}

SYS_INIT(animation_handler_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
