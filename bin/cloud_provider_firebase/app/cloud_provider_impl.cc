// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/cloud_provider_firebase/app/cloud_provider_impl.h"

#include <utility>

#include "lib/fxl/logging.h"
#include "peridot/bin/cloud_provider_firebase/app/convert_status.h"
#include "peridot/bin/cloud_provider_firebase/device_set/cloud_device_set_impl.h"
#include "peridot/bin/cloud_provider_firebase/gcs/cloud_storage_impl.h"
#include "peridot/bin/cloud_provider_firebase/page_handler/impl/page_cloud_handler_impl.h"
#include "peridot/bin/cloud_provider_firebase/page_handler/impl/paths.h"
#include "peridot/bin/ledger/convert/convert.h"

namespace cloud_provider_firebase {

CloudProviderImpl::CloudProviderImpl(
    fxl::RefPtr<fxl::TaskRunner> main_runner,
    ledger::NetworkService* network_service,
    std::string user_id,
    ConfigPtr config,
    std::unique_ptr<auth_provider::AuthProvider> auth_provider,
    fidl::InterfaceRequest<cloud_provider::CloudProvider> request)
    : main_runner_(std::move(main_runner)),
      network_service_(network_service),
      user_id_(std::move(user_id)),
      server_id_(config->server_id),
      auth_provider_(std::move(auth_provider)),
      user_firebase_(network_service_,
                     server_id_,
                     GetFirebasePathForUser(user_id_)),
      binding_(this, std::move(request)) {
  // The class shuts down when the client connection is disconnected.
  binding_.set_connection_error_handler([this] {
    if (on_empty_) {
      on_empty_();
    }
  });
  // The class also shuts down when the auth provider is disconnected.
  auth_provider_->set_connection_error_handler([this] {
    FXL_LOG(ERROR) << "Lost connection to the token provider, "
                   << "shutting down the cloud provider.";
    if (on_empty_) {
      on_empty_();
    }
  });
}

CloudProviderImpl::~CloudProviderImpl() {}

void CloudProviderImpl::GetDeviceSet(
    fidl::InterfaceRequest<cloud_provider::DeviceSet> device_set,
    const GetDeviceSetCallback& callback) {
  // TODO(ppi): Once the switch to standalone cloud provider is done, change
  // CloudDeviceSet to take a raw ptr and re-use |user_firebase_| here.
  auto user_firebase = std::make_unique<firebase::FirebaseImpl>(
      network_service_, server_id_, GetFirebasePathForUser(user_id_));
  auto cloud_device_set =
      std::make_unique<CloudDeviceSetImpl>(std::move(user_firebase));
  device_sets_.emplace(auth_provider_.get(), std::move(cloud_device_set),
                       std::move(device_set));
  callback(cloud_provider::Status::OK);
}

void CloudProviderImpl::GetPageCloud(
    fidl::Array<uint8_t> app_id,
    fidl::Array<uint8_t> page_id,
    fidl::InterfaceRequest<cloud_provider::PageCloud> page_cloud,
    const GetPageCloudCallback& callback) {
  std::string app_id_str = convert::ToString(app_id);
  std::string page_id_str = convert::ToString(page_id);

  std::string app_firebase_path = GetFirebasePathForApp(user_id_, app_id_str);
  auto firebase = std::make_unique<firebase::FirebaseImpl>(
      network_service_, server_id_,
      GetFirebasePathForPage(app_firebase_path, page_id_str));

  std::string app_gcs_prefix = GetGcsPrefixForApp(user_id_, app_id_str);
  auto cloud_storage = std::make_unique<gcs::CloudStorageImpl>(
      main_runner_, network_service_, server_id_,
      GetGcsPrefixForPage(app_gcs_prefix, page_id_str));

  auto handler =
      std::make_unique<cloud_provider_firebase::PageCloudHandlerImpl>(
          firebase.get(), cloud_storage.get());
  page_clouds_.emplace(auth_provider_.get(), std::move(firebase),
                       std::move(cloud_storage), std::move(handler),
                       std::move(page_cloud));
  callback(cloud_provider::Status::OK);
}

void CloudProviderImpl::EraseAllData(const EraseAllDataCallback& callback) {
  auto request = auth_provider_->GetFirebaseToken(
      [this, callback](auth_provider::AuthStatus auth_status,
                       std::string auth_token) mutable {
        if (auth_status != auth_provider::AuthStatus::OK) {
          callback(cloud_provider::Status::AUTH_ERROR);
          return;
        }
        std::vector<std::string> query_params;
        if (!auth_token.empty()) {
          query_params = {"auth=" + auth_token};
        }

        user_firebase_.Delete(
            "", query_params,
            [callback = std::move(callback)](firebase::Status status) {
              callback(ConvertInternalStatus(ConvertFirebaseStatus(status)));
            });

      });
  auth_token_requests_.emplace(request);
}

}  // namespace cloud_provider_firebase