// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ray/gcs/gcs_server/gcs_resource_manager.h"

#include "ray/common/ray_config.h"
#include "ray/gcs/gcs_server/ray_syncer.h"
#include "ray/stats/metric_defs.h"

namespace ray {
namespace gcs {

GcsResourceManager::GcsResourceManager(
    instrumented_io_context &main_io_service, std::shared_ptr<GcsPublisher> gcs_publisher,
    std::shared_ptr<gcs::GcsTableStorage> gcs_table_storage,
    syncer::RaySyncer *ray_syncer)
    : main_io_service_(main_io_service),
      periodical_runner_(main_io_service),
      gcs_publisher_(gcs_publisher),
      gcs_table_storage_(gcs_table_storage),
      ray_syncer_(ray_syncer) {}

void GcsResourceManager::HandleGetResources(const rpc::GetResourcesRequest &request,
                                            rpc::GetResourcesReply *reply,
                                            rpc::SendReplyCallback send_reply_callback) {
  NodeID node_id = NodeID::FromBinary(request.node_id());
  auto iter = cluster_scheduling_resources_.find(node_id);
  if (iter != cluster_scheduling_resources_.end()) {
    const auto &resource_map = iter->second->GetTotalResources().GetResourceMap();
    rpc::ResourceTableData resource_table_data;
    for (const auto &resource : resource_map) {
      resource_table_data.set_resource_capacity(resource.second);
      (*reply->mutable_resources())[resource.first] = resource_table_data;
    }
  }
  GCS_RPC_SEND_REPLY(send_reply_callback, reply, Status::OK());
  ++counts_[CountType::GET_RESOURCES_REQUEST];
}

void GcsResourceManager::UpdateResources(
    const NodeID &node_id, const std::unordered_map<std::string, double> &resources,
    std::function<void(const Status &)> callback) {
  RAY_LOG(DEBUG) << "Updating resources, node id = " << node_id;
  auto iter = cluster_scheduling_resources_.find(node_id);
  if (iter != cluster_scheduling_resources_.end()) {
    // Update `cluster_scheduling_resources_`.
    SchedulingResources &scheduling_resources = *iter->second;
    for (const auto &entry : *changed_resources) {
      scheduling_resources.UpdateResourceCapacity(entry.first, entry.second);
    }

    // Update gcs storage.
    rpc::ResourceMap resource_map;
    for (const auto &entry : scheduling_resources.GetTotalResources().GetResourceMap()) {
      (*resource_map.mutable_items())[entry.first].set_resource_capacity(entry.second);
    }
    for (const auto &entry : *changed_resources) {
      (*resource_map.mutable_items())[entry.first].set_resource_capacity(entry.second);
    }

    auto start = absl::GetCurrentTimeNanos();
    auto on_done = [this, node_id, start,
                    callback = std::move(callback)](const Status &status) {
      auto end = absl::GetCurrentTimeNanos();
      ray::stats::STATS_gcs_new_resource_creation_latency_ms.Record(
          absl::Nanoseconds(end - start) / absl::Milliseconds(1));
      RAY_CHECK_OK(status);
      RAY_LOG(DEBUG) << "Finished updating resources, node id = " << node_id;
      callback(status);
    };

    RAY_CHECK_OK(
        gcs_table_storage_->NodeResourceTable().Put(node_id, resource_map, on_done));
  } else {
    RAY_LOG(ERROR) << "Failed to update resources as node " << node_id
                   << " is not registered.";
    main_io_service_.post(
        std::bind(std::move(callback), Status::Invalid("Node does not exist.")),
        "UpdateResourcesCallback");
  }
}

void GcsResourceManager::HandleDeleteResources(
    const NodeID &node_id, const std::vector<std::string> &resource_names,
    std::function<void(const Status &)> callback) {
  RAY_LOG(DEBUG) << "Deleting node resources, node id = " << node_id;
  auto iter = cluster_scheduling_resources_.find(node_id);
  if (iter != cluster_scheduling_resources_.end()) {
    // Update `cluster_scheduling_resources_`.
    for (const auto &resource_name : resource_names) {
      iter->second->DeleteResource(resource_name);
    }

    // Update gcs storage.
    rpc::ResourceMap resource_map;
    auto resources = iter->second->GetTotalResources().GetResourceMap();
    for (const auto &resource_name : resource_names) {
      resources.erase(resource_name);
    }
    for (const auto &entry : resources) {
      (*resource_map.mutable_items())[entry.first].set_resource_capacity(entry.second);
    }

    auto on_done = [this, callback = std::move(callback)](const Status &status) {
      RAY_CHECK_OK(status);
      callback(status);
    };
    RAY_CHECK_OK(
        gcs_table_storage_->NodeResourceTable().Put(node_id, resource_map, on_done));
  } else {
    main_io_service_.post(
        std::bind(std::move(callback), Status::Invalid("Node does not exist.")),
        "DeleteResourcesCallback");
    RAY_LOG(DEBUG) << "Finished deleting node resources, node id = " << node_id;
  }
}

void GcsResourceManager::HandleGetAllAvailableResources(
    const rpc::GetAllAvailableResourcesRequest &request,
    rpc::GetAllAvailableResourcesReply *reply,
    rpc::SendReplyCallback send_reply_callback) {
  for (const auto &iter : cluster_scheduling_resources_) {
    rpc::AvailableResources resource;
    resource.set_node_id(iter.first.Binary());
    for (const auto &res : iter.second->GetAvailableResources().GetResourceAmountMap()) {
      (*resource.mutable_resources_available())[res.first] = res.second.Double();
    }
    reply->add_resources_list()->CopyFrom(resource);
  }
  GCS_RPC_SEND_REPLY(send_reply_callback, reply, Status::OK());
  ++counts_[CountType::GET_ALL_AVAILABLE_RESOURCES_REQUEST];
}

void GcsResourceManager::UpdateFromResourceReport(const rpc::ResourcesData &data) {
  NodeID node_id = NodeID::FromBinary(data.node_id());
  if (RayConfig::instance().gcs_actor_scheduling_enabled()) {
    UpdateNodeNormalTaskResources(node_id, data);
  } else {
    if (node_resource_usages_.count(node_id) == 0 || data.resources_available_changed()) {
      const auto &resource_changed = MapFromProtobuf(data.resources_available());
      SetAvailableResources(node_id, ResourceSet(resource_changed));
    }
  }

  UpdateNodeResourceUsage(node_id, data);

  // TODO (iycheng): This will only happen in testing. We'll clean this code path
  // in follow up PRs.
  if (ray_syncer_ != nullptr) {
    ray_syncer_->Update(data);
  }
}

void GcsResourceManager::HandleReportResourceUsage(
    const rpc::ReportResourceUsageRequest &request, rpc::ReportResourceUsageReply *reply,
    rpc::SendReplyCallback send_reply_callback) {
  UpdateFromResourceReport(request.resources());

  GCS_RPC_SEND_REPLY(send_reply_callback, reply, Status::OK());
  ++counts_[CountType::REPORT_RESOURCE_USAGE_REQUEST];
}

void GcsResourceManager::HandleGetAllResourceUsage(
    const rpc::GetAllResourceUsageRequest &request, rpc::GetAllResourceUsageReply *reply,
    rpc::SendReplyCallback send_reply_callback) {
  if (!node_resource_usages_.empty()) {
    auto batch = std::make_shared<rpc::ResourceUsageBatchData>();
    absl::flat_hash_map<ResourceSet, rpc::ResourceDemand> aggregate_load;
    for (const auto &usage : node_resource_usages_) {
      // Aggregate the load reported by each raylet.
      auto load = usage.second.resource_load_by_shape();
      for (const auto &demand : load.resource_demands()) {
        auto scheduling_key = ResourceSet(MapFromProtobuf(demand.shape()));
        auto &aggregate_demand = aggregate_load[scheduling_key];
        aggregate_demand.set_num_ready_requests_queued(
            aggregate_demand.num_ready_requests_queued() +
            demand.num_ready_requests_queued());
        aggregate_demand.set_num_infeasible_requests_queued(
            aggregate_demand.num_infeasible_requests_queued() +
            demand.num_infeasible_requests_queued());
        aggregate_demand.set_backlog_size(aggregate_demand.backlog_size() +
                                          demand.backlog_size());
      }

      batch->add_batch()->CopyFrom(usage.second);
    }

    for (const auto &demand : aggregate_load) {
      auto demand_proto = batch->mutable_resource_load_by_shape()->add_resource_demands();
      demand_proto->CopyFrom(demand.second);
      for (const auto &resource_pair : demand.first.GetResourceMap()) {
        (*demand_proto->mutable_shape())[resource_pair.first] = resource_pair.second;
      }
    }

    // Update placement group load to heartbeat batch.
    // This is updated only one per second.
    if (placement_group_load_.has_value()) {
      auto placement_group_load = placement_group_load_.value();
      auto placement_group_load_proto = batch->mutable_placement_group_load();
      placement_group_load_proto->CopyFrom(*placement_group_load.get());
    }
    reply->mutable_resource_usage_data()->CopyFrom(*batch);
  }

  GCS_RPC_SEND_REPLY(send_reply_callback, reply, Status::OK());
  ++counts_[CountType::GET_ALL_RESOURCE_USAGE_REQUEST];
}

void GcsResourceManager::UpdateNodeResourceUsage(const NodeID &node_id,
                                                 const rpc::ResourcesData &resources) {
  auto iter = node_resource_usages_.find(node_id);
  if (iter == node_resource_usages_.end()) {
    auto resources_data = std::make_shared<rpc::ResourcesData>();
    resources_data->CopyFrom(resources);
    node_resource_usages_[node_id] = *resources_data;
  } else {
    if (resources.resources_total_size() > 0) {
      (*iter->second.mutable_resources_total()) = resources.resources_total();
    }
    if (resources.resources_available_changed()) {
      (*iter->second.mutable_resources_available()) = resources.resources_available();
    }
    if (resources.resource_load_changed()) {
      (*iter->second.mutable_resource_load()) = resources.resource_load();
    }
    if (resources.resources_normal_task_changed()) {
      (*iter->second.mutable_resources_normal_task()) = resources.resources_normal_task();
    }
    (*iter->second.mutable_resource_load_by_shape()) = resources.resource_load_by_shape();
    iter->second.set_cluster_full_of_actors_detected(
        resources.cluster_full_of_actors_detected());
  }
}

void GcsResourceManager::Initialize(const GcsInitData &gcs_init_data) {
  const auto &nodes = gcs_init_data.Nodes();
  for (const auto &entry : nodes) {
    if (entry.second.state() == rpc::GcsNodeInfo::ALIVE) {
      OnNodeAdd(entry.second);
    }
  }

  const auto &cluster_resources = gcs_init_data.ClusterResources();
  for (const auto &entry : cluster_resources) {
    const auto &iter = cluster_scheduling_resources_.find(entry.first);
    if (iter != cluster_scheduling_resources_.end()) {
      for (const auto &resource : entry.second.items()) {
        iter->second->UpdateResourceCapacity(resource.first,
                                             resource.second.resource_capacity());
      }
    }
  }
}

const absl::flat_hash_map<NodeID, std::shared_ptr<SchedulingResources>>
    &GcsResourceManager::GetClusterResources() const {
  return cluster_scheduling_resources_;
}

void GcsResourceManager::SetAvailableResources(const NodeID &node_id,
                                               const ResourceSet &resources) {
  auto iter = cluster_scheduling_resources_.find(node_id);
  if (iter != cluster_scheduling_resources_.end()) {
    iter->second->SetAvailableResources(ResourceSet(resources));
  } else {
    RAY_LOG(WARNING)
        << "Skip the setting of available resources of node " << node_id
        << " as it does not exist, maybe it is not registered yet or is already dead.";
  }
}

void GcsResourceManager::DeleteResources(
    const NodeID &node_id, const std::vector<std::string> &deleted_resources) {
  auto iter = cluster_scheduling_resources_.find(node_id);
  if (iter != cluster_scheduling_resources_.end()) {
    for (const auto &resource_name : deleted_resources) {
      iter->second->DeleteResource(resource_name);
    }
  }
}

void GcsResourceManager::OnNodeAdd(const rpc::GcsNodeInfo &node) {
  auto node_id = NodeID::FromBinary(node.node_id());
  if (!cluster_scheduling_resources_.contains(node_id)) {
    absl::flat_hash_map<std::string, double> resource_mapping(
        node.resources_total().begin(), node.resources_total().end());
    // Update the cluster scheduling resources as new node is added.
    ResourceSet node_resources(resource_mapping);
    cluster_scheduling_resources_.emplace(
        node_id, std::make_shared<SchedulingResources>(node_resources));
  }
}

void GcsResourceManager::OnNodeDead(const NodeID &node_id) {
  node_resource_usages_.erase(node_id);
  cluster_scheduling_resources_.erase(node_id);
  latest_resources_normal_task_timestamp_.erase(node_id);
}

bool GcsResourceManager::AcquireResources(const NodeID &node_id,
                                          const ResourceSet &required_resources) {
  auto iter = cluster_scheduling_resources_.find(node_id);
  if (iter != cluster_scheduling_resources_.end()) {
    if (!required_resources.IsSubset(iter->second->GetAvailableResources())) {
      return false;
    }
    iter->second->Acquire(required_resources);
  }
  // If node dead, we will not find the node. This is a normal scenario, so it returns
  // true.
  return true;
}

bool GcsResourceManager::ReleaseResources(const NodeID &node_id,
                                          const ResourceSet &acquired_resources) {
  auto iter = cluster_scheduling_resources_.find(node_id);
  if (iter != cluster_scheduling_resources_.end()) {
    iter->second->Release(acquired_resources);
  }
  // If node dead, we will not find the node. This is a normal scenario, so it returns
  // true.
  return true;
}

void GcsResourceManager::UpdatePlacementGroupLoad(
    const std::shared_ptr<rpc::PlacementGroupLoad> placement_group_load) {
  placement_group_load_ = absl::make_optional(placement_group_load);
}

std::string GcsResourceManager::DebugString() const {
  std::ostringstream stream;
  stream << "GcsResourceManager: "
         << "\n- GetResources request count: "
         << counts_[CountType::GET_RESOURCES_REQUEST]
         << "\n- GetAllAvailableResources request count"
         << counts_[CountType::GET_ALL_AVAILABLE_RESOURCES_REQUEST]
         << "\n- UpdateResources request count: "
         << counts_[CountType::UPDATE_RESOURCES_REQUEST]
         << "\n- DeleteResources request count: "
         << counts_[CountType::DELETE_RESOURCES_REQUEST]
         << "\n- ReportResourceUsage request count: "
         << counts_[CountType::REPORT_RESOURCE_USAGE_REQUEST]
         << "\n- GetAllResourceUsage request count: "
         << counts_[CountType::GET_ALL_RESOURCE_USAGE_REQUEST];
  return stream.str();
}

void GcsResourceManager::AddResourcesChangedListener(std::function<void()> listener) {
  RAY_CHECK(listener != nullptr);
  resources_changed_listeners_.emplace_back(std::move(listener));
}

void GcsResourceManager::UpdateNodeNormalTaskResources(
    const NodeID &node_id, const rpc::ResourcesData &heartbeat) {
  auto iter = cluster_scheduling_resources_.find(node_id);
  if (iter == cluster_scheduling_resources_.end()) {
    return;
  }

  auto &scheduling_resoruces = iter->second;
  ResourceSet resources_normal_task(MapFromProtobuf(heartbeat.resources_normal_task()));
  if (heartbeat.resources_normal_task_changed() &&
      heartbeat.resources_normal_task_timestamp() >
          latest_resources_normal_task_timestamp_[node_id] &&
      !resources_normal_task.IsEqual(scheduling_resoruces->GetNormalTaskResources())) {
    scheduling_resoruces->SetNormalTaskResources(resources_normal_task);
    latest_resources_normal_task_timestamp_[node_id] =
        heartbeat.resources_normal_task_timestamp();
    for (const auto &listener : resources_changed_listeners_) {
      listener();
    }
  }
}

std::string GcsResourceManager::ToString() const {
  std::ostringstream ostr;
  const int indent = 0;
  std::string indent_0(indent + 0 * 2, ' ');
  std::string indent_1(indent + 1 * 2, ' ');
  ostr << "{\n";
  for (const auto &entry : cluster_scheduling_resources_) {
    ostr << indent_1 << entry.first << " : " << entry.second->DebugString() << ",\n";
  }
  ostr << indent_0 << "}\n";
  return ostr.str();
}

}  // namespace gcs
}  // namespace ray
