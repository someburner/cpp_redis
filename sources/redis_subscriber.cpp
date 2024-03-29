// The MIT License (MIT)
//
// Copyright (c) 2015-2017 Simon Ninon <simon.ninon@gmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <cpp_redis/logger.hpp>
#include <cpp_redis/redis_error.hpp>
#include <cpp_redis/redis_subscriber.hpp>

namespace cpp_redis {

#ifndef __CPP_REDIS_USE_CUSTOM_TCP_CLIENT
redis_subscriber::redis_subscriber(void)
: m_auth_reply_callback(nullptr) {
  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber created");
}
#endif /* __CPP_REDIS_USE_CUSTOM_TCP_CLIENT */

redis_subscriber::redis_subscriber(const std::shared_ptr<network::tcp_client_iface>& tcp_client)
: m_client(tcp_client)
, m_auth_reply_callback(nullptr) {
  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber created");
}

redis_subscriber::~redis_subscriber(void) {
  m_client.disconnect(true);
  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber destroyed");
}

void
redis_subscriber::connect(const std::string& host, std::size_t port, const disconnection_handler_t& client_disconnection_handler) {
  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber attempts to connect");

  auto disconnection_handler = std::bind(&redis_subscriber::connection_disconnection_handler, this, std::placeholders::_1);
  auto receive_handler       = std::bind(&redis_subscriber::connection_receive_handler, this, std::placeholders::_1, std::placeholders::_2);
  m_client.connect(host, port, disconnection_handler, receive_handler);

  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber connected");

  m_disconnection_handler = client_disconnection_handler;
}

redis_subscriber&
redis_subscriber::auth(const std::string& password, const reply_callback_t& reply_callback) {
  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber attempts to authenticate");

  m_client.send({"AUTH", password});
  m_auth_reply_callback = reply_callback;

  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber AUTH command sent");

  return *this;
}

void
redis_subscriber::disconnect(bool wait_for_removal) {
  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber attempts to disconnect");
  m_client.disconnect(wait_for_removal);
  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber disconnected");
}

bool
redis_subscriber::is_connected(void) {
  return m_client.is_connected();
}

redis_subscriber&
redis_subscriber::subscribe(const std::string& channel, const subscribe_callback_t& callback, const acknowledgement_callback_t& acknowledgement_callback) {
  std::lock_guard<std::mutex> lock(m_subscribed_channels_mutex);

  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber attemps to subscribe to channel " + channel);
  m_subscribed_channels[channel] = {callback, acknowledgement_callback};
  m_client.send({"SUBSCRIBE", channel});
  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber subscribed to channel " + channel);

  return *this;
}

redis_subscriber&
redis_subscriber::psubscribe(const std::string& pattern, const subscribe_callback_t& callback, const acknowledgement_callback_t& acknowledgement_callback) {
  std::lock_guard<std::mutex> lock(m_psubscribed_channels_mutex);

  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber attemps to psubscribe to channel " + pattern);
  m_psubscribed_channels[pattern] = {callback, acknowledgement_callback};
  m_client.send({"PSUBSCRIBE", pattern});
  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber psubscribed to channel " + pattern);

  return *this;
}

redis_subscriber&
redis_subscriber::unsubscribe(const std::string& channel) {
  std::lock_guard<std::mutex> lock(m_subscribed_channels_mutex);

  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber attemps to unsubscribe from channel " + channel);
  auto it = m_subscribed_channels.find(channel);
  if (it == m_subscribed_channels.end()) {
    __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber was not subscribed to channel " + channel);
    return *this;
  }

  m_client.send({"UNSUBSCRIBE", channel});
  m_subscribed_channels.erase(it);
  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber unsubscribed from channel " + channel);

  return *this;
}

redis_subscriber&
redis_subscriber::punsubscribe(const std::string& pattern) {
  std::lock_guard<std::mutex> lock(m_psubscribed_channels_mutex);

  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber attemps to punsubscribe from channel " + pattern);
  auto it = m_psubscribed_channels.find(pattern);
  if (it == m_psubscribed_channels.end()) {
    __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber was not psubscribed to channel " + pattern);
    return *this;
  }

  m_client.send({"PUNSUBSCRIBE", pattern});
  m_psubscribed_channels.erase(it);
  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber punsubscribed from channel " + pattern);

  return *this;
}

redis_subscriber&
redis_subscriber::commit(void) {
  try {
    __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber attempts to send pipelined commands");
    m_client.commit();
    __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber sent pipelined commands");
  }
  catch (const cpp_redis::redis_error& e) {
    __CPP_REDIS_LOG(error, "cpp_redis::redis_subscriber could not send pipelined commands");
    throw e;
  }

  return *this;
}

void
redis_subscriber::call_acknowledgement_callback(const std::string& channel, const std::map<std::string, callback_holder>& channels, std::mutex& channels_mtx, int64_t nb_chans) {
  std::lock_guard<std::mutex> lock(channels_mtx);

  auto it = channels.find(channel);
  if (it == channels.end())
    return;

  if (it->second.acknowledgement_callback) {
    __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber executes acknowledgement callback for channel " + channel);
    it->second.acknowledgement_callback(nb_chans);
  }
}

void
redis_subscriber::handle_acknowledgement_reply(const std::vector<reply>& reply) {
  if (reply.size() != 3)
    return;

  const auto& title    = reply[0];
  const auto& channel  = reply[1];
  const auto& nb_chans = reply[2];

  if (!title.is_string()
      || !channel.is_string()
      || !nb_chans.is_integer())
    return;

  if (title.as_string() == "subscribe")
    call_acknowledgement_callback(channel.as_string(), m_subscribed_channels, m_subscribed_channels_mutex, nb_chans.as_integer());
  else if (title.as_string() == "psubscribe")
    call_acknowledgement_callback(channel.as_string(), m_psubscribed_channels, m_psubscribed_channels_mutex, nb_chans.as_integer());
}

void
redis_subscriber::handle_subscribe_reply(const std::vector<reply>& reply) {
  if (reply.size() != 3)
    return;

  const auto& title   = reply[0];
  const auto& channel = reply[1];
  const auto& message = reply[2];

  if (!title.is_string()
      || !channel.is_string()
      || !message.is_string())
    return;

  if (title.as_string() != "message")
    return;

  std::lock_guard<std::mutex> lock(m_subscribed_channels_mutex);

  auto it = m_subscribed_channels.find(channel.as_string());
  if (it == m_subscribed_channels.end())
    return;

  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber executes subscribe callback for channel " + channel.as_string());
  it->second.subscribe_callback(channel.as_string(), message.as_string());
}

void
redis_subscriber::handle_psubscribe_reply(const std::vector<reply>& reply) {
  if (reply.size() != 4)
    return;

  const auto& title    = reply[0];
  const auto& pchannel = reply[1];
  const auto& channel  = reply[2];
  const auto& message  = reply[3];

  if (!title.is_string()
      || !pchannel.is_string()
      || !channel.is_string()
      || !message.is_string())
    return;

  if (title.as_string() != "pmessage")
    return;

  std::lock_guard<std::mutex> lock(m_psubscribed_channels_mutex);

  auto it = m_psubscribed_channels.find(pchannel.as_string());
  if (it == m_psubscribed_channels.end())
    return;

  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber executes psubscribe callback for channel " + channel.as_string());
  it->second.subscribe_callback(channel.as_string(), message.as_string());
}

void
redis_subscriber::connection_receive_handler(network::redis_connection&, reply& reply) {
  __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber received reply");

  //! always return an array
  //! otherwise, if auth was defined, this should be the AUTH reply
  //! any other replies from the server are considered as unexepected
  if (!reply.is_array()) {
    if (m_auth_reply_callback) {
      __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber executes auth callback");

      m_auth_reply_callback(reply);
      m_auth_reply_callback = nullptr;
    }

    return;
  }

  auto& array = reply.as_array();

  //! Array size of 3 -> SUBSCRIBE if array[2] is a string
  //! Array size of 3 -> AKNOWLEDGEMENT if array[2] is an integer
  //! Array size of 4 -> PSUBSCRIBE
  //! Otherwise -> unexepcted reply
  if (array.size() == 3 && array[2].is_integer())
    handle_acknowledgement_reply(array);
  else if (array.size() == 3 && array[2].is_string())
    handle_subscribe_reply(array);
  else if (array.size() == 4)
    handle_psubscribe_reply(array);
}

void
redis_subscriber::connection_disconnection_handler(network::redis_connection&) {
  __CPP_REDIS_LOG(warn, "cpp_redis::redis_subscriber has been disconnected");

  if (m_disconnection_handler) {
    __CPP_REDIS_LOG(vdebug, "cpp_redis::redis_subscriber calls disconnection handler");
    m_disconnection_handler(*this);
  }
}

} //! cpp_redis
