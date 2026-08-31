#ifndef EMASTER_CONFIG_RUNTIME_CONFIG_H
#define EMASTER_CONFIG_RUNTIME_CONFIG_H

#include <stddef.h>
#include <stdint.h>

/*
 * 拓扑条目只保存用户在拓扑配置中确认的静态关系。position 是总线顺序，
 * profile_id 用于在设备目录中选择身份和 PDO 事实；本模块不解释过程数据。
 */
typedef struct
{
    uint16_t position;
    const char *axis_id;
    const char *profile_id;
} emaster_topology_slave_config_t;

/* status 原样保存用户给出的配置事实，基础设施不自行解释审批语义。 */
typedef struct
{
    const char *topology_id;
    const char *status;
    const emaster_topology_slave_config_t *slaves;
    size_t slave_count;
} emaster_topology_config_t;

/* 部署配置把主机、专用 EtherCAT 网口和逻辑拓扑绑定在一起。 */
typedef struct
{
    const char *deployment_id;
    const char *hostname;
    const char *ethercat_interface;
    const char *management_interface;
    const emaster_topology_config_t *topology;
} emaster_deployment_config_t;

/* 以下接口只返回生成目录中的只读对象，调用者不得释放或修改返回值。 */
size_t emaster_topology_config_count(void);
const emaster_topology_config_t *emaster_topology_config_at(size_t index);
const emaster_topology_config_t *emaster_topology_config_by_id(const char *topology_id);

size_t emaster_deployment_config_count(void);
const emaster_deployment_config_t *emaster_deployment_config_at(size_t index);
const emaster_deployment_config_t *emaster_deployment_config_by_id(const char *deployment_id);

#endif
