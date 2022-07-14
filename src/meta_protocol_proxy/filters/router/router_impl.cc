#include "src/meta_protocol_proxy/filters/router/router_impl.h"

#include "envoy/upstream/cluster_manager.h"
#include "envoy/upstream/thread_local_cluster.h"

#include "src/meta_protocol_proxy/app_exception.h"
#include "src/meta_protocol_proxy/codec/codec.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace MetaProtocolProxy {
namespace Router {

// ---- DecoderFilter ---- handle request path
void Router::onDestroy() {
  // close the upstream connection if the upstream request has not been completed because there may
  // be more data coming later on the connection after destroying the router
  if (!upstreamRequestFinished()) {
    upstream_request_->releaseUpStreamConnection(true);
  }
  cleanUpstreamRequest();

  for (auto& shadow_router : shadow_routers_) {
    shadow_router.get().onRouterDestroy();
  }

  shadow_routers_.clear();
}

void Router::setDecoderFilterCallbacks(DecoderFilterCallbacks& callbacks) {
  decoder_filter_callbacks_ = &callbacks;
}

FilterStatus Router::onMessageDecoded(MetadataSharedPtr metadata,
                                      MutationSharedPtr requestMutation) {
  auto messageType = metadata->getMessageType();
  ASSERT(messageType == MessageType::Request || messageType == MessageType::Stream_Init);

  requestMetadata_ = metadata;
  route_ = decoder_filter_callbacks_->route();
  if (!route_) {
    ENVOY_STREAM_LOG(debug, "meta protocol router: no cluster match for request '{}'",
                     *decoder_filter_callbacks_, metadata->getRequestId());
    decoder_filter_callbacks_->sendLocalReply(
        AppException(Error{ErrorType::RouteNotFound,
                           fmt::format("meta protocol router: no cluster match for request '{}'",
                                       metadata->getRequestId())}),
        false);
    return FilterStatus::AbortIteration;
  }

  route_entry_ = route_->routeEntry();
  const std::string& cluster_name = route_entry_->clusterName();

  auto prepare_result = prepareUpstreamRequest(cluster_name, metadata, this);
  if (prepare_result.exception.has_value()) {
    decoder_filter_callbacks_->sendLocalReply(prepare_result.exception.value(), false);
    return FilterStatus::AbortIteration;
  }
  auto& upstream_req_info = prepare_result.upstream_request_info.value();

  ENVOY_STREAM_LOG(debug, "meta protocol router: decoding request", *decoder_filter_callbacks_);

  // Save the clone for request mirroring
  auto metadata_clone = requestMetadata_->clone();

  route_entry_->requestMutation(requestMutation);
  upstream_request_ = std::make_unique<UpstreamRequest>(*this, *upstream_req_info.conn_pool_data,
                                                        requestMetadata_, requestMutation);
  auto filter_status = upstream_request_->start();

  // Prepare connections for shadow routers, if there are mirror policies configured and currently
  // enabled.
  const auto& policies = route_entry_->requestMirrorPolicies();
  ENVOY_LOG(debug, "meta protocol router: requestMirrorPolicies size:{}", policies.size());

  if (!policies.empty()) {
    for (const auto& policy : policies) {
      if (policy->enabled(runtime_)) {
        // Reset original message since
        auto shadow_router =
            // We can reuse the same metadata for each request because its original message will be
            // drained in the request
            shadow_writer_.submit(policy->clusterName(), metadata_clone->clone(), requestMutation,
                                  decoder_filter_callbacks_->codec());
        if (shadow_router.has_value()) {
          shadow_routers_.push_back(shadow_router.value());
        }
      }
    }
  }

  return filter_status;
}
// ---- DecoderFilter ----

// ---- EncoderFilter ---- handle response path
void Router::setEncoderFilterCallbacks(EncoderFilterCallbacks& callbacks) {
  encoder_filter_callbacks_ = &callbacks;
}

FilterStatus Router::onMessageEncoded(MetadataSharedPtr metadata, MutationSharedPtr) {
  if (upstream_request_ == nullptr) {
    return FilterStatus::ContinueIteration;
  }

  ENVOY_STREAM_LOG(trace, "meta protocol router: response status: {}", *encoder_filter_callbacks_,
                   metadata->getResponseStatus());

  switch (metadata->getResponseStatus()) {
  case ResponseStatus::Ok:
    if (metadata->getMessageType() == MessageType::Error) {
      upstream_request_->upstreamHost()->outlierDetector().putResult(
          Upstream::Outlier::Result::ExtOriginRequestFailed);
    } else {
      upstream_request_->upstreamHost()->outlierDetector().putResult(
          Upstream::Outlier::Result::ExtOriginRequestSuccess);
    }
    break;
  case ResponseStatus::Error:
    upstream_request_->upstreamHost()->outlierDetector().putResult(
        Upstream::Outlier::Result::ExtOriginRequestFailed);
    break;
  default:
    break;
  }

  return FilterStatus::ContinueIteration;
}
// ---- EncoderFilter ---

// ---- Tcp::ConnectionPool::UpstreamCallbacks ----
void Router::onUpstreamData(Buffer::Instance& data, bool end_stream) {
  // We shouldn't get more data after a response is completed, otherwise it's a codec issue
  ASSERT(!upstream_request_->responseCompleted());
  ENVOY_STREAM_LOG(debug, "meta protocol router: reading response: {} bytes",
                   *decoder_filter_callbacks_, data.length());

  // Start response when receiving the first packet
  if (!upstream_request_->responseStarted()) {
    decoder_filter_callbacks_->startUpstreamResponse(*requestMetadata_);
    upstream_request_->onResponseStarted();
  }

  UpstreamResponseStatus status = decoder_filter_callbacks_->upstreamData(data);
  switch (status) {
  case UpstreamResponseStatus::Complete:
    ENVOY_STREAM_LOG(debug, "meta protocol router: response complete", *decoder_filter_callbacks_);
    upstream_request_->onResponseComplete();
    cleanUpstreamRequest();
    return;
  case UpstreamResponseStatus::Reset:
    ENVOY_STREAM_LOG(debug, "meta protocol router: upstream reset", *decoder_filter_callbacks_);
    // When the upstreamData function returns Reset,
    // the current stream is already released from the upper layer,
    // so there is no need to call callbacks_->resetStream() to notify
    // the upper layer to release the stream.
    upstream_request_->releaseUpStreamConnection(true);
    return;
  case UpstreamResponseStatus::MoreData:
    // Response is incomplete, but no more data is coming. Probably codec or application side error.
    if (end_stream) {
      ENVOY_STREAM_LOG(debug,
                       "meta protocol router: response is incomplete, but no more data is coming",
                       *decoder_filter_callbacks_);
      upstream_request_->onUpstreamConnectionReset(
          ConnectionPool::PoolFailureReason::RemoteConnectionFailure);
      upstream_request_->onResponseComplete();
      cleanUpstreamRequest();
      return;
      // todo we also need to clean the stream
    default:
      NOT_REACHED_GCOVR_EXCL_LINE;
    }
  }
}

void Router::onEvent(Network::ConnectionEvent event) {
  ASSERT(upstream_request_);

  //  if (upstream_request_->stream_reset_ && event == Network::ConnectionEvent::LocalClose) {
  //    ENVOY_LOG(debug, "meta protocol upstream request: the stream reset");
  //    return;
  //  }
  upstream_request_->onUpstreamConnectionEvent(event);
}
// ---- Tcp::ConnectionPool::UpstreamCallbacks ----

// ---- Upstream::LoadBalancerContextBase ----
absl::optional<uint64_t> Router::computeHashKey() {
  if (auto* hash_policy = route_entry_->hashPolicy(); hash_policy != nullptr) {
    auto hash = hash_policy->generateHash(*requestMetadata_);
    if (hash.has_value()) {
      ENVOY_LOG(debug, "meta protocol router: computeHashKey: {}", hash.value());
    }
    return hash;
  }

  return {};
}

const Network::Connection* Router::downstreamConnection() const {
  return decoder_filter_callbacks_ != nullptr ? decoder_filter_callbacks_->connection() : nullptr;
}
// ---- Upstream::LoadBalancerContextBase ----

void Router::cleanUpstreamRequest() {
  ENVOY_LOG(debug, "meta protocol router: clean upstream request");
  if (upstream_request_) {
    upstream_request_.reset();
  }
};

} // namespace Router
} // namespace  MetaProtocolProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
