#pragma once

#include <memory>
#include <string>

#include "envoy/buffer/buffer.h"
#include "envoy/tcp/conn_pool.h"
#include "envoy/upstream/thread_local_cluster.h"

#include "common/common/logger.h"
#include "common/buffer/buffer_impl.h"
#include "common/upstream/load_balancer_impl.h"

#include "src/meta_protocol_proxy/filters/filter.h"
#include "src/meta_protocol_proxy/route/route.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace MetaProtocolProxy {
namespace Router {

class Router : public Tcp::ConnectionPool::UpstreamCallbacks,
               public Upstream::LoadBalancerContextBase,
               public CodecFilter,
               Logger::Loggable<Logger::Id::filter> {
public:
  Router(Upstream::ClusterManager& cluster_manager) : cluster_manager_(cluster_manager) {}
  ~Router() override = default;

  // DecoderFilter
  void onDestroy() override;
  void setDecoderFilterCallbacks(DecoderFilterCallbacks& callbacks) override;

  FilterStatus onMessageDecoded(MetadataSharedPtr metadata, MutationSharedPtr mutation) override;

  // EncoderFilter
  void setEncoderFilterCallbacks(EncoderFilterCallbacks& callbacks) override;
  FilterStatus onMessageEncoded(MetadataSharedPtr metadata, MutationSharedPtr mutation) override;

  // Upstream::LoadBalancerContextBase
  const Envoy::Router::MetadataMatchCriteria* metadataMatchCriteria() override { return nullptr; }
  const Network::Connection* downstreamConnection() const override;

  // Tcp::ConnectionPool::UpstreamCallbacks
  void onUpstreamData(Buffer::Instance& data, bool end_stream) override;
  void onEvent(Network::ConnectionEvent event) override;
  void onAboveWriteBufferHighWatermark() override {}
  void onBelowWriteBufferLowWatermark() override {}

  // This function is for testing only.
  Envoy::Buffer::Instance& upstreamRequestBufferForTest() { return upstream_request_buffer_; }

private:
  struct UpstreamRequest : public Tcp::ConnectionPool::Callbacks {
    UpstreamRequest(Router& parent, Tcp::ConnectionPool::Instance& pool,
                    MetadataSharedPtr& metadata);
    ~UpstreamRequest() override;

    FilterStatus start();
    void resetStream();
    void encodeData(Buffer::Instance& data);

    // Tcp::ConnectionPool::Callbacks
    void onPoolFailure(ConnectionPool::PoolFailureReason reason,
                       Upstream::HostDescriptionConstSharedPtr host) override;
    void onPoolReady(Tcp::ConnectionPool::ConnectionDataPtr&& conn,
                     Upstream::HostDescriptionConstSharedPtr host) override;

    void onRequestStart(bool continue_decoding);
    void onRequestComplete();
    void onResponseComplete();
    void onUpstreamHostSelected(Upstream::HostDescriptionConstSharedPtr host);
    void onResetStream(ConnectionPool::PoolFailureReason reason);

    Router& parent_;
    Tcp::ConnectionPool::Instance& conn_pool_;
    MetadataSharedPtr metadata_;
    MutationSharedPtr mutation_;

    Tcp::ConnectionPool::Cancellable* conn_pool_handle_{};
    Tcp::ConnectionPool::ConnectionDataPtr conn_data_;
    Upstream::HostDescriptionConstSharedPtr upstream_host_;

    bool request_complete_ : 1;
    bool response_started_ : 1;
    bool response_complete_ : 1;
    bool stream_reset_ : 1;
  };

  void cleanup();

  Upstream::ClusterManager& cluster_manager_;

  DecoderFilterCallbacks* callbacks_{};
  EncoderFilterCallbacks* encoder_callbacks_{};
  Route::RouteConstSharedPtr route_{};
  const Route::RouteEntry* route_entry_{};
  Upstream::ClusterInfoConstSharedPtr cluster_;

  std::unique_ptr<UpstreamRequest> upstream_request_;
  Envoy::Buffer::OwnedImpl upstream_request_buffer_;

  bool filter_complete_{false};
};

class IdleTimeoutChecker : public Network::ConnectionCallbacks,
                           public Network::WriteFilter,
                           Logger::Loggable<Logger::Id::filter> {
public:
  IdleTimeoutChecker(Network::ClientConnection& connection, Event::Dispatcher& dispatcher);
  ~IdleTimeoutChecker() override{
    ENVOY_LOG(debug, "XXXXXXXXXX ~IdleTimeoutChecker() XXXXXXXXXXX");
  };

  // Network::ConnectionCallbacks
  void onEvent(Network::ConnectionEvent event) override;
  void onAboveWriteBufferHighWatermark() override{};
  void onBelowWriteBufferLowWatermark() override{};

  // Network::WriteFilter
  Network::FilterStatus onWrite(Buffer::Instance& data, bool end_stream) override;
  void initializeWriteFilterCallbacks(Network::WriteFilterCallbacks&) override {};

private:
  void onIdleTimeout();
  void disableIdleTimer();
  void enableIdleTimer();
  Network::ClientConnection& connection_;
  Event::TimerPtr idle_timer_;
};
} // namespace Router
} // namespace MetaProtocolProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy

