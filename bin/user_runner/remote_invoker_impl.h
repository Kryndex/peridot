// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_USER_RUNNER_REMOTE_INVOKER_IMPL_H_
#define PERIDOT_BIN_USER_RUNNER_REMOTE_INVOKER_IMPL_H_

#include "lib/async/cpp/operation.h"
#include "lib/fidl/cpp/bindings/binding_set.h"
#include "lib/fidl/cpp/bindings/interface_request.h"
#include "lib/ledger/fidl/ledger.fidl.h"
#include "lib/remote/fidl/remote_invoker.fidl.h"

namespace modular {

// See services/user/remote_invoker.fidl for details.
//
// Provides interface for calls to remote devices
class RemoteInvokerImpl : RemoteInvoker {
 public:
  explicit RemoteInvokerImpl(ledger::Ledger* ledger);
  ~RemoteInvokerImpl() override;

  void Connect(fidl::InterfaceRequest<RemoteInvoker> request);

 private:
  // |RemoteInvoker|
  void StartOnDevice(const fidl::String& device_id,
                     const fidl::String& story_id,
                     const StartOnDeviceCallback& callback) override;

  fidl::BindingSet<RemoteInvoker> bindings_;
  OperationQueue operation_queue_;
  ledger::Ledger* const ledger_;

  // Operations implemented here.
  class StartOnDeviceCall;

  FXL_DISALLOW_COPY_AND_ASSIGN(RemoteInvokerImpl);
};

}  // namespace modular

#endif  // PERIDOT_BIN_USER_RUNNER_REMOTE_INVOKER_IMPL_H_
