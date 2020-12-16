/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2017-2019. All rights reserved.
 * iSulad licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: tanyifeng
 * Create: 2017-11-22
 * Description: provide cni network plugin function definition
 **********************************************************************************/
#include "cni_network_plugin.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <utility>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <isula_libutils/log.h>
#include <isula_libutils/cni_anno_port_mappings.h>
#include "cri_helpers.h"
#include "cxxutils.h"
#include "utils.h"
#include "errors.h"
#include "service_container_api.h"

#include "network_api.h"

namespace Network {

void ProbeNetworkPlugins(const std::string &pluginDir, const std::string &binDir,
                         std::vector<std::shared_ptr<NetworkPlugin>> *plugins)
{
    const std::string useBinDir = binDir.empty() ? DEFAULT_CNI_DIR : binDir;
    std::vector<std::string> binDirs = CXXUtils::Split(useBinDir, ',');
    auto plugin = std::make_shared<CniNetworkPlugin>(binDirs, pluginDir);
    plugins->push_back(plugin);
}

CniNetworkPlugin::CniNetworkPlugin(std::vector<std::string> &binDirs, const std::string &confDir,
                                   const std::string &podCidr)
    : m_confDir(confDir), m_binDirs(binDirs), m_podCidr(podCidr), m_needFinish(false)
{
}

CniNetworkPlugin::~CniNetworkPlugin()
{
    m_needFinish = true;
    if (m_syncThread.joinable()) {
        m_syncThread.join();
    }
}

void CniNetworkPlugin::PlatformInit(Errors &error)
{
    char *tpath { nullptr };
    char *serr { nullptr };
    tpath = look_path(const_cast<char *>("nsenter"), &serr);
    if (tpath == nullptr) {
        error.SetError(serr);
        return;
    }
    m_nsenterPath = tpath;
    free(tpath);
}

void CniNetworkPlugin::SyncNetworkConfig()
{
    Errors err;
    WLockNetworkMap(err);
    if (err.NotEmpty()) {
        return;
    }

    if (network_module_update(NETWOKR_API_TYPE_CRI) != 0) {
        err.SetError("update cni conf list failed");
    }

    UnlockNetworkMap(err);
    if (err.NotEmpty()) {
        WARN("Unable to update cni config: %s", err.GetCMessage());
    }
}

void CniNetworkPlugin::Init(CRIRuntimeServiceImpl *criImpl, const std::string &hairpinMode,
                            const std::string &nonMasqueradeCIDR, int mtu, Errors &error)
{
    UNUSED(hairpinMode);
    UNUSED(nonMasqueradeCIDR);
    UNUSED(mtu);

    if (criImpl == nullptr) {
        error.Errorf("Empty runtime service");
        return;
    }
    PlatformInit(error);
    if (error.NotEmpty()) {
        return;
    }

    m_criImpl = criImpl;
    SyncNetworkConfig();

    // start a thread to sync network config from confDir periodically to detect network config updates in every 5 seconds
    m_syncThread = std::thread([&]() {
        UpdateDefaultNetwork();
    });
}

auto CniNetworkPlugin::Name() const -> const std::string &
{
    return CNI_PLUGIN_NAME;
}

void CniNetworkPlugin::CheckInitialized(Errors &err)
{
    RLockNetworkMap(err);
    if (err.NotEmpty()) {
        return;
    }

    if (!network_module_check(NETWOKR_API_TYPE_CRI)) {
        err.SetError("cni config uninitialized");
    }

    UnlockNetworkMap(err);
    if (err.NotEmpty()) {
        WARN("Unable to update cni config: %s", err.GetCMessage());
    }
}

void CniNetworkPlugin::Status(Errors &err)
{
    CheckInitialized(err);
}

static bool CheckCNIArgValue(const std::string &val)
{
    if (val.find(';') != std::string::npos) {
        return false;
    }
    if (std::count(val.begin(), val.end(), '=') != 1) {
        return false;
    }
    return true;
}

static void GetExtensionCNIArgs(const std::map<std::string, std::string> &annotations,
                                std::map<std::string, std::string> &args)
{
    // get cni multinetwork extension
    auto iter = annotations.find(CRIHelpers::Constants::CNI_MUTL_NET_EXTENSION_KEY);
    if (iter != annotations.end()) {
        if (!CheckCNIArgValue(iter->second)) {
            WARN("Ignore: invalid multinetwork cni args: %s", iter->second.c_str());
        } else {
            args[CRIHelpers::Constants::CNI_MUTL_NET_EXTENSION_ARGS_KEY] = iter->second;
        }
    }

    for (const auto &work : annotations) {
        if (work.first.find(CRIHelpers::Constants::CNI_ARGS_EXTENSION_PREFIX_KEY) != 0) {
            continue;
        }
        if (!CheckCNIArgValue(work.second)) {
            WARN("Ignore: invalid extension cni args: %s", work.second.c_str());
            continue;
        }
        auto strs = CXXUtils::Split(work.second, '=');
        iter = annotations.find(work.first);
        if (iter != annotations.end()) {
            WARN("Ignore: Same key cni args: %s", work.first.c_str());
            continue;
        }
        args[strs[0]] = strs[1];
    }
}

static void PrepareAdaptorArgs(const std::string &podName, const std::string &podNs, const std::string &podSandboxID,
                               const std::map<std::string, std::string> &annotations, const std::map<std::string, std::string> &options,
                               network_api_conf *config, Errors &err)
{
    size_t workLen;
    std::map<std::string, std::string> cniArgs;

    auto iter = options.find("UID");
    std::string podUID { "" };
    if (iter != options.end()) {
        podUID = iter->second;
    }

    cniArgs["K8S_POD_UID"] = podUID;
    cniArgs["IgnoreUnknown"] = "1";
    cniArgs["K8S_POD_NAMESPACE"] = podNs;
    cniArgs["K8S_POD_NAME"] = podName;
    cniArgs["K8S_POD_INFRA_CONTAINER_ID"] = podSandboxID;

    GetExtensionCNIArgs(annotations, cniArgs);
    workLen = cniArgs.size();

    config->args = (json_map_string_string *)util_common_calloc_s(sizeof(json_map_string_string));
    if (config->args == nullptr) {
        ERROR("Out of memory");
        goto err_out;
    }
    config->args->keys = (char **)util_smart_calloc_s(sizeof(char *), workLen);
    if (config->args->keys == nullptr) {
        ERROR("Out of memory");
        goto err_out;
    }
    config->args->values = (char **)util_smart_calloc_s(sizeof(char *), workLen);
    if (config->args->values == nullptr) {
        ERROR("Out of memory");
        goto err_out;
    }

    workLen = 0;
    for (const auto &work : cniArgs) {
        config->args->keys[workLen] = util_strdup_s(work.first.c_str());
        config->args->values[workLen] = util_strdup_s(work.second.c_str());
        config->args->len += 1;
        workLen++;
    }
    return;
err_out:
    err.SetError("prepare network api config failed");
}

static void PrepareAdaptorAttachNetworks(const std::map<std::string, std::string> &annotations,
                                         network_api_conf *config, Errors &err)
{
    cri_pod_network_container *networks = CRIHelpers::GetNetworkPlaneFromPodAnno(annotations, err);
    if (err.NotEmpty()) {
        ERROR("Couldn't get network plane from pod annotations: %s", err.GetCMessage());
        err.SetError("Prepare Adaptor Attach Networks failed");
        goto free_out;
    }
    if (networks == nullptr) {
        goto free_out;
    }
    config->extral_nets = (struct attach_net_conf **)util_smart_calloc_s(sizeof(struct attach_net_conf *), networks->len);
    if (config->extral_nets == nullptr) {
        ERROR("Out of memory");
        err.SetError("Prepare Adaptor Attach Networks failed");
        goto free_out;
    }

    for (size_t i = 0; i < networks->len; i++) {
        if (networks->items[i] == nullptr || networks->items[i]->name == nullptr || networks->items[i]->interface == nullptr) {
            continue;
        }
        config->extral_nets[i] = (struct attach_net_conf *)util_common_calloc_s(sizeof(struct attach_net_conf));
        if (config->extral_nets[i] == nullptr) {
            ERROR("Out of memory");
            err.SetError("Prepare Adaptor Attach Networks failed");
            goto free_out;
        }
        config->extral_nets[i]->name = util_strdup_s(networks->items[i]->name);
        config->extral_nets[i]->interface = util_strdup_s(networks->items[i]->interface);
        config->extral_nets_len += 1;
    }

free_out:
    free_cri_pod_network_container(networks);
}

static void PrepareAdaptorAnnotations(const std::map<std::string, std::string> &annos, network_api_conf *config,
                                      Errors &err)
{
    if (config->annotations == nullptr) {
        config->annotations = map_new(MAP_STR_STR, MAP_DEFAULT_CMP_FUNC, MAP_DEFAULT_FREE_FUNC);
    }
    if (config->annotations == nullptr) {
        err.SetError("Out of memory");
        ERROR("Out of memory");
        return;
    }

    auto iter = annos.find(CRIHelpers::Constants::POD_CHECKPOINT_KEY);
    std::string jsonCheckpoint;

    if (iter != annos.end()) {
        jsonCheckpoint = iter->second;
    }
    if (jsonCheckpoint.empty()) {
        return;
    }
    DEBUG("add checkpoint: %s", jsonCheckpoint.c_str());

    cri::PodSandboxCheckpoint checkpoint;
    CRIHelpers::GetCheckpoint(jsonCheckpoint, checkpoint, err);
    if (err.NotEmpty() || checkpoint.GetData() == nullptr) {
        err.Errorf("could not retrieve port mappings: %s", err.GetCMessage());
        return;
    }
    if (checkpoint.GetData()->GetPortMappings().size() == 0) {
        return;
    }

    parser_error jerr = nullptr;
    char *tmpVal = nullptr;
    size_t i = 0;
    cni_anno_port_mappings_container *cni_pms = (cni_anno_port_mappings_container *)util_common_calloc_s(sizeof(
                                                                                                             cni_anno_port_mappings_container));
    if (cni_pms == nullptr) {
        ERROR("Out of memory");
        err.SetError("Out of memory");
        goto free_out;
    }
    cni_pms->items = (cni_anno_port_mappings_element **)util_smart_calloc_s(sizeof(cni_anno_port_mappings_element *),
                                                                            checkpoint.GetData()->GetPortMappings().size());

    for (const auto &pm : checkpoint.GetData()->GetPortMappings()) {
        cni_anno_port_mappings_element *elem = (cni_anno_port_mappings_element *)util_common_calloc_s(sizeof(
                                                                                                          cni_anno_port_mappings_element));
        if (elem == nullptr) {
            ERROR("Out of memory");
            err.SetError("Out of memory");
            goto free_out;
        }
        if (pm.GetHostPort() != nullptr && *pm.GetHostPort() > 0) {
            elem->host_port = *pm.GetHostPort();
        }
        if (pm.GetContainerPort() != nullptr) {
            elem->container_port = *pm.GetContainerPort();
        }
        if (pm.GetProtocol() != nullptr) {
            elem->protocol = util_strdup_s(pm.GetProtocol()->c_str());
        }
        cni_pms->items[i++] = elem;
        cni_pms->len += 1;
    }
    tmpVal = cni_anno_port_mappings_container_generate_json(cni_pms, nullptr, &jerr);
    if (network_module_insert_portmapping(tmpVal, config) != 0) {
        err.SetError("add portmappings failed");
    }
    free(tmpVal);

free_out:
    free(jerr);
    free_cni_anno_port_mappings_container(cni_pms);
}

void BuildAdaptorCNIConfig(const std::string &ns, const std::string &defaultInterface, const std::string &name,
                           const std::string &netnsPath, const std::string &podSandboxID,
                           const std::map<std::string, std::string> &annotations,
                           const std::map<std::string, std::string> &options, network_api_conf **api_conf, Errors &err)
{
    network_api_conf *config = nullptr;

    config = (network_api_conf *)util_common_calloc_s(sizeof(network_api_conf));
    if (config == nullptr) {
        ERROR("Out of memory");
        err.SetError("Out of memory");
        return;
    }

    // fill attach network names for pod
    PrepareAdaptorAttachNetworks(annotations, config, err);
    if (err.NotEmpty()) {
        goto err_out;
    }

    // fill args for cni plugin
    PrepareAdaptorArgs(name, ns, podSandboxID, annotations, options, config, err);
    if (err.NotEmpty()) {
        goto err_out;
    }

    // fill annotations for cni runtime config
    // 1. portmappings;
    // 2. iprange;
    PrepareAdaptorAnnotations(annotations, config, err);

    config->name = util_strdup_s(name.c_str());
    config->ns = util_strdup_s(ns.c_str());
    config->pod_id = util_strdup_s(podSandboxID.c_str());
    config->netns_path = util_strdup_s(netnsPath.c_str());
    if (!defaultInterface.empty()) {
        config->default_interface = util_strdup_s(defaultInterface.c_str());
    }

    *api_conf = config;
    config = nullptr;
    return;
err_out:
    err.AppendError("BuildAdaptorCNIConfig failed");
    free_network_api_conf(config);
}

auto CniNetworkPlugin::GetNetNS(const std::string &podSandboxID, Errors &err) -> std::string
{
    int ret = 0;
    char fullpath[PATH_MAX] { 0 };
    std::string result;
    const std::string NetNSFmt { "/proc/%d/ns/net" };

    container_inspect *inspect_data = CRIHelpers::InspectContainer(podSandboxID, err, false);
    if (inspect_data == nullptr) {
        goto cleanup;
    }
    if (inspect_data->state->pid == 0) {
        err.Errorf("cannot find network namespace for the terminated container %s", podSandboxID.c_str());
        goto cleanup;
    }
    ret = snprintf(fullpath, sizeof(fullpath), NetNSFmt.c_str(), inspect_data->state->pid);
    if ((size_t)ret >= sizeof(fullpath) || ret < 0) {
        err.SetError("Sprint nspath failed");
        goto cleanup;
    }
    result = fullpath;

cleanup:
    free_container_inspect(inspect_data);
    return result;
}


void CniNetworkPlugin::SetUpPod(const std::string &ns, const std::string &name, const std::string &interfaceName,
                                const std::string &id, const std::map<std::string, std::string> &annotations,
                                const std::map<std::string, std::string> &options, Errors &err)
{
    CheckInitialized(err);
    if (err.NotEmpty()) {
        return;
    }
    std::string netnsPath = GetNetNS(id, err);
    if (err.NotEmpty()) {
        ERROR("CNI failed to retrieve network namespace path: %s", err.GetCMessage());
        return;
    }

    network_api_conf *config = nullptr;
    BuildAdaptorCNIConfig(ns, interfaceName, name, netnsPath, id, annotations, options, &config, err);
    if (err.NotEmpty()) {
        ERROR("build network api config failed");
        return;
    }

    RLockNetworkMap(err);
    if (err.NotEmpty()) {
        ERROR("%s", err.GetCMessage());
        return;
    }

    // TODO: parse result of attach
    network_api_result_list *result = nullptr;
    if (network_module_attach(config, NETWOKR_API_TYPE_CRI, &result) != 0) {
        err.Errorf("setup cni for container: %s failed", id.c_str());
    }

    UnlockNetworkMap(err);
    free_network_api_result_list(result);
    free_network_api_conf(config);
}

void CniNetworkPlugin::TearDownPod(const std::string &ns, const std::string &name, const std::string &interfaceName,
                                   const std::string &id, const std::map<std::string, std::string> &annotations,
                                   Errors &err)
{
    CheckInitialized(err);
    if (err.NotEmpty()) {
        return;
    }
    Errors tmpErr;

    std::string netnsPath = GetNetNS(id, err);
    if (err.NotEmpty()) {
        WARN("CNI failed to retrieve network namespace path: %s", err.GetCMessage());
        err.Clear();
    }

    std::map<std::string, std::string> tmpOpts;
    network_api_conf *config = nullptr;
    BuildAdaptorCNIConfig(ns, interfaceName, name, netnsPath, id, annotations, tmpOpts, &config, err);
    if (err.NotEmpty()) {
        ERROR("build network api config failed");
        return;
    }

    RLockNetworkMap(err);
    if (err.NotEmpty()) {
        ERROR("get lock failed: %s", err.GetCMessage());
        return;
    }

    if (network_module_detach(config, NETWOKR_API_TYPE_CRI) != 0) {
        err.Errorf("teardown cni for container: %s failed", id.c_str());
    }

    UnlockNetworkMap(err);
    free_network_api_conf(config);
}

auto CniNetworkPlugin::Capabilities() -> std::map<int, bool> *
{
    return m_noop.Capabilities();
}

void CniNetworkPlugin::SetPodCidr(const std::string &podCidr)
{
    Errors err;

    WLockNetworkMap(err);
    if (err.NotEmpty()) {
        ERROR("%s", err.GetCMessage());
        return;
    }

    if (!m_podCidr.empty()) {
        WARN("Ignoring subsequent pod CIDR update to %s", podCidr.c_str());
        goto unlock_out;
    }

    m_podCidr = podCidr;

unlock_out:
    UnlockNetworkMap(err);
}

void CniNetworkPlugin::Event(const std::string &name, std::map<std::string, std::string> &details)
{
    if (name != CRIHelpers::Constants::NET_PLUGIN_EVENT_POD_CIDR_CHANGE) {
        return;
    }

    auto iter = details.find(CRIHelpers::Constants::NET_PLUGIN_EVENT_POD_CIDR_CHANGE_DETAIL_CIDR);
    if (iter == details.end()) {
        WARN("%s event didn't contain pod CIDR", CRIHelpers::Constants::NET_PLUGIN_EVENT_POD_CIDR_CHANGE.c_str());
        return;
    }

    SetPodCidr(iter->second);
}

void CniNetworkPlugin::GetPodNetworkStatus(const std::string & /*ns*/, const std::string & /*name*/,
                                           const std::string &interfaceName, const std::string &podSandboxID,
                                           PodNetworkStatus &status, Errors &err)
{
    std::string netnsPath;
    std::vector<std::string> ips;
    Errors tmpErr;

    if (podSandboxID.empty()) {
        err.SetError("Empty podsandbox ID");
        goto out;
    }

    netnsPath = GetNetNS(podSandboxID, tmpErr);
    if (tmpErr.NotEmpty()) {
        err.Errorf("CNI failed to retrieve network namespace path: %s", tmpErr.GetCMessage());
        goto out;
    }
    if (netnsPath.empty()) {
        err.Errorf("Cannot find the network namespace, skipping pod network status for container %s",
                   podSandboxID.c_str());
        goto out;
    }
    GetPodIP(m_nsenterPath, netnsPath, interfaceName, ips, err);
    if (err.NotEmpty()) {
        ERROR("GetPodIP failed: %s", err.GetCMessage());
        goto out;
    }
    status.SetIPs(ips);

out:
    INFO("Get pod: %s network status success", podSandboxID.c_str());
}

void CniNetworkPlugin::RLockNetworkMap(Errors &error)
{
    int ret = pthread_rwlock_rdlock(&m_netsLock);
    if (ret != 0) {
        error.Errorf("Failed to get read lock");
        ERROR("Get read lock failed: %s", strerror(ret));
    }
}

void CniNetworkPlugin::WLockNetworkMap(Errors &error)
{
    int ret = pthread_rwlock_wrlock(&m_netsLock);
    if (ret != 0) {
        error.Errorf("Failed to get write lock");
        ERROR("Get write lock failed: %s", strerror(ret));
    }
}

void CniNetworkPlugin::UnlockNetworkMap(Errors &error)
{
    int ret = pthread_rwlock_unlock(&m_netsLock);
    if (ret != 0) {
        error.Errorf("Failed to unlock");
        ERROR("Unlock failed: %s", strerror(ret));
    }
}

void CniNetworkPlugin::UpdateDefaultNetwork()
{
    const int defaultSyncConfigCnt = 5;
    const int defaultSyncConfigPeriod = 1000;

    pthread_setname_np(pthread_self(), "CNIUpdater");

    while (true) {
        for (int i = 0; i < defaultSyncConfigCnt; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(defaultSyncConfigPeriod));
            if (m_needFinish) {
                return;
            }
        }
        SyncNetworkConfig();
    }
}

} // namespace Network
