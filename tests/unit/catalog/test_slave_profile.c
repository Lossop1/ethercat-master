#include "emaster/catalog/slave_profile.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(expression)                                                                       \
    do                                                                                          \
    {                                                                                           \
        if (!(expression))                                                                      \
        {                                                                                       \
            fprintf(stderr, "检查失败：%s:%d: %s\n", __FILE__, __LINE__, #expression);        \
            ++failures;                                                                         \
        }                                                                                       \
    } while (0)

static void test_generated_profile(void)
{
    const emaster_slave_profile_t *profile =
        emaster_slave_profile_by_id("cyberbeast.isvd90rc-300b-100-70.rev1");
    const size_t profile_count = emaster_slave_profile_count();

    /* 测试当前已知型号存在，但不把目录总数固定为当前设备数量。 */
    CHECK(profile_count > 0U);
    CHECK(profile != NULL);
    if (profile == NULL)
    {
        return;
    }

    CHECK(strcmp(profile->model, "ISVD90RC-300B-100-70") == 0);
    CHECK(profile->identity.vendor_id == UINT32_C(0x000C0B00));
    CHECK(profile->identity.product_code == UINT32_C(0x00080117));
    CHECK(profile->identity.revision == UINT32_C(0x00000001));
    CHECK(profile->rx_pdo_index == UINT16_C(0x1600));
    CHECK(profile->tx_pdo_index == UINT16_C(0x1A00));
    CHECK(profile->rx_pdo_bytes == UINT16_C(14));
    CHECK(profile->tx_pdo_bytes == UINT16_C(14));
    CHECK(profile->requires_distributed_clocks);
    CHECK(emaster_slave_profile_at(profile_count) == NULL);
    CHECK(emaster_slave_profile_by_id("unknown.profile") == NULL);
    CHECK(emaster_slave_profile_by_id(NULL) == NULL);
}

static void test_identity_matching(void)
{
    const emaster_slave_profile_t *profile = emaster_slave_profile_at(0U);
    emaster_slave_identity_t actual = {
        .vendor_id = UINT32_C(0x000C0B00),
        .product_code = UINT32_C(0x00080117),
        .revision = UINT32_C(0x00000001),
    };

    CHECK(profile != NULL);
    CHECK(emaster_slave_identity_matches(profile, &actual));
    actual.revision = UINT32_C(2);
    CHECK(!emaster_slave_identity_matches(profile, &actual));
    CHECK(!emaster_slave_identity_matches(NULL, &actual));
    CHECK(!emaster_slave_identity_matches(profile, NULL));
}

int main(void)
{
    test_generated_profile();
    test_identity_matching();

    if (failures != 0)
    {
        fprintf(stderr, "%d 个测试断言失败\n", failures);
        return 1;
    }
    return 0;
}
