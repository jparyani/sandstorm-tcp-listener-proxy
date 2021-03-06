// Copyright (c) 2016 Sandstorm Development Group, Inc.
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

// Hack around stdlib bug with C++14.
#include <initializer_list>  // force libstdc++ to include its config
#undef _GLIBCXX_HAVE_GETS    // correct broken config
// End hack.

#include <kj/main.h>
#include <kj/debug.h>
#include <kj/io.h>
#include <kj/async-io.h>
#include <capnp/rpc-twoparty.h>
#include <capnp/ez-rpc.h>
#include <capnp/serialize.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#include <sandstorm/grain.capnp.h>
#include <sandstorm/web-session.capnp.h>
#include <sandstorm/hack-session.capnp.h>
#include <sandstorm/sandstorm-http-bridge.capnp.h>

#include "sandstorm-tcp-listener-proxy.h"

namespace sandstorm {
  kj::Maybe<kj::AutoCloseFd> raiiOpenIfExists(kj::StringPtr name, int flags, mode_t  mode = 0666) {
    int fd = open(name.cStr(), flags, mode);
    if (fd == -1) {
      if (errno == ENOENT) {
        return nullptr;
      } else {
        KJ_FAIL_SYSCALL("open", errno, name);
      }
    } else {
      return kj::AutoCloseFd(fd);
    }
  }

  class TcpProxyListenerMain {
  public:
    TcpProxyListenerMain(kj::ProcessContext& context): context(context), handle(nullptr) { }

    kj::MainFunc getMain() {
       return kj::MainBuilder(context, "TcpProxyListener version: 0.0.1",
                             "Runs a TCP listener proxy for bridging IpInterface to convential "
                             "TCP listening applications.")
        .expectArg("<token>", KJ_BIND_METHOD(*this, setToken))
        .expectArg("<localPort>", KJ_BIND_METHOD(*this, setLocalPort))
        .expectArg("<externalPort>", KJ_BIND_METHOD(*this, setExternalPort))
        .expectOptionalArg("<claimSessionId>", KJ_BIND_METHOD(*this, setSessionId))
        .callAfterParsing(KJ_BIND_METHOD(*this, run))
        .build();
    }

    kj::MainBuilder::Validity setToken(kj::StringPtr _token) {
      token = kj::heapString(_token);
      return true;
    }

    kj::MainBuilder::Validity setLocalPort(kj::StringPtr _localPort) {
      localPort = kj::heapString(_localPort);
      return true;
    }

    kj::MainBuilder::Validity setExternalPort(kj::StringPtr _externalPort) {
      externalPort = kj::heapString(_externalPort);
      return true;
    }

    kj::MainBuilder::Validity setSessionId(kj::StringPtr _sessionId) {
      sessionId = kj::heapString(_sessionId);
      return true;
    }

    kj::MainBuilder::Validity run() {
      capnp::EzRpcClient client("unix:/tmp/sandstorm-api");
      SandstormHttpBridge::Client restorer = client.getMain<SandstormHttpBridge>();

      auto tokenBytes = token.asBytes();
      auto request = restorer.getSandstormApiRequest();
      auto api = request.send().getApi();
      kj::Array<unsigned char> byteArray;
      if (sessionId.size() != 0) {
        // This has a sessionId, so treat it as a claimrequest token
        auto contextRequest = restorer.getSessionContextRequest();
        contextRequest.setId(sessionId);
        auto context = contextRequest.send().wait(client.getWaitScope()).getContext();
        auto claimRequest = context.claimRequestRequest();
        claimRequest.setRequestToken(token);
        auto cap = claimRequest.send().wait(client.getWaitScope()).getCap();
        auto saveRequest = api.saveRequest();
        saveRequest.setCap(cap);
        auto saveResults = saveRequest.send().wait(client.getWaitScope());
        byteArray = kj::heapArray(saveResults.getToken());
        tokenBytes = byteArray;
        KJ_IF_MAYBE(fd, raiiOpenIfExists("/var/token", O_WRONLY | O_CLOEXEC)) {
          kj::FdOutputStream(kj::mv(*fd)).write(tokenBytes.begin(), tokenBytes.size());
        }
      }
      auto proxy = setupTcpProxy(kj::mv(api), client.getIoProvider(), tokenBytes, kj::str(localPort), kj::str(externalPort));

      auto handle = proxy.wait(client.getWaitScope());
      kj::NEVER_DONE.wait(client.getWaitScope());
      return true;
    }

  private:
    kj::ProcessContext& context;
    kj::String token;
    kj::String localPort;
    kj::String externalPort;
    kj::String sessionId;
    sandstorm::Handle::Client handle;
  };

} // namespace sandstorm

KJ_MAIN(sandstorm::TcpProxyListenerMain)
