#pragma once

#include <memory>
#include <map>

#include "envoy/tcp/conn_pool.h"
#include "envoy/upstream/thread_local_cluster.h"
#include "envoy/upstream/load_balancer.h"
#include "source/common/buffer/buffer_impl.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace MetaProtocolProxy {

class UpstreamHandler : public Tcp::ConnectionPool::Callbacks,
                        public Tcp::ConnectionPool::UpstreamCallbacks,
                        Logger::Loggable<Logger::Id::filter> {
public:
  using DeleteCallbackType = std::function<void(const std::string&)>;
  UpstreamHandler(const std::string& key, Network::Connection& connection,
                  DeleteCallbackType delete_callback)
      : key_(key), downstream_connection_(connection), delete_callback_(delete_callback) {}
  ~UpstreamHandler() override;

  int start(Upstream::TcpPoolData& pool_data);

  void onData(Buffer::Instance& data, bool end_stream);

  static absl::optional<Upstream::TcpPoolData>
  createTcpPoolData(Upstream::ThreadLocalCluster& thread_local_cluster,
                    Upstream::LoadBalancerContext& context);

  // Tcp::ConnectionPool::Callbacks
  void onPoolFailure(ConnectionPool::PoolFailureReason reason,
                     absl::string_view transport_failure_reason,
                     Upstream::HostDescriptionConstSharedPtr host) override;
  void onPoolReady(Tcp::ConnectionPool::ConnectionDataPtr&& conn_data,
                   Upstream::HostDescriptionConstSharedPtr host) override;

  // Tcp::ConnectionPool::UpstreamCallbacks
  void onUpstreamData(Buffer::Instance& data, bool end_stream) override;
  void onEvent(Network::ConnectionEvent event) override;
  void onAboveWriteBufferHighWatermark() override {}
  void onBelowWriteBufferLowWatermark() override {}

private:
  void onClose();

private:
  std::string key_;
  Network::Connection& downstream_connection_;
  DeleteCallbackType delete_callback_;
  Tcp::ConnectionPool::Cancellable* upstream_handle_{};
  Tcp::ConnectionPool::ConnectionDataPtr conn_data_;
  Upstream::HostDescriptionConstSharedPtr upstream_host_;

  // 连接建立过程中缓存下消息
  std::vector<std::pair<Buffer::OwnedImpl, bool>> request_caches_;
};

class UpstreamHandlerManager : Logger::Loggable<Logger::Id::filter> {
public:
  void add(const std::string& key, std::shared_ptr<UpstreamHandler> client);
  void del(const std::string& key);
  std::shared_ptr<UpstreamHandler> get(const std::string& key);
  void clear();

private:
  // key: clusterName or clusterName_address
  std::map<std::string, std::shared_ptr<UpstreamHandler>> upstream_handlers_;
};

} // namespace MetaProtocolProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
