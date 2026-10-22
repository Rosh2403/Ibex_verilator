#ifndef IBEX_SOC_COSIM_H
#define IBEX_SOC_COSIM_H

#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "cosim_soc_protocol.h"
#include "sim_ctrl_extension.h"
#include "verilator_sim_ctrl.h"

extern "C" void uartdpi_set_cosim_callbacks(void (*write_byte)(unsigned char),
                                             int (*read_byte)(void));

class SocCosim : public SimCtrlExtension {
 public:
  ~SocCosim() override {
    uartdpi_set_cosim_callbacks(nullptr, nullptr);
    if (client_ >= 0) close(client_);
    if (server_ >= 0) close(server_);
    if (bound_) unlink(path_.c_str());
    active_ = nullptr;
  }

  bool ParseCLIArguments(int argc, char **argv, bool &exit_app) override {
    const std::string prefix = "--qemu-cosim-socket=";
    for (int index = 1; index < argc; ++index) {
      std::string argument = argv[index];
      if (argument == "--help") {
        std::cout << "--qemu-cosim-socket=PATH  Wait for full-SoC stepping and UART commands"
                  << std::endl;
      }
      if (argument.compare(0, prefix.size(), prefix) == 0) {
        path_ = argument.substr(prefix.size());
        if (path_.empty()) {
          exit_app = true;
          return false;
        }
      }
    }
    return true;
  }

  void PreExec() override {
    if (path_.empty()) return;
    active_ = this;
    interrupted_ = 0;
    struct sigaction action {};
    action.sa_handler = Signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, nullptr);

    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (path_.size() >= sizeof(address.sun_path)) {
      throw std::runtime_error("cosim socket path is too long");
    }
    std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
    server_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_ < 0 || bind(server_, reinterpret_cast<sockaddr *>(&address),
                            sizeof(address)) < 0) {
      throw std::runtime_error("cosim bind: " + std::string(strerror(errno)));
    }
    bound_ = true;
    if (listen(server_, 1) < 0) {
      throw std::runtime_error("cosim listen failed");
    }
    std::cout << "Full-SoC cosim listening on " << path_ << std::endl;
    if (!Wait(server_, POLLIN)) {
      throw std::runtime_error("cosim stopped while waiting for QEMU");
    }
    client_ = accept(server_, nullptr, nullptr);
    if (client_ < 0) throw std::runtime_error("cosim accept failed");
    uartdpi_set_cosim_callbacks(WriteUart, ReadUart);
  }

  void OnClock(unsigned long sim_time) override {
    if (path_.empty() || stopped_) return;
    if (remaining_ > 0) {
      --remaining_;
      return;
    }
    if (pending_step_ && !Reply(0)) {
      StopTransport();
      return;
    }
    pending_step_ = false;
    for (;;) {
      CosimSocReq request {};
      if (!Transfer(&request, sizeof(request), false)) {
        StopTransport();
        return;
      }
      uint32_t command = cosim_soc_le32(request.cmd);
      uint64_t parameter = cosim_soc_le64(request.param);
      if (cosim_soc_le32(request.magic) != COSIM_SOC_REQ_MAGIC) {
        Reply(COSIM_SOC_FLAG_ERROR | COSIM_SOC_FLAG_STOPPED);
        Stop(false);
        return;
      }
      if (command == COSIM_SOC_CMD_STEP && parameter > 0 &&
          parameter <= COSIM_SOC_MAX_STEP) {
        remaining_ = parameter - 1;
        pending_step_ = true;
        return;
      }
      if (command == COSIM_SOC_CMD_SHUTDOWN && parameter == 0) {
        Reply(COSIM_SOC_FLAG_STOPPED);
        Stop(true);
        return;
      }
      if (command == COSIM_SOC_CMD_UART_RX && parameter <= 255 &&
          rx_.size() < 4096) {
        rx_.push_back(static_cast<unsigned char>(parameter));
      } else if (command != COSIM_SOC_CMD_PING ||
                 parameter != COSIM_SOC_VERSION) {
        Reply(COSIM_SOC_FLAG_ERROR | COSIM_SOC_FLAG_STOPPED);
        Stop(false);
        return;
      }
      if (!Reply(0)) {
        StopTransport();
        return;
      }
    }
  }

  void PostExec() override {
    if (!path_.empty() && pending_step_ && !stopped_) {
      Reply(COSIM_SOC_FLAG_STOPPED);
      pending_step_ = false;
    }
  }

 private:
  static SocCosim *active_;
  static volatile sig_atomic_t interrupted_;
  std::string path_;
  int server_ = -1;
  int client_ = -1;
  bool bound_ = false;
  bool stopped_ = false;
  bool pending_step_ = false;
  bool disconnected_ = false;
  uint64_t remaining_ = 0;
  std::deque<unsigned char> tx_;
  std::deque<unsigned char> rx_;

  static void Signal(int signal) { interrupted_ = signal; }

  static void WriteUart(unsigned char byte) {
    if (active_->tx_.size() >= 4096) {
      active_->Stop(false);
      return;
    }
    active_->tx_.push_back(byte);
  }

  static int ReadUart() {
    if (active_->rx_.empty()) return -1;
    int byte = active_->rx_.front();
    active_->rx_.pop_front();
    return byte;
  }

  void Stop(bool success) {
    stopped_ = true;
    VerilatorSimCtrl::GetInstance().RequestStop(success);
  }

  void StopTransport() { Stop(interrupted_ != 0 || disconnected_); }

  bool Wait(int descriptor, short events) {
    while (!interrupted_) {
      struct pollfd item {descriptor, events, 0};
      int result = poll(&item, 1, 100);
      if (result > 0) {
        if (item.revents & events) return true;
        if (descriptor == client_ && (item.revents & (POLLHUP | POLLERR))) {
          disconnected_ = true;
        }
        return false;
      }
      if (result < 0 && errno != EINTR) return false;
    }
    return false;
  }

  bool Transfer(void *buffer, size_t length, bool writing) {
    auto *bytes = static_cast<unsigned char *>(buffer);
    while (length) {
      if (!Wait(client_, writing ? POLLOUT : POLLIN)) return false;
      ssize_t count = writing ? send(client_, bytes, length, 0)
                              : recv(client_, bytes, length, 0);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) {
        disconnected_ = count == 0 || errno == EPIPE || errno == ECONNRESET;
        return false;
      }
      bytes += count;
      length -= count;
    }
    return true;
  }

  bool Reply(uint8_t flags) {
    size_t length = std::min(tx_.size(), size_t(COSIM_SOC_MAX_UART_CHUNK));
    CosimSocResp response {};
    response.magic = cosim_soc_le32(COSIM_SOC_RESP_MAGIC);
    response.uart_len = cosim_soc_le32(static_cast<uint32_t>(length));
    response.flags = flags;
    if (!Transfer(&response, sizeof(response), true)) return false;
    std::vector<unsigned char> bytes;
    for (size_t index = 0; index < length; ++index) {
      bytes.push_back(tx_.front());
      tx_.pop_front();
    }
    return bytes.empty() || Transfer(bytes.data(), bytes.size(), true);
  }
};

SocCosim *SocCosim::active_ = nullptr;
volatile sig_atomic_t SocCosim::interrupted_ = 0;

#endif
