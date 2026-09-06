#define _POSIX_C_SOURCE 200809L

#include "console.h"
#include "emaster/audit/run_report.h"
#include "emaster/bus/control_session.h"
#include "emaster/config/runtime_config.h"
#include "emaster/messages.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested = 0;

static void request_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static bool application_stop_requested(void *user_data) {
    (void)user_data;
    return stop_requested != 0;
}

static bool install_signal_handlers(void) {
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) != 0) {
        return false;
    }
    return sigaction(SIGINT, &action, NULL) == 0 && sigaction(SIGTERM, &action, NULL) == 0;
}

static const emaster_deployment_config_t *deployment_for_current_host(void) {
    char hostname[256];
    const emaster_deployment_config_t *match = NULL;
    size_t index;

    if (gethostname(hostname, sizeof(hostname) - 1U) != 0) {
        return NULL;
    }
    hostname[sizeof(hostname) - 1U] = '\0';
    for (index = 0U; index < emaster_deployment_config_count(); ++index) {
        const emaster_deployment_config_t *candidate = emaster_deployment_config_at(index);
        if (candidate != NULL && candidate->hostname != NULL &&
            strcmp(hostname, candidate->hostname) == 0) {
            if (match != NULL) {
                return NULL;
            }
            match = candidate;
        }
    }
    return match;
}

int main(int argc, char **argv) {
    const emaster_deployment_config_t *deployment;
    emaster_session_axis_plan_t *plan_axes;
    emaster_session_plan_t plan;
    emaster_control_session_axis_result_t *results;
    emaster_control_session_report_t report;
    emaster_control_session_callbacks_t callbacks;
    emaster_session_plan_status_t plan_status;
    emaster_control_session_status_t session_status;
    bool report_published;
    size_t axis_capacity;

    if (argc != 1) {
        fprintf(stderr, emaster_text(EMASTER_TEXT_CONTROL_SESSION_USAGE), argv[0]);
        return 2;
    }
    if (!install_signal_handlers()) {
        fputs(emaster_text(EMASTER_TEXT_CONTROL_SESSION_SIGNAL_FAILED), stderr);
        return 1;
    }
    deployment = deployment_for_current_host();
    if (deployment == NULL || deployment->topology == NULL) {
        fputs(emaster_text(EMASTER_TEXT_MESSAGE_DEPLOYMENT_UNAVAILABLE), stderr);
        return 1;
    }
    axis_capacity = deployment->topology->slave_count;
    plan_axes = calloc(axis_capacity, sizeof(*plan_axes));
    results = calloc(axis_capacity, sizeof(*results));
    if (plan_axes == NULL || results == NULL) {
        free(plan_axes);
        free(results);
        fputs(emaster_text(EMASTER_TEXT_PROBE_OUT_OF_MEMORY), stderr);
        return 1;
    }
    plan_status = emaster_session_plan_build(deployment, plan_axes, axis_capacity, &plan);
    if (plan_status != EMASTER_SESSION_PLAN_READY) {
        free(plan_axes);
        free(results);
        fputs(emaster_text(EMASTER_TEXT_CONTROL_SESSION_PLAN_FAILED), stderr);
        return 1;
    }
    fprintf(stdout, emaster_text(EMASTER_TEXT_CONTROL_SESSION_START), deployment->hostname,
            deployment->ethercat_interface, deployment->topology->topology_id,
            (unsigned int)plan.axis_count);
    (void)fflush(stdout);

    memset(&report, 0, sizeof(report));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.stop_requested = application_stop_requested;
    session_status = emaster_soem_control_session(&plan, results, axis_capacity,
                                                  &callbacks, &report);
    report_published = emaster_run_report_publish(&plan, &report, deployment->run_report_path);
    emaster_master_console_result(&plan, &report, report_published);
    emaster_control_session_report_destroy(&report);
    free(plan_axes);
    free(results);
    return session_status == EMASTER_CONTROL_SESSION_OK && report_published ? 0 : 1;
}
