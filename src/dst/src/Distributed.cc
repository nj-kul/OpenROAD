// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2021-2025, The OpenROAD Authors

#include "dst/Distributed.h"

#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/system_error.hpp>
#include <boost/thread/thread.hpp>
#include <memory>
#include <string>
#include <utility>

#include "LoadBalancer.h"
#include "Worker.h"
#include "dst/JobCallBack.h"
#include "dst/JobMessage.h"
#include "utl/Logger.h"
namespace dst {
const int MAX_TRIES = 5;
}

using namespace dst;

Distributed::Distributed(utl::Logger* logger) : logger_(logger)
{
}

Distributed::~Distributed()
{
  for (auto cb : callbacks_) {
    delete cb;
  }
  callbacks_.clear();
}

void Distributed::init(utl::Logger* logger)
{
  logger_ = logger;
}

void Distributed::runWorker(const char* ip,
                            unsigned short port,
                            bool interactive)
{
  try {
    auto uWorker = std::make_unique<Worker>(this, logger_, ip, port);
    auto worker = uWorker.get();
    workers_.push_back(std::move(uWorker));
    if (interactive) {
      boost::thread t(boost::bind(&Worker::run, worker));
      t.detach();
    } else {
      worker->run();
    }
  } catch (std::exception& e) {
    logger_->error(utl::DST, 1, "Worker server error: {}", e.what());
  }
}

void Distributed::runLoadBalancer(const char* ip,
                                  unsigned short port,
                                  const char* workers_domain)
{
  try {
    asio::io_context service;
    LoadBalancer balancer(this, service, logger_, ip, workers_domain, port);
    if (std::strcmp(workers_domain, "") == 0) {
      for (const auto& worker : end_points_) {
        balancer.addWorker(worker.ip, worker.port);
      }
    }
    service.run();
  } catch (std::exception& e) {
    logger_->error(utl::DST, 9, "LoadBalancer error: {}", e.what());
  }
}

void Distributed::addWorkerAddress(const char* address, unsigned short port)
{
  end_points_.emplace_back(address, port);
}
// TODO: exponential backoff
bool sendMsg(dst::socket& sock, const std::string& msg, std::string& errorMsg)
{
  int tries = 0;
  while (tries++ < MAX_TRIES) {
    boost::system::error_code error;
    sock.wait(asio::ip::tcp::socket::wait_write);
    write(sock, asio::buffer(msg), error);
    if (!error) {
      errorMsg.clear();
      return true;
    }
    errorMsg = error.message();
  }
  return false;
}

bool readMsg(dst::socket& sock, std::string& dataStr)
{
  boost::system::error_code error;
  asio::streambuf receive_buffer;
  sock.wait(asio::ip::tcp::socket::wait_read);
  asio::read(sock, receive_buffer, asio::transfer_all(), error);
  if (error && error != asio::error::eof) {
    dataStr = error.message();
    return false;
  }

  auto bufs = receive_buffer.data();
  auto offset = asio::buffers_begin(bufs) + receive_buffer.size();
  std::string result;
  if (offset <= asio::buffers_end(bufs)) {
    result = std::string(asio::buffers_begin(bufs), offset);
  }

  dataStr = result;
  return !dataStr.empty();
}

bool Distributed::sendJob(JobMessage& msg,
                          const char* ip,
                          unsigned short port,
                          JobMessage& result)
{
  int tries = 0;
  std::string msgStr;
  if (!JobMessage::serializeMsg(JobMessage::WRITE, msg, msgStr)) {
    logger_->warn(utl::DST, 112, "Serializing JobMessage failed");
    return false;
  }
  std::string resultStr;
  while (tries++ < MAX_TRIES) {
    asio::io_context service;
    dst::socket sock(service);
    try {
      sock.connect(tcp::endpoint(ip::make_address(ip), port));
    } catch (const boost::system::system_error& ex) {
      logger_->warn(utl::DST,
                    113,
                    "Trial {}, socket connection failed with message \"{}\"",
                    tries,
                    ex.what());
      continue;
    }
    bool ok = sendMsg(sock, msgStr, resultStr);
    if (!ok) {
      continue;
    }
    ok = readMsg(sock, resultStr);
    if (!ok) {
      continue;
    }
    if (!JobMessage::serializeMsg(JobMessage::READ, result, resultStr)) {
      continue;
    }
    if (sock.is_open()) {
      sock.close();
    }
    return true;
  }
  if (resultStr.empty()) {
    resultStr = "MAX_TRIES reached";
  }
  logger_->warn(
      utl::DST, 114, "Sending job failed with message \"{}\"", resultStr);
  return false;
}

inline bool getNextMsg(std::string& haystack,
                       const std::string& needle,
                       std::string& result)
{
  std::size_t found = haystack.find(needle);
  if (found != std::string::npos) {
    result = haystack.substr(0, found + needle.size());
    haystack.erase(0, found + needle.size());
    return true;
  }
  return false;
}
bool Distributed::sendJobMultiResult(JobMessage& msg,
                                     const char* ip,
                                     unsigned short port,
                                     JobMessage& result)
{
  int tries = 0;
  std::string msgStr;
  if (!JobMessage::serializeMsg(JobMessage::WRITE, msg, msgStr)) {
    logger_->warn(utl::DST, 12, "Serializing JobMessage failed");
    return false;
  }
  std::string resultStr;
  while (tries++ < MAX_TRIES) {
    asio::io_context service;
    dst::socket sock(service);
    try {
      sock.connect(tcp::endpoint(ip::make_address(ip), port));
    } catch (const boost::system::system_error& ex) {
      logger_->warn(utl::DST,
                    13,
                    "Socket connection failed with message \"{}\"",
                    ex.what());
      return false;
    }
    boost::asio::ip::tcp::no_delay option(true);
    sock.set_option(option);
    bool ok = sendMsg(sock, msgStr, resultStr);
    if (!ok) {
      continue;
    }
    ok = readMsg(sock, resultStr);
    if (!ok) {
      continue;
    }
    std::string split;
    while (getNextMsg(resultStr, JobMessage::EOP, split)) {
      JobMessage tmp;
      if (!JobMessage::serializeMsg(JobMessage::READ, tmp, split)) {
        logger_->error(utl::DST, 9999, "Problem in deserialize {}", split);
      } else {
        result.addJobDescription(std::move(tmp.getJobDescriptionRef()));
      }
    }
    result.setJobType(JobMessage::SUCCESS);
    if (sock.is_open()) {
      sock.close();
    }
    return true;
  }
  if (resultStr.empty()) {
    resultStr = "MAX_TRIES reached";
  }
  logger_->warn(
      utl::DST, 14, "Sending job failed with message \"{}\"", resultStr);
  return false;
}

bool Distributed::sendResult(JobMessage& msg, dst::socket& sock)
{
  std::string msgStr;
  if (!JobMessage::serializeMsg(JobMessage::WRITE, msg, msgStr)) {
    logger_->warn(utl::DST, 20, "Serializing result JobMessage failed");
    return false;
  }
  int tries = 0;
  std::string error;
  while (tries++ < MAX_TRIES) {
    if (sendMsg(sock, msgStr, error)) {
      return true;
    }
  }
  logger_->warn(
      utl::DST, 22, "Sending result failed with message \"{}\"", error);
  return false;
}

void Distributed::addCallBack(JobCallBack* cb)
{
  callbacks_.push_back(cb);
}
