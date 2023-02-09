#include "src/meta_protocol_proxy/upstream_handler.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace MetaProtocolProxy {

UpstreamHandler::~UpstreamHandler() {
  ENVOY_LOG(trace, "********** UpstreamHandler destructed ***********");
  if (upstream_handle_ != nullptr) {
    upstream_handle_->cancel(ConnectionPool::CancelPolicy::CloseExcess);
    upstream_handle_ = nullptr;
    ENVOY_LOG(info, "UpstreamHandler: reset connection pool handler");
  }
}

void UpstreamHandler::onClose() {
  ENVOY_LOG(info, "UpstreamHandler[{}] onClose", key_);
  delete_callback_(key_);

  if (upstream_handle_) {
    ASSERT(!conn_data_);
    upstream_handle_->cancel(Tcp::ConnectionPool::CancelPolicy::Default);
    upstream_handle_ = nullptr;
    ENVOY_LOG(info, "UpstreamHandler::onClose reset connection pool handler");
  }

  //if (conn_data_) {
  //  conn_data_.reset();
  //  ENVOY_LOG(info, "UpstreamHandler::onClose conn reset");
  //}
}

absl::optional<Upstream::TcpPoolData>
UpstreamHandler::createTcpPoolData(Upstream::ThreadLocalCluster& thread_local_cluster,
                                   Upstream::LoadBalancerContext& context) {
  return thread_local_cluster.tcpConnPool(Upstream::ResourcePriority::Default, &context);
}

int UpstreamHandler::start(Upstream::TcpPoolData& pool_data) {
  Tcp::ConnectionPool::Cancellable* handle = pool_data.newConnection(*this);
  if (handle) {
    ASSERT(upstream_handle_ == nullptr);
    upstream_handle_ = handle;
  }
  return 0;
}

void UpstreamHandler::onPoolFailure(ConnectionPool::PoolFailureReason reason,
                                    absl::string_view transport_failure_reason,
                                    Upstream::HostDescriptionConstSharedPtr host) {
  ENVOY_LOG(error, "UpstreamHandler onPoolFailure [{}]:{} {} {}", key_, host->address()->asString(),
            reason, transport_failure_reason);
  upstream_handle_ = nullptr;
  // 失败时等待下一次请求触发重试
  delete_callback_(key_);
}

void UpstreamHandler::onPoolReady(Tcp::ConnectionPool::ConnectionDataPtr&& conn_data,
                                  Upstream::HostDescriptionConstSharedPtr host) {
  ENVOY_LOG(debug, "UpstreamHandler onPoolReady [{}]:{}", key_, host->address()->asString());
  upstream_host_ = host;
  conn_data_ = std::move(conn_data);
  upstream_handle_ = nullptr;
  conn_data_->addUpstreamCallbacks(*this);

  // 如果有缓存消息，直接发送
  ENVOY_CONN_LOG(debug, "UpstreamHandler[{}]: request_caches_ size:{}", conn_data_->connection(),
                 key_, request_caches_.size());
  for (auto& request_caches : request_caches_) {
    conn_data_->connection().write(request_caches.first, request_caches.second);
  }
}

void UpstreamHandler::onData(Buffer::Instance& data, bool end_stream) {
  if (conn_data_) {
    ENVOY_CONN_LOG(debug, "UpstreamHandler[{}] data length:{}, end_stream:{}",
                   conn_data_->connection(), key_, data.length(), end_stream);
    conn_data_->connection().write(data, end_stream);
  } else if (upstream_handle_) {
    // 如果正在建立连接，先缓存下消息
    ENVOY_LOG(debug, "UpstreamHandler[{}] data length:{}, end_stream:{}, cache msg", key_,
              data.length(), end_stream);
    request_caches_.push_back(std::make_pair(Buffer::OwnedImpl(data), end_stream));
  } else {
    // 理论上不会到这里
    ENVOY_LOG(error, "UpstreamHandler[{}] sendMessage failed", key_);
  }
}

void UpstreamHandler::onUpstreamData(Buffer::Instance& data, bool end_stream) {
  ENVOY_LOG(debug, "UpstreamHandler[{}]: upstream callback length {} , end:{}", key_, data.length(),
            end_stream);
  // 先简单实现，直接原路返回
  downstream_connection_.write(data, end_stream);
}

void UpstreamHandler::onEvent(Envoy::Network::ConnectionEvent event) {
  ENVOY_LOG(debug, "UpstreamHandler[{}]: connection Event {}", key_, event);

  switch (event) {
  case Network::ConnectionEvent::RemoteClose:
    onClose();
    break;
  case Network::ConnectionEvent::LocalClose:
    onClose();
    break;
  default:
    // Connected is consumed by the connection pool.
    // NOT_REACHED_GCOVR_EXCL_LINE;
    break;
  }
}

void UpstreamHandlerManager::add(const std::string& key, std::shared_ptr<UpstreamHandler> handler) {
  if (handler) {
    ENVOY_LOG(debug, "Add upstream handler, key:{}", key);
    upstream_handlers_.emplace(key, handler);
  }
}

void UpstreamHandlerManager::del(const std::string& key) {
  auto it = upstream_handlers_.find(key);
  if (it != upstream_handlers_.end()) {
    ENVOY_LOG(debug, "Del upstream handler key:{}", key);
    upstream_handlers_.erase(it);
  } else {
    ENVOY_LOG(debug, "Del upstream handler key:{} not find", key);
  }
}

std::shared_ptr<UpstreamHandler> UpstreamHandlerManager::get(const std::string& key) {
  auto it = upstream_handlers_.find(key);
  if (it != upstream_handlers_.end()) {
    return it->second;
  }
  return nullptr;
}

void UpstreamHandlerManager::clear() {
  upstream_handlers_.clear();
}

} // namespace  MetaProtocolProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
