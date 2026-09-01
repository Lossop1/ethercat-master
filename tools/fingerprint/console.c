#define _POSIX_C_SOURCE 200809L

#include "console.h"
#include "emaster/messages.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static const char *probe_status_text(emaster_preop_probe_status_t status)
{
    switch (status)
    {
        case EMASTER_PREOP_PROBE_INVALID_ARGUMENT:
            return emaster_text(EMASTER_TEXT_PROBE_INVALID_ARGUMENT);
        case EMASTER_PREOP_PROBE_INTERFACE_ENUMERATION_FAILED:
            return emaster_text(EMASTER_TEXT_PROBE_INTERFACE_ENUMERATION_FAILED);
        case EMASTER_PREOP_PROBE_INTERFACE_OPEN_FAILED:
            return emaster_text(EMASTER_TEXT_PROBE_INTERFACE_OPEN_FAILED);
        case EMASTER_PREOP_PROBE_NO_SLAVES:
            return emaster_text(EMASTER_TEXT_PROBE_NO_SLAVES);
        case EMASTER_PREOP_PROBE_TOO_MANY_SLAVES:
            return emaster_text(EMASTER_TEXT_PROBE_TOO_MANY_SLAVES);
        case EMASTER_PREOP_PROBE_PREOP_NOT_REACHED:
            return emaster_text(EMASTER_TEXT_PROBE_PREOP_NOT_REACHED);
        case EMASTER_PREOP_PROBE_SDO_READ_FAILED:
            return emaster_text(EMASTER_TEXT_PROBE_SDO_READ_FAILED);
        case EMASTER_PREOP_PROBE_PDO_DISCOVERY_FAILED:
            return emaster_text(EMASTER_TEXT_PROBE_PDO_DISCOVERY_FAILED);
        case EMASTER_PREOP_PROBE_OUT_OF_MEMORY:
            return emaster_text(EMASTER_TEXT_PROBE_OUT_OF_MEMORY);
        case EMASTER_PREOP_PROBE_RESTORE_INIT_FAILED:
            return emaster_text(EMASTER_TEXT_PROBE_RESTORE_INIT_FAILED);
        case EMASTER_PREOP_PROBE_OK:
            return emaster_text(EMASTER_TEXT_PROBE_OK);
    }
    return emaster_text(EMASTER_TEXT_PROBE_UNKNOWN);
}

emaster_command_t emaster_console_parse_command(int argc, char **argv)
{
    emaster_command_t result = {EMASTER_COMMAND_INVALID, NULL};

    if (argc == 2 && strcmp(argv[1], emaster_text(EMASTER_TEXT_COMMAND_INTERFACES)) == 0)
    {
        result.kind = EMASTER_COMMAND_INTERFACES;
    }
    else if (argc == 2 && strcmp(argv[1], emaster_text(EMASTER_TEXT_COMMAND_DEPLOYMENTS)) == 0)
    {
        result.kind = EMASTER_COMMAND_DEPLOYMENTS;
    }
    else if (argc == 3 && strcmp(argv[1], emaster_text(EMASTER_TEXT_COMMAND_CAPTURE)) == 0)
    {
        result.kind = EMASTER_COMMAND_CAPTURE;
        result.output_path = argv[2];
    }
    return result;
}

void emaster_console_print_usage(const char *program)
{
    fprintf(stderr, emaster_text(EMASTER_TEXT_USAGE), program,
            emaster_text(EMASTER_TEXT_COMMAND_INTERFACES), program,
            emaster_text(EMASTER_TEXT_COMMAND_DEPLOYMENTS), program,
            emaster_text(EMASTER_TEXT_COMMAND_CAPTURE));
}

void emaster_console_print_interface(const char *name, const char *description,
                                     void *user_data)
{
    FILE *output = user_data;
    fprintf(output, emaster_text(EMASTER_TEXT_INTERFACE_LINE), name,
            description != NULL ? description : "");
}

void emaster_console_print_deployment(const emaster_deployment_config_t *deployment)
{
    printf(emaster_text(EMASTER_TEXT_DEPLOYMENT_LINE), deployment->deployment_id,
           deployment->hostname, deployment->ethercat_interface, deployment->topology->topology_id);
}

bool emaster_console_confirm_preop(const emaster_deployment_config_t *deployment)
{
    char input[32];
    size_t length;

    if (deployment == NULL || !isatty(STDIN_FILENO))
    {
        fputs(emaster_text(EMASTER_TEXT_PREOP_CONFIRM_REQUIRED), stderr);
        return false;
    }
    fprintf(stderr, emaster_text(EMASTER_TEXT_PREOP_CONFIRM_PROMPT), deployment->hostname,
            deployment->ethercat_interface, deployment->topology->topology_id,
            emaster_text(EMASTER_TEXT_PREOP_CONFIRMATION_TOKEN));
    if (fflush(stderr) != 0 || fgets(input, sizeof(input), stdin) == NULL)
    {
        return false;
    }
    length = strcspn(input, "\r\n");
    input[length] = '\0';
    return strcmp(input, emaster_text(EMASTER_TEXT_PREOP_CONFIRMATION_TOKEN)) == 0;
}

void emaster_console_print_message(emaster_message_id_t message, const char *detail,
                                   int system_error)
{
    const char *text = detail != NULL ? detail : "";
    const char *error_text = system_error != 0 ? strerror(system_error) : "";

    switch (message)
    {
        case EMASTER_MESSAGE_DEPLOYMENT_UNAVAILABLE:
            fputs(emaster_text(EMASTER_TEXT_MESSAGE_DEPLOYMENT_UNAVAILABLE), stderr);
            break;
        case EMASTER_MESSAGE_OUTPUT_EXISTS:
            fprintf(stderr, emaster_text(EMASTER_TEXT_MESSAGE_OUTPUT_EXISTS), text);
            break;
        case EMASTER_MESSAGE_SDO_PLAN_FAILED:
            fputs(emaster_text(EMASTER_TEXT_MESSAGE_SDO_PLAN_FAILED), stderr);
            break;
        case EMASTER_MESSAGE_PREOP_NOT_CONFIRMED:
            fputs(emaster_text(EMASTER_TEXT_MESSAGE_PREOP_NOT_CONFIRMED), stderr);
            break;
        case EMASTER_MESSAGE_TIMESTAMP_FAILED:
            fputs(emaster_text(EMASTER_TEXT_MESSAGE_TIMESTAMP_FAILED), stderr);
            break;
        case EMASTER_MESSAGE_EMPTY_REPORT:
            fputs(emaster_text(EMASTER_TEXT_MESSAGE_EMPTY_REPORT), stderr);
            break;
        case EMASTER_MESSAGE_TOPOLOGY_OR_PDO_MISMATCH:
            fputs(emaster_text(EMASTER_TEXT_MESSAGE_TOPOLOGY_OR_PDO_MISMATCH), stderr);
            break;
        case EMASTER_MESSAGE_OUTPUT_PATH_TOO_LONG:
            fputs(emaster_text(EMASTER_TEXT_MESSAGE_OUTPUT_PATH_TOO_LONG), stderr);
            break;
        case EMASTER_MESSAGE_OUTPUT_CREATE_FAILED:
            fprintf(stderr, emaster_text(EMASTER_TEXT_MESSAGE_OUTPUT_CREATE_FAILED), error_text);
            break;
        case EMASTER_MESSAGE_OUTPUT_OPEN_FAILED:
            fprintf(stderr, emaster_text(EMASTER_TEXT_MESSAGE_OUTPUT_OPEN_FAILED), error_text);
            break;
        case EMASTER_MESSAGE_OUTPUT_WRITE_FAILED:
            fputs(emaster_text(EMASTER_TEXT_MESSAGE_OUTPUT_WRITE_FAILED), stderr);
            break;
        case EMASTER_MESSAGE_OUTPUT_FLUSH_FAILED:
            fprintf(stderr, emaster_text(EMASTER_TEXT_MESSAGE_OUTPUT_FLUSH_FAILED), error_text);
            break;
        case EMASTER_MESSAGE_OUTPUT_PUBLISH_FAILED:
            fprintf(stderr, emaster_text(EMASTER_TEXT_MESSAGE_OUTPUT_PUBLISH_FAILED), error_text);
            break;
        case EMASTER_MESSAGE_DIAGNOSTIC_REPORT_SAVED:
            fputs(emaster_text(EMASTER_TEXT_MESSAGE_DIAGNOSTIC_REPORT_SAVED), stderr);
            break;
        case EMASTER_MESSAGE_INCOMPLETE_REPORT_REJECTED:
            fputs(emaster_text(EMASTER_TEXT_MESSAGE_INCOMPLETE_REPORT_REJECTED), stderr);
            break;
    }
}

void emaster_console_print_probe_status(emaster_preop_probe_status_t status,
                                        const char *interface_name, bool warning)
{
    emaster_text_id_t status_text = warning ? EMASTER_TEXT_PROBE_STATUS_WARNING
                                            : EMASTER_TEXT_PROBE_STATUS_ERROR;
    if (interface_name != NULL && interface_name[0] != '\0')
    {
        status_text = warning ? EMASTER_TEXT_PROBE_STATUS_INTERFACE_WARNING
                              : EMASTER_TEXT_PROBE_STATUS_INTERFACE_ERROR;
        fprintf(stderr, emaster_text(status_text), probe_status_text(status), interface_name);
    }
    else
    {
        fprintf(stderr, emaster_text(status_text), probe_status_text(status));
    }
}

void emaster_console_print_pdo_failure(const emaster_pdo_layout_t *layout,
                                       uint16_t position)
{
    if (layout == NULL || layout->status == EMASTER_PDO_DISCOVERY_COMPLETE)
    {
        return;
    }
    fprintf(stderr, emaster_text(EMASTER_TEXT_PDO_FAILURE), (unsigned int)position,
            (unsigned int)layout->status, layout->failed_index,
            (unsigned int)layout->failed_subindex);
}
