/*
 *  Copyright (c) 2017, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <boost/thread.hpp>
#include <folly/FileUtil.h>
#include <folly/experimental/TestUtil.h>
#include <folly/io/async/AsyncSSLSocket.h>
#include <folly/io/async/AsyncServerSocket.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/portability/GTest.h>
#include <proxygen/httpserver/HTTPServer.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/httpserver/ScopedHTTPServer.h>
#include <proxygen/lib/utils/TestUtils.h>
#include <proxygen/lib/http/HTTPConnector.h>
#include <proxygen/httpclient/samples/curl/CurlClient.h>
#include <wangle/client/ssl/SSLSession.h>


using namespace folly;
using namespace proxygen;
using namespace testing;
using namespace CurlService;

using folly::AsyncSSLSocket;
using folly::AsyncServerSocket;
using folly::EventBaseManager;
using folly::SSLContext;
using folly::SSLContext;
using folly::SocketAddress;

namespace {

const std::string kTestDir = getContainingDirectory(__FILE__).str();

}

class ServerThread {
 private:
  boost::barrier barrier_{2};
  std::thread t_;
  HTTPServer* server_{nullptr};

 public:

  explicit ServerThread(HTTPServer* server) : server_(server) {}
  ~ServerThread() {
    if (server_) {
      server_->stop();
    }
    t_.join();
  }

  bool start() {
    bool throws = false;
    t_ = std::thread([&]() {
      server_->start([&]() { barrier_.wait(); },
                     [&](std::exception_ptr /*ex*/) {
                       throws = true;
                       server_ = nullptr;
                       barrier_.wait();
                     });
    });
    barrier_.wait();
    return !throws;
  }
};

TEST(MultiBind, HandlesListenFailures) {
  SocketAddress addr("127.0.0.1", 0);

  auto evb = EventBaseManager::get()->getEventBase();
  AsyncServerSocket::UniquePtr socket(new AsyncServerSocket(evb));
  socket->bind(addr);

  // Get the ephemeral port
  socket->getAddress(&addr);
  int port = addr.getPort();

  std::vector<HTTPServer::IPConfig> ips = {
    {
      folly::SocketAddress("127.0.0.1", port),
      HTTPServer::Protocol::HTTP
    }
  };

  HTTPServerOptions options;
  options.threads = 4;

  auto server = std::make_unique<HTTPServer>(std::move(options));

  // We have to bind both the sockets before listening on either
  server->bind(ips);

  // On kernel 2.6 trying to listen on a FD that another socket
  // has bound to fails. While in kernel 3.2 only when one socket tries
  // to listen on a FD that another socket is listening on fails.
  try {
    socket->listen(1024);
  } catch (const std::exception& ex) {
    return;
  }

  ServerThread st(server.get());
  EXPECT_FALSE(st.start());
}

TEST(HttpServerStartStop, TestRepeatStopCalls) {
  HTTPServerOptions options;
  auto server = std::make_unique<HTTPServer>(std::move(options));
  auto st = std::make_unique<ServerThread>(server.get());
  EXPECT_TRUE(st->start());

  server->stop();
  // Calling stop again should be benign.
  server->stop();
}

// Make an SSL connection to the server
class Cb : public folly::AsyncSocket::ConnectCallback {
 public:
  explicit Cb(folly::AsyncSSLSocket* sock) : sock_(sock) {}
  void connectSuccess() noexcept override {
    success = true;
    reusedSession = sock_->getSSLSessionReused();
    session.reset(sock_->getSSLSession());
    if (sock_->getPeerCert()) {
      // keeps this alive until Cb is destroyed, even if sock is closed
      peerCert_ = sock_->getPeerCert();
    }
    sock_->close();
  }

  void connectErr(const folly::AsyncSocketException&) noexcept override {
    success = false;
  }

  const X509* getPeerCert() { return peerCert_.get(); }

  bool success{false};
  bool reusedSession{false};
  wangle::SSLSessionPtr session;
  folly::AsyncSSLSocket* sock_{nullptr};
  folly::ssl::X509UniquePtr peerCert_{nullptr};
};

TEST(SSL, SSLTest) {
  HTTPServer::IPConfig cfg{
    folly::SocketAddress("127.0.0.1", 0),
      HTTPServer::Protocol::HTTP};
  wangle::SSLContextConfig sslCfg;
  sslCfg.isDefault = true;
  sslCfg.setCertificate(
    kTestDir + "certs/test_cert1.pem",
    kTestDir + "certs/test_key1.pem",
    "");
  cfg.sslConfigs.push_back(sslCfg);

  HTTPServerOptions options;
  options.threads = 4;

  auto server = std::make_unique<HTTPServer>(std::move(options));

  std::vector<HTTPServer::IPConfig> ips{cfg};
  server->bind(ips);

  ServerThread st(server.get());
  EXPECT_TRUE(st.start());

  folly::EventBase evb;
  auto ctx = std::make_shared<SSLContext>();
  folly::AsyncSSLSocket::UniquePtr sock(new folly::AsyncSSLSocket(ctx, &evb));
  Cb cb(sock.get());
  sock->connect(&cb, server->addresses().front().address, 1000);
  evb.loop();
  EXPECT_TRUE(cb.success);
}

class TestHandlerFactory : public RequestHandlerFactory {
 public:
  class TestHandler : public proxygen::RequestHandler {
    void onRequest(std::unique_ptr<proxygen::HTTPMessage>) noexcept override {}
    void onBody(std::unique_ptr<folly::IOBuf>) noexcept override {}
    void onUpgrade(proxygen::UpgradeProtocol) noexcept override {}

    void onEOM() noexcept override {
      ResponseBuilder(downstream_)
          .status(200, "OK")
          .body(IOBuf::copyBuffer("hello"))
          .sendWithEOM();
    }

    void requestComplete() noexcept override { delete this; }

    void onError(ProxygenError) noexcept override { delete this; }
  };

  RequestHandler* onRequest(RequestHandler*, HTTPMessage*) noexcept override {
    return new TestHandler();
  }

  void onServerStart(folly::EventBase*) noexcept override {}
  void onServerStop() noexcept override {}
};

std::pair<std::unique_ptr<HTTPServer>, std::unique_ptr<ServerThread>>
setupServer(bool allowInsecureConnectionsOnSecureServer = false,
            folly::Optional<wangle::TLSTicketKeySeeds> seeds = folly::none) {
  HTTPServer::IPConfig cfg{folly::SocketAddress("127.0.0.1", 0),
                           HTTPServer::Protocol::HTTP};
  wangle::SSLContextConfig sslCfg;
  sslCfg.isDefault = true;
  sslCfg.setCertificate(
      kTestDir + "certs/test_cert1.pem", kTestDir + "certs/test_key1.pem", "");
  cfg.sslConfigs.push_back(sslCfg);
  cfg.allowInsecureConnectionsOnSecureServer =
      allowInsecureConnectionsOnSecureServer;
  cfg.ticketSeeds = seeds;

  HTTPServerOptions options;
  options.threads = 4;
  options.handlerFactories =
      RequestHandlerChain().addThen<TestHandlerFactory>().build();

  auto server = std::make_unique<HTTPServer>(std::move(options));

  std::vector<HTTPServer::IPConfig> ips{cfg};
  server->bind(ips);

  auto st = std::make_unique<ServerThread>(server.get());
  EXPECT_TRUE(st->start());
  return std::make_pair(std::move(server), std::move(st));
}

TEST(SSL, TestAllowInsecureOnSecureServer) {
  std::unique_ptr<HTTPServer> server;
  std::unique_ptr<ServerThread> st;
  std::tie(server, st) = setupServer(true);

  folly::EventBase evb;
  URL url(folly::to<std::string>(
      "http://localhost:", server->addresses().front().address.getPort()));
  HTTPHeaders headers;
  CurlClient curl(&evb, HTTPMethod::GET, url, nullptr, headers, "");
  curl.setFlowControlSettings(64 * 1024);
  curl.setLogging(false);
  HHWheelTimer::UniquePtr timer{new HHWheelTimer(
      &evb,
      std::chrono::milliseconds(HHWheelTimer::DEFAULT_TICK_INTERVAL),
      AsyncTimeout::InternalEnum::NORMAL,
      std::chrono::milliseconds(1000))};
  HTTPConnector connector(&curl, timer.get());
  connector.connect(&evb,
                    server->addresses().front().address,
                    std::chrono::milliseconds(1000));
  evb.loop();
  auto response = curl.getResponse();
  EXPECT_EQ(200, response->getStatusCode());
}

TEST(SSL, DisallowInsecureOnSecureServer) {
  std::unique_ptr<HTTPServer> server;
  std::unique_ptr<ServerThread> st;
  std::tie(server, st) = setupServer(false);

  folly::EventBase evb;
  URL url(folly::to<std::string>(
      "http://localhost:", server->addresses().front().address.getPort()));
  HTTPHeaders headers;
  CurlClient curl(&evb, HTTPMethod::GET, url, nullptr, headers, "");
  curl.setFlowControlSettings(64 * 1024);
  curl.setLogging(false);
  HHWheelTimer::UniquePtr timer{new HHWheelTimer(
      &evb,
      std::chrono::milliseconds(HHWheelTimer::DEFAULT_TICK_INTERVAL),
      AsyncTimeout::InternalEnum::NORMAL,
      std::chrono::milliseconds(1000))};
  HTTPConnector connector(&curl, timer.get());
  connector.connect(&evb,
                    server->addresses().front().address,
                    std::chrono::milliseconds(1000));
  evb.loop();
  auto response = curl.getResponse();
  EXPECT_EQ(nullptr, response);
}

TEST(SSL, TestResumptionWithTickets) {
  std::unique_ptr<HTTPServer> server;
  std::unique_ptr<ServerThread> st;
  wangle::TLSTicketKeySeeds seeds;
  seeds.currentSeeds.push_back(hexlify("hello"));
  std::tie(server, st) = setupServer(false, seeds);

  folly::EventBase evb;
  auto ctx = std::make_shared<SSLContext>();
  folly::AsyncSSLSocket::UniquePtr sock(new folly::AsyncSSLSocket(ctx, &evb));
  Cb cb(sock.get());
  sock->connect(&cb, server->addresses().front().address, 1000);
  evb.loop();
  ASSERT_TRUE(cb.success);
  ASSERT_NE(nullptr, cb.session.get());
  ASSERT_FALSE(cb.reusedSession);

  folly::AsyncSSLSocket::UniquePtr sock2(new folly::AsyncSSLSocket(ctx, &evb));
  sock2->setSSLSession(cb.session.get());
  Cb cb2(sock2.get());
  sock2->connect(&cb2, server->addresses().front().address, 1000);
  evb.loop();
  ASSERT_TRUE(cb2.success);
  ASSERT_NE(nullptr, cb2.session.get());
  ASSERT_TRUE(cb2.reusedSession);
}

TEST(SSL, TestResumptionAfterUpdateFails) {
  std::unique_ptr<HTTPServer> server;
  std::unique_ptr<ServerThread> st;
  wangle::TLSTicketKeySeeds seeds;
  seeds.currentSeeds.push_back(hexlify("hello"));
  std::tie(server, st) = setupServer(false, seeds);

  folly::EventBase evb;
  auto ctx = std::make_shared<SSLContext>();
  folly::AsyncSSLSocket::UniquePtr sock(new folly::AsyncSSLSocket(ctx, &evb));
  Cb cb(sock.get());
  sock->connect(&cb, server->addresses().front().address, 1000);
  evb.loop();
  ASSERT_TRUE(cb.success);
  ASSERT_NE(nullptr, cb.session.get());
  ASSERT_FALSE(cb.reusedSession);

  wangle::TLSTicketKeySeeds newSeeds;
  newSeeds.currentSeeds.push_back(hexlify("goodbyte"));
  server->updateTicketSeeds(newSeeds);

  folly::AsyncSSLSocket::UniquePtr sock2(new folly::AsyncSSLSocket(ctx, &evb));
  sock2->setSSLSession(cb.session.get());
  Cb cb2(sock2.get());
  sock2->connect(&cb2, server->addresses().front().address, 1000);
  evb.loop();
  ASSERT_TRUE(cb2.success);
  ASSERT_NE(nullptr, cb2.session.get());
  ASSERT_FALSE(cb2.reusedSession);

  folly::AsyncSSLSocket::UniquePtr sock3(new folly::AsyncSSLSocket(ctx, &evb));
  sock3->setSSLSession(cb2.session.get());
  Cb cb3(sock3.get());
  sock3->connect(&cb3, server->addresses().front().address, 1000);
  evb.loop();
  ASSERT_TRUE(cb3.success);
  ASSERT_NE(nullptr, cb3.session.get());
  ASSERT_TRUE(cb3.reusedSession);
}

TEST(SSL, TestUpdateTLSCredentials) {
  // Set up a temporary file with credentials that we will update
  folly::test::TemporaryFile credFile;
  auto copyCreds = [path = credFile.path()](const std::string& certFile,
                                        const std::string& keyFile) {
    std::string certData, keyData;
    folly::readFile(certFile.c_str(), certData);
    folly::writeFile(certData, path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    folly::writeFile(std::string("\n"), path.c_str(), O_WRONLY | O_APPEND);
    folly::readFile(keyFile.c_str(), keyData);
    folly::writeFile(keyData, path.c_str(), O_WRONLY | O_APPEND);
  };

  auto getCertDigest = [&](const X509* x) -> std::string {
    unsigned int n;
    unsigned char md[EVP_MAX_MD_SIZE];
    const EVP_MD* dig = EVP_sha256();

    if (!X509_digest(x, dig, md, &n)) {
      throw std::runtime_error("Cannot calculate digest");
    }
    return std::string((const char*)md, n);
  };

  HTTPServer::IPConfig cfg{folly::SocketAddress("127.0.0.1", 0),
                           HTTPServer::Protocol::HTTP};
  wangle::SSLContextConfig sslCfg;
  sslCfg.isDefault = true;
  copyCreds(kTestDir + "certs/test_cert1.pem",
            kTestDir + "certs/test_key1.pem");
  sslCfg.setCertificate(credFile.path().string(), credFile.path().string(), "");
  cfg.sslConfigs.push_back(sslCfg);

  HTTPServerOptions options;
  options.threads = 4;

  auto server = std::make_unique<HTTPServer>(std::move(options));

  std::vector<HTTPServer::IPConfig> ips{cfg};
  server->bind(ips);

  ServerThread st(server.get());
  EXPECT_TRUE(st.start());

  // First connection which should return old cert
  folly::EventBase evb;
  auto ctx = std::make_shared<SSLContext>();
  std::string certDigest1, certDigest2;

  // Connect and store digest of server cert
  auto connectAndFetchServerCert = [&]() -> std::string {
    folly::AsyncSSLSocket::UniquePtr sock(new folly::AsyncSSLSocket(ctx, &evb));
    Cb cb(sock.get());
    sock->connect(&cb, server->addresses().front().address, 1000);
    evb.loop();
    EXPECT_TRUE(cb.success);

    auto x509 = cb.getPeerCert();
    EXPECT_NE(x509, nullptr);
    return getCertDigest(x509);
  };

  // Original cert
  auto cert1 = connectAndFetchServerCert();
  EXPECT_EQ(cert1.length(), SHA256_DIGEST_LENGTH);

  // Update cert/key
  copyCreds(kTestDir + "certs/test_cert2.pem",
            kTestDir + "certs/test_key2.pem");
  server->updateTLSCredentials();
  evb.loop();

  // Should get new cert
  auto cert2 = connectAndFetchServerCert();
  EXPECT_EQ(cert2.length(), SHA256_DIGEST_LENGTH);
  EXPECT_NE(cert1, cert2);
}

TEST(GetListenSocket, TestNoBootstrap) {
  HTTPServerOptions options;
  auto server = std::make_unique<HTTPServer>(std::move(options));
  auto st = std::make_unique<ServerThread>(server.get());
  EXPECT_TRUE(st->start());

  auto socketFd = server->getListenSocket();
  ASSERT_EQ(-1, socketFd);
}

TEST(GetListenSocket, TestBootstrapWithNoBinding) {
  std::unique_ptr<HTTPServer> server;
  std::unique_ptr<ServerThread> st;
  wangle::TLSTicketKeySeeds seeds;
  seeds.currentSeeds.push_back(hexlify("hello"));
  std::tie(server, st) = setupServer(false, seeds);

  // Stop listening on socket
  server->stopListening();

  auto socketFd = server->getListenSocket();
  ASSERT_EQ(-1, socketFd);
}

TEST(GetListenSocket, TestBootstrapWithBinding) {
  std::unique_ptr<HTTPServer> server;
  std::unique_ptr<ServerThread> st;
  wangle::TLSTicketKeySeeds seeds;
  seeds.currentSeeds.push_back(hexlify("hello"));
  std::tie(server, st) = setupServer(false, seeds);

  auto socketFd = server->getListenSocket();
  ASSERT_NE(-1, socketFd);
}

TEST(UseExistingSocket, TestWithExistingAsyncServerSocket) {
  auto evb = EventBaseManager::get()->getEventBase();
  AsyncServerSocket::UniquePtr serverSocket(new folly::AsyncServerSocket);
  serverSocket->bind(0);

  HTTPServer::IPConfig cfg{folly::SocketAddress("127.0.0.1", 0),
                           HTTPServer::Protocol::HTTP};
  std::vector<HTTPServer::IPConfig> ips{cfg};

  HTTPServerOptions options;
  options.handlerFactories =
      RequestHandlerChain().addThen<TestHandlerFactory>().build();
  // Use the existing AsyncServerSocket for binding
  auto existingFd = serverSocket->getSocket();
  options.useExistingSocket(std::move(serverSocket));

  auto server = std::make_unique<HTTPServer>(std::move(options));
  auto st = std::make_unique<ServerThread>(server.get());
  server->bind(ips);

  EXPECT_TRUE(st->start());

  auto socketFd = server->getListenSocket();
  ASSERT_EQ(existingFd, socketFd);
}

TEST(UseExistingSocket, TestWithSocketFd) {
  auto evb = EventBaseManager::get()->getEventBase();
  AsyncServerSocket::UniquePtr serverSocket(new folly::AsyncServerSocket);
  serverSocket->bind(0);

  HTTPServer::IPConfig cfg{folly::SocketAddress("127.0.0.1", 0),
                           HTTPServer::Protocol::HTTP};
  HTTPServerOptions options;
  options.handlerFactories =
      RequestHandlerChain().addThen<TestHandlerFactory>().build();
  // Use the socket fd from the existing AsyncServerSocket for binding
  auto existingFd = serverSocket->getSocket();
  options.useExistingSocket(existingFd);

  auto server = std::make_unique<HTTPServer>(std::move(options));
  auto st = std::make_unique<ServerThread>(server.get());
  std::vector<HTTPServer::IPConfig> ips{cfg};
  server->bind(ips);


  EXPECT_TRUE(st->start());

  auto socketFd = server->getListenSocket();
  ASSERT_EQ(existingFd, socketFd);
}

TEST(UseExistingSocket, TestWithMultipleSocketFds) {
  auto evb = EventBaseManager::get()->getEventBase();
  AsyncServerSocket::UniquePtr serverSocket(new folly::AsyncServerSocket);
  serverSocket->bind(0);
  try {
    serverSocket->bind(1024);
  } catch (const std::exception& ex) {
    // This is fine because we are trying to bind to multiple ports
  }

  HTTPServer::IPConfig cfg{folly::SocketAddress("127.0.0.1", 0),
                           HTTPServer::Protocol::HTTP};
  HTTPServerOptions options;
  options.handlerFactories =
      RequestHandlerChain().addThen<TestHandlerFactory>().build();
  // Use the socket fd from the existing AsyncServerSocket for binding
  auto existingFds = serverSocket->getSockets();
  options.useExistingSockets(existingFds);

  auto server = std::make_unique<HTTPServer>(std::move(options));
  auto st = std::make_unique<ServerThread>(server.get());
  std::vector<HTTPServer::IPConfig> ips{cfg};
  server->bind(ips);


  EXPECT_TRUE(st->start());

  auto socketFd = server->getListenSocket();
  ASSERT_EQ(existingFds[0], socketFd);
}

class ScopedServerTest : public testing::Test {
 public:
  void SetUp() override {
    timer_.reset(new HHWheelTimer(
        &evb_,
        std::chrono::milliseconds(HHWheelTimer::DEFAULT_TICK_INTERVAL),
        AsyncTimeout::InternalEnum::NORMAL,
        std::chrono::milliseconds(1000)));
  }

 protected:
  std::unique_ptr<ScopedHTTPServer>
  createScopedServer() {
    auto opts = createDefaultOpts();
    auto res = ScopedHTTPServer::start(cfg_, std::move(opts));
    auto addresses = res->getAddresses();
    address_ = addresses.front().address;
    return res;
  }

  std::unique_ptr<CurlClient> connectSSL() {
    URL url(
        folly::to<std::string>("https://localhost:", address_.getPort()));
    HTTPHeaders headers;
    auto client = std::make_unique<CurlClient>(
      &evb_, HTTPMethod::GET, url, nullptr, headers, "");
    client->setFlowControlSettings(64 * 1024);
    client->setLogging(false);
    client->initializeSsl("", "http/1.1");
    HTTPConnector connector(client.get(), timer_.get());
    connector.connectSSL(
      &evb_,
      address_,
      client->getSSLContext(),
      nullptr,
      std::chrono::milliseconds(1000));
    evb_.loop();
    return client;
  }

  std::unique_ptr<CurlClient> connectPlainText() {
    URL url(
        folly::to<std::string>("http://localhost:", address_.getPort()));
    HTTPHeaders headers;
    auto client = std::make_unique<CurlClient>(
      &evb_, HTTPMethod::GET, url, nullptr, headers, "");
    client->setFlowControlSettings(64 * 1024);
    client->setLogging(false);
    HTTPConnector connector(client.get(), timer_.get());
    connector.connect(
      &evb_,
      address_,
      std::chrono::milliseconds(1000));
    evb_.loop();
    return client;
  }

  HTTPServerOptions createDefaultOpts() {
    HTTPServerOptions res;
    res.handlerFactories =
      RequestHandlerChain().addThen<TestHandlerFactory>().build();
    res.threads = 4;
    return res;
  }

  folly::EventBase evb_;
  folly::SocketAddress address_;
  HHWheelTimer::UniquePtr timer_;
  HTTPServer::IPConfig cfg_{
      folly::SocketAddress("127.0.0.1", 0),
        HTTPServer::Protocol::HTTP};
};

TEST_F(ScopedServerTest, start) {
  auto server = createScopedServer();
  auto client = connectPlainText();
  auto resp = client->getResponse();
  EXPECT_EQ(200, resp->getStatusCode());
}

TEST_F(ScopedServerTest, startStrictSSL) {
  wangle::SSLContextConfig sslCfg;
  sslCfg.isDefault = true;
  sslCfg.setCertificate(
    "/path/should/not/exist",
    "/path/should/not/exist",
    "");
  cfg_.sslConfigs.push_back(sslCfg);
  EXPECT_DEATH(createScopedServer(), "");
}

TEST_F(ScopedServerTest, startNotStrictSSL) {
  wangle::SSLContextConfig sslCfg;
  sslCfg.isDefault = true;
  sslCfg.setCertificate(
    "/path/should/not/exist",
    "/path/should/not/exist",
    "");
  cfg_.strictSSL = false;
  cfg_.sslConfigs.push_back(sslCfg);
  auto server = createScopedServer();
  auto client = connectPlainText();
  auto resp = client->getResponse();
  EXPECT_EQ(200, resp->getStatusCode());
}

TEST_F(ScopedServerTest, startSSLWithInsecure) {
  wangle::SSLContextConfig sslCfg;
  sslCfg.isDefault = true;
  sslCfg.setCertificate(
    kTestDir + "certs/test_cert1.pem",
    kTestDir + "certs/test_key1.pem",
    "");
  cfg_.sslConfigs.push_back(sslCfg);
  cfg_.allowInsecureConnectionsOnSecureServer = true;
  auto server = createScopedServer();
  auto client = connectPlainText();
  auto resp = client->getResponse();
  EXPECT_EQ(200, resp->getStatusCode());

  client = connectSSL();
  resp = client->getResponse();
  EXPECT_EQ(200, resp->getStatusCode());
}
