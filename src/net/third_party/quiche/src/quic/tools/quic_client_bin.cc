// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A binary wrapper for QuicClient.
// Connects to a host using QUIC, sends a request to the provided URL, and
// displays the response.
//
// Some usage examples:
//
// Standard request/response:
//   quic_client www.google.com
//   quic_client www.google.com --quiet
//   quic_client www.google.com --port=443
//
// Use a specific version:
//   quic_client www.google.com --quic_version=23
//
// Send a POST instead of a GET:
//   quic_client www.google.com --body="this is a POST body"
//
// Append additional headers to the request:
//   quic_client www.google.com --headers="Header-A: 1234; Header-B: 5678"
//
// Connect to a host different to the URL being requested:
//   quic_client mail.google.com --host=www.google.com
//
// Connect to a specific IP:
//   IP=`dig www.google.com +short | head -1`
//   quic_client www.google.com --host=${IP}
//
// Send repeated requests and change ephemeral port between requests
//   quic_client www.google.com --num_requests=10
//
// Try to connect to a host which does not speak QUIC:
//   quic_client www.example.com

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "net/third_party/quiche/src/quic/core/quic_packets.h"
#include "net/third_party/quiche/src/quic/core/quic_server_id.h"
#include "net/third_party/quiche/src/quic/core/quic_utils.h"
#include "net/third_party/quiche/src/quic/core/quic_versions.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_default_proof_providers.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_ptr_util.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_socket_address.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_str_cat.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_string_piece.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_system_event_loop.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_text_utils.h"
#include "net/third_party/quiche/src/quic/tools/quic_client.h"
#include "net/third_party/quiche/src/quic/tools/quic_url.h"

namespace {

using quic::QuicSocketAddress;
using quic::QuicStringPiece;
using quic::QuicTextUtils;
using quic::QuicUrl;

class FakeProofVerifier : public quic::ProofVerifier {
 public:
  ~FakeProofVerifier() override {}
  quic::QuicAsyncStatus VerifyProof(
      const std::string& /*hostname*/,
      const uint16_t /*port*/,
      const std::string& /*server_config*/,
      quic::QuicTransportVersion /*quic_version*/,
      quic::QuicStringPiece /*chlo_hash*/,
      const std::vector<std::string>& /*certs*/,
      const std::string& /*cert_sct*/,
      const std::string& /*signature*/,
      const quic::ProofVerifyContext* /*context*/,
      std::string* /*error_details*/,
      std::unique_ptr<quic::ProofVerifyDetails>* /*details*/,
      std::unique_ptr<quic::ProofVerifierCallback> /*callback*/) override {
    return quic::QUIC_SUCCESS;
  }
  quic::QuicAsyncStatus VerifyCertChain(
      const std::string& /*hostname*/,
      const std::vector<std::string>& /*certs*/,
      const quic::ProofVerifyContext* /*context*/,
      std::string* /*error_details*/,
      std::unique_ptr<quic::ProofVerifyDetails>* /*details*/,
      std::unique_ptr<quic::ProofVerifierCallback> /*callback*/) override {
    return quic::QUIC_SUCCESS;
  }
  std::unique_ptr<quic::ProofVerifyContext> CreateDefaultContext() override {
    return nullptr;
  }
};

QuicSocketAddress LookupAddress(std::string host, std::string port) {
  addrinfo hint;
  memset(&hint, 0, sizeof(hint));
  hint.ai_protocol = IPPROTO_UDP;

  addrinfo* info_list = nullptr;
  int result = getaddrinfo(host.c_str(), port.c_str(), &hint, &info_list);
  if (result != 0) {
    QUIC_LOG(ERROR) << "Failed to look up " << host << ": "
                    << gai_strerror(result);
    return QuicSocketAddress();
  }

  CHECK(info_list != nullptr);
  std::unique_ptr<addrinfo, void (*)(addrinfo*)> info_list_owned(info_list,
                                                                 freeaddrinfo);
  return QuicSocketAddress(*info_list->ai_addr);
}

}  // namespace

DEFINE_QUIC_COMMAND_LINE_FLAG(
    std::string,
    host,
    "",
    "The IP or hostname to connect to. If not provided, the host "
    "will be derived from the provided URL.");

DEFINE_QUIC_COMMAND_LINE_FLAG(int32_t, port, 0, "The port to connect to.");

DEFINE_QUIC_COMMAND_LINE_FLAG(std::string,
                              body,
                              "",
                              "If set, send a POST with this body.");

DEFINE_QUIC_COMMAND_LINE_FLAG(
    std::string,
    body_hex,
    "",
    "If set, contents are converted from hex to ascii, before "
    "sending as body of a POST. e.g. --body_hex=\"68656c6c6f\"");

DEFINE_QUIC_COMMAND_LINE_FLAG(
    std::string,
    headers,
    "",
    "A semicolon separated list of key:value pairs to "
    "add to request headers.");

DEFINE_QUIC_COMMAND_LINE_FLAG(bool,
                              quiet,
                              false,
                              "Set to true for a quieter output experience.");

DEFINE_QUIC_COMMAND_LINE_FLAG(
    std::string,
    quic_version,
    "",
    "QUIC version to speak, e.g. 21. If not set, then all available "
    "versions are offered in the handshake. Also supports wire versions "
    "such as Q043 or T099.");

DEFINE_QUIC_COMMAND_LINE_FLAG(
    int32_t,
    quic_ietf_draft,
    0,
    "QUIC IETF draft number to use over the wire, e.g. 18. "
    "By default this sets quic_version to T099. "
    "This also enables required internal QUIC flags.");

DEFINE_QUIC_COMMAND_LINE_FLAG(
    bool,
    version_mismatch_ok,
    false,
    "If true, a version mismatch in the handshake is not considered a "
    "failure. Useful for probing a server to determine if it speaks "
    "any version of QUIC.");

DEFINE_QUIC_COMMAND_LINE_FLAG(
    bool,
    redirect_is_success,
    true,
    "If true, an HTTP response code of 3xx is considered to be a "
    "successful response, otherwise a failure.");

DEFINE_QUIC_COMMAND_LINE_FLAG(int32_t,
                              initial_mtu,
                              0,
                              "Initial MTU of the connection.");

DEFINE_QUIC_COMMAND_LINE_FLAG(
    int32_t,
    num_requests,
    1,
    "How many sequential requests to make on a single connection.");

DEFINE_QUIC_COMMAND_LINE_FLAG(bool,
                              disable_certificate_verification,
                              false,
                              "If true, don't verify the server certificate.");

DEFINE_QUIC_COMMAND_LINE_FLAG(
    bool,
    drop_response_body,
    false,
    "If true, drop response body immediately after it is received.");

int main(int argc, char* argv[]) {
  QuicSystemEventLoop event_loop("quic_client");
  const char* usage = "Usage: quic_client [options] <url>";

  // All non-flag arguments should be interpreted as URLs to fetch.
  std::vector<std::string> urls =
      quic::QuicParseCommandLineFlags(usage, argc, argv);
  if (urls.size() != 1) {
    quic::QuicPrintCommandLineFlagHelp(usage);
    exit(0);
  }

  QuicUrl url(urls[0], "https");
  std::string host = GetQuicFlag(FLAGS_host);
  if (host.empty()) {
    host = url.host();
  }
  int port = GetQuicFlag(FLAGS_port);
  if (port == 0) {
    port = url.port();
  }

  // Determine IP address to connect to from supplied hostname.
  QuicSocketAddress addr = LookupAddress(host, quic::QuicStrCat(port));
  if (!addr.IsInitialized()) {
    return 1;
  }
  std::cerr << "Resolved " << url.ToString() << " to " << addr.ToString()
            << std::endl;

  // Build the client, and try to connect.
  quic::QuicEpollServer epoll_server;
  quic::QuicServerId server_id(url.host(), port, false);
  quic::ParsedQuicVersionVector versions = quic::CurrentSupportedVersions();

  std::string quic_version_string = GetQuicFlag(FLAGS_quic_version);
  const int32_t quic_ietf_draft = GetQuicFlag(FLAGS_quic_ietf_draft);
  if (quic_ietf_draft > 0) {
    quic::QuicVersionInitializeSupportForIetfDraft(quic_ietf_draft);
    if (quic_version_string.length() == 0) {
      quic_version_string = "T099";
    }
  }
  if (quic_version_string.length() > 0) {
    if (quic_version_string[0] == 'T') {
      // ParseQuicVersionString checks quic_supports_tls_handshake.
      SetQuicFlag(&FLAGS_quic_supports_tls_handshake, true);
    }
    quic::ParsedQuicVersion parsed_quic_version =
        quic::ParseQuicVersionString(quic_version_string);
    if (parsed_quic_version.transport_version ==
        quic::QUIC_VERSION_UNSUPPORTED) {
      return 1;
    }
    versions.clear();
    versions.push_back(parsed_quic_version);
    quic::QuicEnableVersion(parsed_quic_version);
  }

  const int32_t num_requests(GetQuicFlag(FLAGS_num_requests));
  std::unique_ptr<quic::ProofVerifier> proof_verifier;
  if (GetQuicFlag(FLAGS_disable_certificate_verification)) {
    proof_verifier = quic::QuicMakeUnique<FakeProofVerifier>();
  } else {
    proof_verifier = quic::CreateDefaultProofVerifier();
  }
  quic::QuicClient client(addr, server_id, versions, &epoll_server,
                          std::move(proof_verifier));
  int32_t initial_mtu = GetQuicFlag(FLAGS_initial_mtu);
  client.set_initial_max_packet_length(
      initial_mtu != 0 ? initial_mtu : quic::kDefaultMaxPacketSize);
  client.set_drop_response_body(GetQuicFlag(FLAGS_drop_response_body));
  if (!client.Initialize()) {
    std::cerr << "Failed to initialize client." << std::endl;
    return 1;
  }
  if (!client.Connect()) {
    quic::QuicErrorCode error = client.session()->error();
    if (error == quic::QUIC_INVALID_VERSION) {
      std::cerr << "Server talks QUIC, but none of the versions supported by "
                << "this client: " << ParsedQuicVersionVectorToString(versions)
                << std::endl;
      // 0: No error.
      // 20: Failed to connect due to QUIC_INVALID_VERSION.
      return GetQuicFlag(FLAGS_version_mismatch_ok) ? 0 : 20;
    }
    std::cerr << "Failed to connect to " << addr.ToString()
              << ". Error: " << quic::QuicErrorCodeToString(error) << std::endl;
    return 1;
  }
  std::cerr << "Connected to " << addr.ToString() << std::endl;

  // Construct the string body from flags, if provided.
  std::string body = GetQuicFlag(FLAGS_body);
  if (!GetQuicFlag(FLAGS_body_hex).empty()) {
    DCHECK(GetQuicFlag(FLAGS_body).empty())
        << "Only set one of --body and --body_hex.";
    body = QuicTextUtils::HexDecode(GetQuicFlag(FLAGS_body_hex));
  }

  // Construct a GET or POST request for supplied URL.
  spdy::SpdyHeaderBlock header_block;
  header_block[":method"] = body.empty() ? "GET" : "POST";
  header_block[":scheme"] = url.scheme();
  header_block[":authority"] = url.HostPort();
  header_block[":path"] = url.PathParamsQuery();

  // Append any additional headers supplied on the command line.
  for (QuicStringPiece sp :
       QuicTextUtils::Split(GetQuicFlag(FLAGS_headers), ';')) {
    QuicTextUtils::RemoveLeadingAndTrailingWhitespace(&sp);
    if (sp.empty()) {
      continue;
    }
    std::vector<QuicStringPiece> kv = QuicTextUtils::Split(sp, ':');
    QuicTextUtils::RemoveLeadingAndTrailingWhitespace(&kv[0]);
    QuicTextUtils::RemoveLeadingAndTrailingWhitespace(&kv[1]);
    header_block[kv[0]] = kv[1];
  }

  // Make sure to store the response, for later output.
  client.set_store_response(true);

  for (int i = 0; i < num_requests; ++i) {
    // Send the request.
    client.SendRequestAndWaitForResponse(header_block, body, /*fin=*/true);

    // Print request and response details.
    if (!GetQuicFlag(FLAGS_quiet)) {
      std::cout << "Request:" << std::endl;
      std::cout << "headers:" << header_block.DebugString();
      if (!GetQuicFlag(FLAGS_body_hex).empty()) {
        // Print the user provided hex, rather than binary body.
        std::cout << "body:\n"
                  << QuicTextUtils::HexDump(
                         QuicTextUtils::HexDecode(GetQuicFlag(FLAGS_body_hex)))
                  << std::endl;
      } else {
        std::cout << "body: " << body << std::endl;
      }
      std::cout << std::endl;

      if (!client.preliminary_response_headers().empty()) {
        std::cout << "Preliminary response headers: "
                  << client.preliminary_response_headers() << std::endl;
        std::cout << std::endl;
      }

      std::cout << "Response:" << std::endl;
      std::cout << "headers: " << client.latest_response_headers() << std::endl;
      std::string response_body = client.latest_response_body();
      if (!GetQuicFlag(FLAGS_body_hex).empty()) {
        // Assume response is binary data.
        std::cout << "body:\n"
                  << QuicTextUtils::HexDump(response_body) << std::endl;
      } else {
        std::cout << "body: " << response_body << std::endl;
      }
      std::cout << "trailers: " << client.latest_response_trailers()
                << std::endl;
    }

    if (!client.connected()) {
      std::cerr << "Request caused connection failure. Error: "
                << quic::QuicErrorCodeToString(client.session()->error())
                << std::endl;
      return 1;
    }

    size_t response_code = client.latest_response_code();
    if (response_code >= 200 && response_code < 300) {
      std::cerr << "Request succeeded (" << response_code << ")." << std::endl;
    } else if (response_code >= 300 && response_code < 400) {
      if (GetQuicFlag(FLAGS_redirect_is_success)) {
        std::cerr << "Request succeeded (redirect " << response_code << ")."
                  << std::endl;
      } else {
        std::cerr << "Request failed (redirect " << response_code << ")."
                  << std::endl;
        return 1;
      }
    } else {
      std::cerr << "Request failed (" << response_code << ")." << std::endl;
      return 1;
    }

    // Change the ephemeral port if there are more requests to do.
    if (i + 1 < num_requests) {
      if (!client.ChangeEphemeralPort()) {
        std::cerr << "Failed to change ephemeral port." << std::endl;
        return 1;
      }
    }
  }

  return 0;
}