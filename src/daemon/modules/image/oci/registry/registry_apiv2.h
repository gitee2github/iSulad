/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2020. All rights reserved.
 * iSulad licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: wangfengtu
 * Create: 2020-03-05
 * Description: provide registry api v2 definition
 ******************************************************************************/
#ifndef DAEMON_MODULES_IMAGE_OCI_REGISTRY_REGISTRY_APIV2_H
#define DAEMON_MODULES_IMAGE_OCI_REGISTRY_REGISTRY_APIV2_H

#include <stddef.h>

#include "registry_type.h"

#ifdef __cplusplus
extern "C" {
#endif

int fetch_manifest(pull_descriptor *desc);

int fetch_config(pull_descriptor *desc);

int fetch_layer(pull_descriptor *desc, size_t index);

int login_to_registry(pull_descriptor *desc);

int fetch_catalog(search_descriptor *desc);

int fetch_tags(search_descriptor *desc, char** output_buffer);

#ifdef __cplusplus
}
#endif

#endif

