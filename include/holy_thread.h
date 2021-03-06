// Copyright (c) 2015-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#ifndef INCLUDE_HOLY_THREAD_H_
#define INCLUDE_HOLY_THREAD_H_

#include <map>
#include <set>
#include <string>

#include "include/xdebug.h"
#include "src/server_thread.h"
#include "src/pink_epoll.h"
#include "src/pink_item.h"
#include "include/pink_mutex.h"

namespace pink {

template <typename Conn>
class HolyThread: public ServerThread {
 public:
  // This type thread thread will listen and work self list redis thread
  explicit HolyThread(int port, int cron_interval);
  HolyThread(const std::string& bind_ip, int port, int cron_interval);
  HolyThread(const std::set<std::string>& bind_ips, int port, int cron_interval);
  virtual ~HolyThread();

  /*
   *  public for external statistics
   */
  pthread_rwlock_t rwlock_;
  std::map<int, void *> conns_;

 private:
  void CronHandle() override {}
  bool AccessHandle(std::string& ip) override {
    return true;
  }

  void HandleNewConn(const int connfd, const std::string &ip_port) override;
  void HandleConnEvent(PinkFiredEvent *pfe) override;

  void Cleanup();
};  // class HolyThread

template <typename Conn>
HolyThread<Conn>::HolyThread(int port, int cron_interval = 0) :
  ServerThread::ServerThread(port, cron_interval) {
  pthread_rwlock_init(&rwlock_, NULL);
}

template <typename Conn>
HolyThread<Conn>::HolyThread(const std::string& bind_ip, int port, int cron_interval = 0) :
  ServerThread::ServerThread(bind_ip, port, cron_interval) {
  pthread_rwlock_init(&rwlock_, NULL);
}

template <typename Conn>
HolyThread<Conn>::HolyThread(const std::set<std::string>& bind_ips, int port,
                             int cron_interval = 0) :
  ServerThread::ServerThread(bind_ips, port, cron_interval) {
  pthread_rwlock_init(&rwlock_, NULL);
}

template <typename Conn>
HolyThread<Conn>::~HolyThread() {
  Cleanup();
}

template <typename Conn>
void HolyThread<Conn>::HandleNewConn(const int connfd, const std::string &ip_port) {
  Conn *tc = new Conn(connfd, ip_port, this);
  tc->SetNonblock();
  {
    RWLock l(&rwlock_, true);
    conns_[connfd] = tc;
  }

  pink_epoll_->PinkAddEvent(connfd, EPOLLIN);
}

template <typename Conn>
void HolyThread<Conn>::HandleConnEvent(PinkFiredEvent *pfe) {
  if (pfe == NULL) {
    return;
  }
  Conn *in_conn = NULL;
  int should_close = 0;
  std::map<int, void *>::iterator iter;
  {
    RWLock l(&rwlock_, false);
    if ((iter = conns_.find(pfe->fd)) == conns_.end()) {
      pink_epoll_->PinkDelEvent(pfe->fd);
      return;
    }
  }

  if (pfe->mask & EPOLLIN) {
    in_conn = static_cast<Conn *>(iter->second);
    ReadStatus getRes = in_conn->GetRequest();
    struct timeval now;
    gettimeofday(&now, NULL);
    in_conn->set_last_interaction(now);
    if (getRes != kReadAll && getRes != kReadHalf) {
      // kReadError kReadClose kFullError kParseError
      should_close = 1;
    } else if (in_conn->is_reply()) {
      pink_epoll_->PinkModEvent(pfe->fd, 0, EPOLLOUT);
    } else {
      return;
    }
  }
  if (pfe->mask & EPOLLOUT) {
    in_conn = static_cast<Conn *>(iter->second);
    WriteStatus write_status = in_conn->SendReply();
    if (write_status == kWriteAll) {
      in_conn->set_is_reply(false);
      pink_epoll_->PinkModEvent(pfe->fd, 0, EPOLLIN);
    } else if (write_status == kWriteHalf) {
      return;
    } else if (write_status == kWriteError) {
      should_close = 1;
    }
  }
  if ((pfe->mask & EPOLLERR) || (pfe->mask & EPOLLHUP) || should_close) {
    log_info("close pfe fd here");
    pink_epoll_->PinkDelEvent(pfe->fd);
    close(pfe->fd);
    delete(in_conn);
    in_conn = NULL;

    RWLock l(&rwlock_, true);
    conns_.erase(pfe->fd);
  }
}

// clean conns
template <typename Conn>
void HolyThread<Conn>::Cleanup() {
  RWLock l(&rwlock_, true);
  Conn *in_conn;
  std::map<int, void *>::iterator iter = conns_.begin();
  for (; iter != conns_.end(); iter++) {
    close(iter->first);
    in_conn = static_cast<Conn *>(iter->second);
    delete in_conn;
  }
}

}  // namespace pink

#endif  // INCLUDE_HOLY_THREAD_H_
