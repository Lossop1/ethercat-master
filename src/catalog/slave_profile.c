#include "emaster/catalog/slave_profile.h"

#include <string.h>

bool emaster_slave_identity_matches(const emaster_slave_profile_t *profile,
                                    const emaster_slave_identity_t *actual)
{
    if (profile == NULL || actual == NULL)
    {
        return false;
    }

    return profile->identity.vendor_id == actual->vendor_id &&
           profile->identity.product_code == actual->product_code &&
           profile->identity.revision == actual->revision;
}

const emaster_slave_profile_t *emaster_slave_profile_by_id(const char *profile_id)
{
    size_t index;

    if (profile_id == NULL)
    {
        return NULL;
    }

    for (index = 0; index < emaster_slave_profile_count(); ++index)
    {
        const emaster_slave_profile_t *profile = emaster_slave_profile_at(index);
        if (profile != NULL && strcmp(profile->profile_id, profile_id) == 0)
        {
            return profile;
        }
    }

    return NULL;
}
