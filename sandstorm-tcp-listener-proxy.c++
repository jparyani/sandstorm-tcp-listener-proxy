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

namespace sandstorm {
  class TcpByteSteamImpl final : public sandstorm::ByteStream::Server {
  public:
    explicit TcpByteSteamImpl(sandstorm::ByteStream::Client&& downstream, kj::Own<kj::AsyncIoStream>&& stream) : downstream(downstream), stream(kj::mv(stream)), readerLoop(readLoop()) {
      // TODO(someday): allow for half open connections by handling readerLoop better
    }

    kj::Promise<void> write(WriteContext context) {
      auto data = context.getParams().getData();
      return stream->write(data.begin(), data.size());
    }

    kj::Promise<void> done(DoneContext context) {
      // TODO(soon): implement done
      // KJ_FAIL_REQUIRE("not implemented");
      return kj::READY_NOW;
    }

  private:
    kj::Promise<void> readLoop() {
      return stream->tryRead(buffer, 1, sizeof(buffer)).then([this](auto size) {
        auto req = downstream.writeRequest();
        req.setData(kj::ArrayPtr<capnp::byte>(buffer, size));
        return req.send().then([this](auto args) {
          return readLoop();
        });
      });
    }
    sandstorm::ByteStream::Client downstream;
    kj::Own<kj::AsyncIoStream> stream;
    kj::byte buffer[1024];
    kj::Promise<void> readerLoop;
  };

  class TcpPortImpl final : public sandstorm::TcpPort::Server {
  public:
    explicit TcpPortImpl(kj::AsyncIoProvider& provider, kj::String& port) : provider(provider), port(kj::heapString(port)) {}
    kj::Promise<void> connect(ConnectContext context) {
      return provider.getNetwork().parseAddress(kj::str("127.0.0.1:", port)).then([context](auto addr) mutable {
        return addr->connect().then([context](auto stream) mutable {
          context.getResults().setUpstream(kj::heap<TcpByteSteamImpl>(context.getParams().getDownstream(), kj::mv(stream)));
        });
      });
    }
    kj::AsyncIoProvider& provider;
    kj::String port;
  };


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

    kj::MainBuilder::Validity run() {
      capnp::EzRpcClient client("unix:/tmp/sandstorm-api");
      SandstormHttpBridge::Client restorer = client.getMain<SandstormHttpBridge>();

      auto request = restorer.getSandstormApiRequest();
      auto api = request.send().getApi();
      auto req = api.restoreRequest();
      req.setToken(token.asBytes());

      kj::Promise<void> promise = req.send().then([this, &client](auto args) mutable {
        auto req = args.getCap().template castAs<sandstorm::IpInterface>().listenTcpRequest();
        req.setPortNum(atoi(externalPort.cStr()));
        req.setPort(kj::heap<TcpPortImpl>(client.getIoProvider(), localPort));
        return req.send().then([this](auto args) mutable {
          handle = args.getHandle();
        });
      });

      // promise.wait(client.getWaitScope());
      kj::NEVER_DONE.wait(client.getWaitScope());
      return true;
    }

  private:
    kj::ProcessContext& context;
    kj::String token;
    kj::String localPort;
    kj::String externalPort;
    sandstorm::Handle::Client handle;
  };

} // namespace sandstorm

KJ_MAIN(sandstorm::TcpProxyListenerMain)
