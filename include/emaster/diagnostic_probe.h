#ifndef EMASTER_DIAGNOSTIC_PROBE_H
#define EMASTER_DIAGNOSTIC_PROBE_H

typedef struct
{
    const char *interface_name;
    const char *output_path;
} emaster_diagnostic_probe_options_t;

int emaster_diagnostic_list_interfaces(void);
int emaster_diagnostic_probe_run(const emaster_diagnostic_probe_options_t *options);

#endif
