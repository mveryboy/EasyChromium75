// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/services/device_sync/cryptauth_client_impl.h"

#include <memory>
#include <utility>

#include "base/bind.h"
#include "base/command_line.h"
#include "base/memory/ptr_util.h"
#include "chromeos/components/multidevice/logging/logging.h"
#include "chromeos/services/device_sync/proto/cryptauth_devicesync.pb.h"
#include "chromeos/services/device_sync/proto/cryptauth_enrollment.pb.h"
#include "chromeos/services/device_sync/switches.h"
#include "services/identity/public/cpp/identity_manager.h"
#include "services/identity/public/cpp/primary_account_access_token_fetcher.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace chromeos {

namespace device_sync {

namespace {

// Default URL of Google APIs endpoint hosting CryptAuth v1.
const char kDefaultCryptAuthV1HTTPHost[] = "https://www.googleapis.com";

// URL subpath hosting the CryptAuth v1 service.
const char kCryptAuthV1Path[] = "cryptauth/v1/";

// URL subpaths for each CryptAuth v1 API.
const char kGetMyDevicesPath[] = "deviceSync/getmydevices";
const char kFindEligibleUnlockDevicesPath[] =
    "deviceSync/findeligibleunlockdevices";
const char kFindEligibleForPromotionPath[] =
    "deviceSync/findeligibleforpromotion";
const char kSendDeviceSyncTicklePath[] = "deviceSync/senddevicesynctickle";
const char kToggleEasyUnlockPath[] = "deviceSync/toggleeasyunlock";
const char kSetupEnrollmentPath[] = "enrollment/setup";
const char kFinishEnrollmentPath[] = "enrollment/finish";

// Default URL of Google APIs endpoint hosting CryptAuth v2 Enrollment.
const char kDefaultCryptAuthV2EnrollmentHTTPHost[] =
    "https://cryptauthenrollment.googleapis.com";

// Default URL of Google APIs endpoint hosting CryptAuth v2 DeviceSync.
const char kDefaultCryptAuthV2DeviceSyncHTTPHost[] =
    "https://cryptauthdevicesync.googleapis.com";

// URL subpaths for each CryptAuth v2 API endpoint.
// Note: Although "v1" is part of the path names, these are in fact v2 API
//       endpoints. Also, the "/" is necessary for GURL::Resolve() to parse the
//       paths correctly; otherwise, ":" is interpreted as a scheme delimiter.
const char kSyncKeysPath[] = "/v1:syncKeys";
const char kEnrollKeysPath[] = "/v1:enrollKeys";
const char kSyncMetadataPath[] = "/v1:syncMetadata";
const char kShareGroupPrivateKeyPath[] = "/v1:shareGroupPrivateKey";
const char kBatchNotifyGroupDevicesPath[] = "/v1:batchNotifyGroupDevices";
const char kBatchGetFeatureStatusesPath[] = "/v1:batchGetFeatureStatuses";
const char kBatchSetFeatureStatusesPath[] = "/v1:batchSetFeatureStatuses";

// Query string of the API URL indicating that the response should be in a
// serialized protobuf format.
const char kQueryProtobuf[] = "?alt=proto";

const char kCryptAuthOAuth2Scope[] =
    "https://www.googleapis.com/auth/cryptauth";

// Creates the full CryptAuth v1 URL for endpoint to the API with
// |request_path|.
GURL CreateV1RequestUrl(const std::string& request_path) {
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  GURL google_apis_url = command_line->HasSwitch(switches::kCryptAuthHTTPHost)
                             ? GURL(command_line->GetSwitchValueASCII(
                                   switches::kCryptAuthHTTPHost))
                             : GURL(kDefaultCryptAuthV1HTTPHost);
  return google_apis_url.Resolve(kCryptAuthV1Path + request_path +
                                 kQueryProtobuf);
}

// Creates the full URL for endpoint to the CryptAuth v2 Enrollment API with
// |request_path|.
GURL CreateV2EnrollmentRequestUrl(const std::string& request_path) {
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  GURL google_apis_url =
      command_line->HasSwitch(switches::kCryptAuthV2EnrollmentHTTPHost)
          ? GURL(command_line->GetSwitchValueASCII(
                switches::kCryptAuthV2EnrollmentHTTPHost))
          : GURL(kDefaultCryptAuthV2EnrollmentHTTPHost);
  return google_apis_url.Resolve(request_path + kQueryProtobuf);
}

// Creates the full URL for endpoint to the CryptAuth v2 DeviceSync API with
// |request_path|.
GURL CreateV2DeviceSyncRequestUrl(const std::string& request_path) {
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  GURL google_apis_url =
      command_line->HasSwitch(switches::kCryptAuthV2DeviceSyncHTTPHost)
          ? GURL(command_line->GetSwitchValueASCII(
                switches::kCryptAuthV2DeviceSyncHTTPHost))
          : GURL(kDefaultCryptAuthV2DeviceSyncHTTPHost);
  return google_apis_url.Resolve(request_path + kQueryProtobuf);
}

}  // namespace

CryptAuthClientImpl::CryptAuthClientImpl(
    std::unique_ptr<CryptAuthApiCallFlow> api_call_flow,
    identity::IdentityManager* identity_manager,
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    const cryptauth::DeviceClassifier& device_classifier)
    : api_call_flow_(std::move(api_call_flow)),
      identity_manager_(identity_manager),
      url_loader_factory_(std::move(url_loader_factory)),
      device_classifier_(device_classifier),
      has_call_started_(false),
      weak_ptr_factory_(this) {}

CryptAuthClientImpl::~CryptAuthClientImpl() {}

void CryptAuthClientImpl::GetMyDevices(
    const cryptauth::GetMyDevicesRequest& request,
    const GetMyDevicesCallback& callback,
    const ErrorCallback& error_callback,
    const net::PartialNetworkTrafficAnnotationTag& partial_traffic_annotation) {
  MakeApiCall(CreateV1RequestUrl(kGetMyDevicesPath),
              RequestWithDeviceClassifierSet(request), callback, error_callback,
              partial_traffic_annotation);
}

void CryptAuthClientImpl::FindEligibleUnlockDevices(
    const cryptauth::FindEligibleUnlockDevicesRequest& request,
    const FindEligibleUnlockDevicesCallback& callback,
    const ErrorCallback& error_callback) {
  net::PartialNetworkTrafficAnnotationTag partial_traffic_annotation =
      net::DefinePartialNetworkTrafficAnnotation(
          "cryptauth_find_eligible_unlock_devices", "oauth2_api_call_flow",
          R"(
      semantics {
        sender: "CryptAuth Device Manager"
        description:
          "Gets the list of mobile devices that can be used by Smart Lock to "
          "unlock the current device."
        trigger:
          "This request is sent when the user starts the Smart Lock setup flow."
        data: "OAuth 2.0 token and the device's public key."
        destination: GOOGLE_OWNED_SERVICE
      }
      policy {
        setting:
          "This feature cannot be disabled in settings, but the request will "
          "only be sent if the user explicitly tries to enable Smart Lock "
          "(EasyUnlock), i.e. starts the setup flow."
        chrome_policy {
          EasyUnlockAllowed {
            EasyUnlockAllowed: false
          }
        }
      })");
  MakeApiCall(CreateV1RequestUrl(kFindEligibleUnlockDevicesPath),
              RequestWithDeviceClassifierSet(request), callback, error_callback,
              partial_traffic_annotation);
}

void CryptAuthClientImpl::FindEligibleForPromotion(
    const cryptauth::FindEligibleForPromotionRequest& request,
    const FindEligibleForPromotionCallback& callback,
    const ErrorCallback& error_callback) {
  net::PartialNetworkTrafficAnnotationTag partial_traffic_annotation =
      net::DefinePartialNetworkTrafficAnnotation(
          "cryptauth_find_eligible_for_promotion", "oauth2_api_call_flow",
          R"(
      semantics {
        sender: "Promotion Manager"
        description:
          "Return whether the current device is eligible for a Smart Lock promotion."
        trigger:
          "This request is sent when the user starts the Smart Lock setup flow."
        data: "OAuth 2.0 token and the device's public key."
        destination: GOOGLE_OWNED_SERVICE
      }
      policy {
        setting:
          "This feature cannot be disabled in settings"
        chrome_policy {
          EasyUnlockAllowed {
            EasyUnlockAllowed: false
          }
        }
      })");
  MakeApiCall(CreateV1RequestUrl(kFindEligibleForPromotionPath),
              RequestWithDeviceClassifierSet(request), callback, error_callback,
              partial_traffic_annotation);
}

void CryptAuthClientImpl::SendDeviceSyncTickle(
    const cryptauth::SendDeviceSyncTickleRequest& request,
    const SendDeviceSyncTickleCallback& callback,
    const ErrorCallback& error_callback,
    const net::PartialNetworkTrafficAnnotationTag& partial_traffic_annotation) {
  MakeApiCall(CreateV1RequestUrl(kSendDeviceSyncTicklePath),
              RequestWithDeviceClassifierSet(request), callback, error_callback,
              partial_traffic_annotation);
}

void CryptAuthClientImpl::ToggleEasyUnlock(
    const cryptauth::ToggleEasyUnlockRequest& request,
    const ToggleEasyUnlockCallback& callback,
    const ErrorCallback& error_callback) {
  net::PartialNetworkTrafficAnnotationTag partial_traffic_annotation =
      net::DefinePartialNetworkTrafficAnnotation("cryptauth_toggle_easyunlock",
                                                 "oauth2_api_call_flow", R"(
      semantics {
        sender: "CryptAuth Device Manager"
        description: "Enables Smart Lock (EasyUnlock) for the current device."
        trigger:
          "This request is send after the user goes through the EasyUnlock "
          "setup flow."
        data: "OAuth 2.0 token and the device public key."
        destination: GOOGLE_OWNED_SERVICE
      }
      policy {
        setting:
          "This feature cannot be disabled in settings, but the request will "
          "only be send if the user explicitly enables Smart Lock "
          "(EasyUnlock), i.e. uccessfully complete the setup flow."
        chrome_policy {
          EasyUnlockAllowed {
            EasyUnlockAllowed: false
          }
        }
      })");
  MakeApiCall(CreateV1RequestUrl(kToggleEasyUnlockPath),
              RequestWithDeviceClassifierSet(request), callback, error_callback,
              partial_traffic_annotation);
}

void CryptAuthClientImpl::SetupEnrollment(
    const cryptauth::SetupEnrollmentRequest& request,
    const SetupEnrollmentCallback& callback,
    const ErrorCallback& error_callback) {
  net::PartialNetworkTrafficAnnotationTag partial_traffic_annotation =
      net::DefinePartialNetworkTrafficAnnotation(
          "cryptauth_enrollment_flow_setup", "oauth2_api_call_flow", R"(
      semantics {
        sender: "CryptAuth Device Manager"
        description: "Starts the CryptAuth registration flow."
        trigger:
          "Occurs periodically, at least once a month, because if the device "
          "does not re-enroll for more than a specific number of days "
          "(currently 45) it will be removed from the server."
        data:
          "Various device information (public key, bluetooth MAC address, "
          "model, OS version, screen size, manufacturer, has screen lock "
          "enabled), and OAuth 2.0 token."
        destination: GOOGLE_OWNED_SERVICE
      }
      policy {
        setting:
          "This feature cannot be disabled by settings. However, this request "
          "is made only for signed-in users."
        chrome_policy {
          SigninAllowed {
            SigninAllowed: false
          }
        }
      })");
  MakeApiCall(CreateV1RequestUrl(kSetupEnrollmentPath),
              RequestWithDeviceClassifierSet(request), callback, error_callback,
              partial_traffic_annotation);
}

void CryptAuthClientImpl::FinishEnrollment(
    const cryptauth::FinishEnrollmentRequest& request,
    const FinishEnrollmentCallback& callback,
    const ErrorCallback& error_callback) {
  net::PartialNetworkTrafficAnnotationTag partial_traffic_annotation =
      net::DefinePartialNetworkTrafficAnnotation(
          "cryptauth_enrollment_flow_finish", "oauth2_api_call_flow", R"(
      semantics {
        sender: "CryptAuth Device Manager"
        description: "Finishes the CryptAuth registration flow."
        trigger:
          "Occurs periodically, at least once a month, because if the device "
          "does not re-enroll for more than a specific number of days "
          "(currently 45) it will be removed from the server."
        data: "OAuth 2.0 token."
        destination: GOOGLE_OWNED_SERVICE
      }
      policy {
        setting:
          "This feature cannot be disabled by settings. However, this request "
          "is made only for signed-in users."
        chrome_policy {
          SigninAllowed {
            SigninAllowed: false
          }
        }
      })");
  MakeApiCall(CreateV1RequestUrl(kFinishEnrollmentPath),
              RequestWithDeviceClassifierSet(request), callback, error_callback,
              partial_traffic_annotation);
}

void CryptAuthClientImpl::SyncKeys(const cryptauthv2::SyncKeysRequest& request,
                                   const SyncKeysCallback& callback,
                                   const ErrorCallback& error_callback) {
  net::PartialNetworkTrafficAnnotationTag partial_traffic_annotation =
      net::DefinePartialNetworkTrafficAnnotation(
          "cryptauth_v2_enrollment_flow_sync_keys", "oauth2_api_call_flow", R"(
      semantics {
        sender: "CryptAuth V2 Enroller"
        description: "Starts the CryptAuth v2 Enrollment flow."
        trigger:
          "Occurs periodically at a period provided by CryptAuth in the "
          "previous SyncKeysResponse's ClientDirective. The client can also "
          "bypass the periodic schedule and immediately trigger a "
          "SyncKeysRequest."
        data:
          "A list of all keys used by the client; metadata about the "
          "local device's feature support, hardware, etc.; and an OAuth 2.0 "
          "token."
        destination: GOOGLE_OWNED_SERVICE
      }
      policy {
        setting:
          "This feature cannot be disabled by settings. However, this request "
          "is made only for signed-in users."
        chrome_policy {
          SigninAllowed {
            SigninAllowed: false
          }
        }
      })");
  MakeApiCall(CreateV2EnrollmentRequestUrl(kSyncKeysPath), request, callback,
              error_callback, partial_traffic_annotation);
}

void CryptAuthClientImpl::EnrollKeys(
    const cryptauthv2::EnrollKeysRequest& request,
    const EnrollKeysCallback& callback,
    const ErrorCallback& error_callback) {
  net::PartialNetworkTrafficAnnotationTag partial_traffic_annotation =
      net::DefinePartialNetworkTrafficAnnotation(
          "cryptauth_v2_enrollment_flow_enroll_keys", "oauth2_api_call_flow",
          R"(
      semantics {
        sender: "CryptAuth V2 Enroller"
        description: "Finishes the CryptAuth v2 Enrollment flow."
        trigger:
          "The second part of the v2 Enrollment flow. Sent after the client "
          "receives a SyncKeysResponse from CryptAuth, requesting the client "
          "create new keys."
        data:
          "A list of newly created key material and necessary proofs for "
          "verifying the keys."
        destination: GOOGLE_OWNED_SERVICE
      }
      policy {
        setting:
          "This feature cannot be disabled by settings. However, this request "
          "is made only for signed-in users."
        chrome_policy {
          SigninAllowed {
            SigninAllowed: false
          }
        }
      })");
  MakeApiCall(CreateV2EnrollmentRequestUrl(kEnrollKeysPath), request, callback,
              error_callback, partial_traffic_annotation);
}

void CryptAuthClientImpl::SyncMetadata(
    const cryptauthv2::SyncMetadataRequest& request,
    const SyncMetadataCallback& callback,
    const ErrorCallback& error_callback) {
  net::PartialNetworkTrafficAnnotationTag partial_traffic_annotation =
      net::DefinePartialNetworkTrafficAnnotation(
          "cryptauth_v2_devicesync_sync_metadata", "oauth2_api_call_flow",
          R"(
      semantics {
        sender: "CryptAuth V2 Device Manager"
        description:
          "Sends device metadata to CryptAuth and recieves metadata data for "
          "the user's other devices."
        trigger:
          "CryptAuth will potentially instruct the client to invoke "
          "SyncMetadata at the end of enrollment flows, which occur "
          "periodically, or via GCM messages. There is no dedicated periodic "
          "scheduling. The client can also force a SyncMetadataRequest."
        data:
          "Sends the device's encrypted metadata. Receives encrypted metadata "
          "from other user devices. Can potentially receive the group public "
          "key and/or the encrypted group private key, used for the encryption "
          "and decryption of all device metadata."
        destination: GOOGLE_OWNED_SERVICE
      }
      policy {
        setting:
          "This feature cannot be disabled by settings. However, this request "
          "is made only for signed-in users."
        chrome_policy {
          SigninAllowed {
            SigninAllowed: false
          }
        }
      })");
  MakeApiCall(CreateV2DeviceSyncRequestUrl(kSyncMetadataPath), request,
              callback, error_callback, partial_traffic_annotation);
}

void CryptAuthClientImpl::ShareGroupPrivateKey(
    const cryptauthv2::ShareGroupPrivateKeyRequest& request,
    const ShareGroupPrivateKeyCallback& callback,
    const ErrorCallback& error_callback) {
  net::PartialNetworkTrafficAnnotationTag partial_traffic_annotation =
      net::DefinePartialNetworkTrafficAnnotation(
          "cryptauth_v2_devicesync_share_group_private_key",
          "oauth2_api_call_flow",
          R"(
      semantics {
        sender: "CryptAuth V2 Device Manager"
        description:
          "The device shares the group private key by encrypting it with the "
          "public key of the user's other devices."
        trigger:
          "If the SyncMetadataResponse indicates that other user devices need "
          "the group private key, then the client immediately invokes "
          "ShareGroupPrivateKey."
        data:
          "The group private key encrypted with the public key of other user "
          "devices."
        destination: GOOGLE_OWNED_SERVICE
      }
      policy {
        setting:
          "This feature cannot be disabled by settings. However, this request "
          "is made only for signed-in users."
        chrome_policy {
          SigninAllowed {
            SigninAllowed: false
          }
        }
      })");
  MakeApiCall(CreateV2DeviceSyncRequestUrl(kShareGroupPrivateKeyPath), request,
              callback, error_callback, partial_traffic_annotation);
}

// TODO(https://crbug.com/953087): Populate the "sender" and "trigger" fields
// when method is used in codebase.
void CryptAuthClientImpl::BatchNotifyGroupDevices(
    const cryptauthv2::BatchNotifyGroupDevicesRequest& request,
    const BatchNotifyGroupDevicesCallback& callback,
    const ErrorCallback& error_callback) {
  net::PartialNetworkTrafficAnnotationTag partial_traffic_annotation =
      net::DefinePartialNetworkTrafficAnnotation(
          "cryptauth_v2_devicesync_batch_notify_group_devices",
          "oauth2_api_call_flow",
          R"(
      semantics {
        sender: "TBD"
        description:
          "The client sends a list of the user's devices that it wants to "
          "tickle via a GCM message."
        trigger: "TBD"
        data:
          "The list of device IDs to notify as well as a specification of the "
          "the CryptAuth service (Enrollment or DeviceSync) and feature "
          "relevant to the tickle."
        destination: GOOGLE_OWNED_SERVICE
      }
      policy {
        setting:
          "This feature cannot be disabled by settings. However, this request "
          "is made only for signed-in users."
        chrome_policy {
          SigninAllowed {
            SigninAllowed: false
          }
        }
      })");
  MakeApiCall(CreateV2DeviceSyncRequestUrl(kBatchNotifyGroupDevicesPath),
              request, callback, error_callback, partial_traffic_annotation);
}

// TODO(https://crbug.com/953087): Populate the "sender" and "trigger" fields
// when method is used in codebase.
void CryptAuthClientImpl::BatchGetFeatureStatuses(
    const cryptauthv2::BatchGetFeatureStatusesRequest& request,
    const BatchGetFeatureStatusesCallback& callback,
    const ErrorCallback& error_callback) {
  net::PartialNetworkTrafficAnnotationTag partial_traffic_annotation =
      net::DefinePartialNetworkTrafficAnnotation(
          "cryptauth_v2_devicesync_batch_get_feature_statuses",
          "oauth2_api_call_flow",
          R"(
      semantics {
        sender: "TBD"
        description:
          "The client queries CryptAuth for the state of features on the "
          "user's devices, for example, whether or not Magic Tether is enabled "
          "on any of the user's phones."
        trigger: "TBD"
        data: "The user device IDs and feature types to query."
        destination: GOOGLE_OWNED_SERVICE
      }
      policy {
        setting:
          "This feature cannot be disabled by settings. However, this request "
          "is made only for signed-in users."
        chrome_policy {
          SigninAllowed {
            SigninAllowed: false
          }
        }
      })");
  MakeApiCall(CreateV2DeviceSyncRequestUrl(kBatchGetFeatureStatusesPath),
              request, callback, error_callback, partial_traffic_annotation);
}

// TODO(https://crbug.com/953087): Populate the "sender" and "trigger" fields
// when method is used in codebase.
void CryptAuthClientImpl::BatchSetFeatureStatuses(
    const cryptauthv2::BatchSetFeatureStatusesRequest& request,
    const BatchSetFeatureStatusesCallback& callback,
    const ErrorCallback& error_callback) {
  net::PartialNetworkTrafficAnnotationTag partial_traffic_annotation =
      net::DefinePartialNetworkTrafficAnnotation(
          "cryptauth_v2_devicesync_batch_set_feature_statuses",
          "oauth2_api_call_flow",
          R"(
      semantics {
        sender: "TBD"
        description:
          "The client requests CryptAuth to set the state of various features "
          "for the user's devices."
        trigger: "TBD"
        data: "User device IDs and feature state specifications."
        destination: GOOGLE_OWNED_SERVICE
      }
      policy {
        setting:
          "This feature cannot be disabled by settings. However, this request "
          "is made only for signed-in users."
        chrome_policy {
          SigninAllowed {
            SigninAllowed: false
          }
        }
      })");
  MakeApiCall(CreateV2DeviceSyncRequestUrl(kBatchSetFeatureStatusesPath),
              request, callback, error_callback, partial_traffic_annotation);
}

std::string CryptAuthClientImpl::GetAccessTokenUsed() {
  return access_token_used_;
}

template <class RequestProto, class ResponseProto>
void CryptAuthClientImpl::MakeApiCall(
    const GURL& request_url,
    const RequestProto& request_proto,
    const base::Callback<void(const ResponseProto&)>& response_callback,
    const ErrorCallback& error_callback,
    const net::PartialNetworkTrafficAnnotationTag& partial_traffic_annotation) {
  if (has_call_started_) {
    PA_LOG(ERROR) << "CryptAuthClientImpl::MakeApiCall(): Tried to make an API "
                  << "call, but the client had already been used.";
    NOTREACHED();
    return;
  }
  has_call_started_ = true;

  api_call_flow_->SetPartialNetworkTrafficAnnotation(
      partial_traffic_annotation);

  std::string serialized_request;
  if (!request_proto.SerializeToString(&serialized_request)) {
    PA_LOG(ERROR) << "CryptAuthClientImpl::MakeApiCall(): Failure serializing "
                  << "request proto.";
    NOTREACHED();
    return;
  }

  request_url_ = request_url;
  error_callback_ = error_callback;

  OAuth2TokenService::ScopeSet scopes;
  scopes.insert(kCryptAuthOAuth2Scope);

  access_token_fetcher_ = std::make_unique<
      identity::PrimaryAccountAccessTokenFetcher>(
      "cryptauth_client", identity_manager_, scopes,
      base::BindOnce(&CryptAuthClientImpl::OnAccessTokenFetched<ResponseProto>,
                     weak_ptr_factory_.GetWeakPtr(), serialized_request,
                     response_callback),
      identity::PrimaryAccountAccessTokenFetcher::Mode::kWaitUntilAvailable);
}

template <class ResponseProto>
void CryptAuthClientImpl::OnAccessTokenFetched(
    const std::string& serialized_request,
    const base::Callback<void(const ResponseProto&)>& response_callback,
    GoogleServiceAuthError error,
    identity::AccessTokenInfo access_token_info) {
  access_token_fetcher_.reset();

  if (error.state() != GoogleServiceAuthError::NONE) {
    OnApiCallFailed(NetworkRequestError::kAuthenticationError);
    return;
  }
  access_token_used_ = access_token_info.token;

  api_call_flow_->Start(
      request_url_, url_loader_factory_, access_token_used_, serialized_request,
      base::Bind(&CryptAuthClientImpl::OnFlowSuccess<ResponseProto>,
                 weak_ptr_factory_.GetWeakPtr(), response_callback),
      base::Bind(&CryptAuthClientImpl::OnApiCallFailed,
                 weak_ptr_factory_.GetWeakPtr()));
}

template <class ResponseProto>
void CryptAuthClientImpl::OnFlowSuccess(
    const base::Callback<void(const ResponseProto&)>& result_callback,
    const std::string& serialized_response) {
  ResponseProto response;
  if (!response.ParseFromString(serialized_response)) {
    OnApiCallFailed(NetworkRequestError::kResponseMalformed);
    return;
  }
  result_callback.Run(response);
}

void CryptAuthClientImpl::OnApiCallFailed(NetworkRequestError error) {
  error_callback_.Run(error);
}

template <class RequestProto>
RequestProto CryptAuthClientImpl::RequestWithDeviceClassifierSet(
    const RequestProto& request) {
  RequestProto request_copy(request);
  request_copy.mutable_device_classifier()->CopyFrom(device_classifier_);

  return request_copy;
}

// CryptAuthClientFactoryImpl
CryptAuthClientFactoryImpl::CryptAuthClientFactoryImpl(
    identity::IdentityManager* identity_manager,
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    const cryptauth::DeviceClassifier& device_classifier)
    : identity_manager_(identity_manager),
      url_loader_factory_(std::move(url_loader_factory)),
      device_classifier_(device_classifier) {}

CryptAuthClientFactoryImpl::~CryptAuthClientFactoryImpl() {}

std::unique_ptr<CryptAuthClient> CryptAuthClientFactoryImpl::CreateInstance() {
  return std::make_unique<CryptAuthClientImpl>(
      base::WrapUnique(new CryptAuthApiCallFlow()), identity_manager_,
      url_loader_factory_, device_classifier_);
}

}  // namespace device_sync

}  // namespace chromeos