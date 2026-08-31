#include "emaster/diagnostic_probe.h"

#include <stdio.h>
#include <string.h>

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s --list-interfaces\n"
            "  %s --interface IFACE --output FILE --acknowledge-preop\n\n"
            "The probe sends EtherCAT discovery traffic and requests PRE-OP. It performs only "
            "SDO reads, does not map PDOs, does not configure DC, and does not request SAFE-OP "
            "or OP. It attempts to restore INIT before closing.\n",
            program, program);
}

int main(int argc, char **argv)
{
    emaster_diagnostic_probe_options_t options = {0};
    int acknowledge_preop = 0;
    int index;

    if (argc == 2 && strcmp(argv[1], "--list-interfaces") == 0)
    {
        return emaster_diagnostic_list_interfaces();
    }

    for (index = 1; index < argc; ++index)
    {
        if (strcmp(argv[index], "--interface") == 0 && index + 1 < argc)
        {
            options.interface_name = argv[++index];
        }
        else if (strcmp(argv[index], "--output") == 0 && index + 1 < argc)
        {
            options.output_path = argv[++index];
        }
        else if (strcmp(argv[index], "--acknowledge-preop") == 0)
        {
            acknowledge_preop = 1;
        }
        else
        {
            usage(argv[0]);
            return 2;
        }
    }

    if (options.interface_name == NULL || options.output_path == NULL || !acknowledge_preop)
    {
        usage(argv[0]);
        return 2;
    }

    return emaster_diagnostic_probe_run(&options);
}
