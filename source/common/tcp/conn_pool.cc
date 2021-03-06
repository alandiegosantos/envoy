#include "common/tcp/conn_pool.h"

#include <memory>

#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/upstream/upstream.h"

#include "common/stats/timespan_impl.h"
#include "common/upstream/upstream_impl.h"

namespace Envoy {
namespace Tcp {

ActiveTcpClient::ActiveTcpClient(Envoy::ConnectionPool::ConnPoolImplBase& parent,
                                 const Upstream::HostConstSharedPtr& host,
                                 uint64_t concurrent_stream_limit)
    : Envoy::ConnectionPool::ActiveClient(parent, host->cluster().maxRequestsPerConnection(),
                                          concurrent_stream_limit),
      parent_(parent) {
  Upstream::Host::CreateConnectionData data = host->createConnection(
      parent_.dispatcher(), parent_.socketOptions(), parent_.transportSocketOptions());
  real_host_description_ = data.host_description_;
  connection_ = std::move(data.connection_);
  connection_->addConnectionCallbacks(*this);
  connection_->detectEarlyCloseWhenReadDisabled(false);
  connection_->addReadFilter(std::make_shared<ConnReadFilter>(*this));
  connection_->setConnectionStats({host->cluster().stats().upstream_cx_rx_bytes_total_,
                                   host->cluster().stats().upstream_cx_rx_bytes_buffered_,
                                   host->cluster().stats().upstream_cx_tx_bytes_total_,
                                   host->cluster().stats().upstream_cx_tx_bytes_buffered_,
                                   &host->cluster().stats().bind_errors_, nullptr});

  connection_->connect();
}

ActiveTcpClient::~ActiveTcpClient() {
  // Handle the case where deferred delete results in the ActiveClient being destroyed before
  // TcpConnectionData. Make sure the TcpConnectionData will not refer to this ActiveTcpClient
  // and handle clean up normally done in clearCallbacks()
  if (tcp_connection_data_) {
    ASSERT(state_ == ActiveClient::State::CLOSED);
    tcp_connection_data_->release();
    parent_.onStreamClosed(*this, true);
    parent_.checkForDrained();
  }
}

void ActiveTcpClient::clearCallbacks() {
  if (state_ == Envoy::ConnectionPool::ActiveClient::State::BUSY && parent_.hasPendingStreams()) {
    auto* pool = &parent_;
    pool->dispatcher().post([pool]() -> void { pool->onUpstreamReady(); });
  }
  callbacks_ = nullptr;
  tcp_connection_data_ = nullptr;
  parent_.onStreamClosed(*this, true);
  parent_.checkForDrained();
}

void ActiveTcpClient::onEvent(Network::ConnectionEvent event) {
  Envoy::ConnectionPool::ActiveClient::onEvent(event);
  // Do not pass the Connected event to any session which registered during onEvent above.
  // Consumers of connection pool connections assume they are receiving already connected
  // connections.
  if (callbacks_ && event != Network::ConnectionEvent::Connected) {
    callbacks_->onEvent(event);
    // After receiving a disconnect event, the owner of callbacks_ will likely self-destruct.
    // Clear the pointer to avoid using it again.
    callbacks_ = nullptr;
  }
}

} // namespace Tcp
} // namespace Envoy
