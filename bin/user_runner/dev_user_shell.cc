// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Implementation of a user shell for module development. It takes a
// root module URL and data for its Link as command line arguments,
// which can be set using the device_runner --user-shell-args flag.

#include <utility>

#include <memory>

#include "lib/app/cpp/application_context.h"
#include "lib/app/cpp/connect.h"
#include "lib/app/fidl/service_provider.fidl.h"
#include "lib/app_driver/cpp/app_driver.h"
#include "lib/fidl/cpp/bindings/binding.h"
#include "lib/fidl/cpp/bindings/binding_set.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/command_line.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/macros.h"
#include "lib/lifecycle/fidl/lifecycle.fidl.h"
#include "lib/story/fidl/link.fidl.h"
#include "lib/suggestion/fidl/suggestion_provider.fidl.h"
#include "lib/ui/views/fidl/view_manager.fidl.h"
#include "lib/ui/views/fidl/view_provider.fidl.h"
#include "lib/user/fidl/focus.fidl.h"
#include "lib/user/fidl/user_shell.fidl.h"
#include "peridot/lib/fidl/single_service_app.h"
#include "peridot/lib/fidl/view_host.h"

namespace {

class Settings {
 public:
  explicit Settings(const fxl::CommandLine& command_line) {
    root_module =
        command_line.GetOptionValueWithDefault("root_module", "example_recipe");
    root_link = command_line.GetOptionValueWithDefault("root_link", "");
    story_id = command_line.GetOptionValueWithDefault("story_id", "");
  }

  std::string root_module;
  std::string root_link;
  std::string story_id;
};

class DevUserShellApp : modular::StoryWatcher,
                        maxwell::SuggestionListener,
                        public modular::SingleServiceApp<modular::UserShell> {
 public:
  explicit DevUserShellApp(app::ApplicationContext* const application_context,
                           Settings settings)
      : SingleServiceApp(application_context),
        settings_(std::move(settings)),
        story_watcher_binding_(this) {}

  ~DevUserShellApp() override = default;

 private:
  // |SingleServiceApp|
  void CreateView(
      fidl::InterfaceRequest<mozart::ViewOwner> view_owner_request,
      fidl::InterfaceRequest<app::ServiceProvider> /*services*/) override {
    view_owner_request_ = std::move(view_owner_request);
    Connect();
  }

  // |UserShell|
  void Initialize(fidl::InterfaceHandle<modular::UserShellContext>
                      user_shell_context) override {
    user_shell_context_.Bind(std::move(user_shell_context));
    user_shell_context_->GetStoryProvider(story_provider_.NewRequest());
    user_shell_context_->GetSuggestionProvider(
        suggestion_provider_.NewRequest());
    user_shell_context_->GetFocusController(focus_controller_.NewRequest());
    user_shell_context_->GetVisibleStoriesController(
        visible_stories_controller_.NewRequest());

    suggestion_provider_->SubscribeToInterruptions(
        suggestion_listener_bindings_.AddBinding(this));
    suggestion_provider_->SubscribeToNext(
        suggestion_listener_bindings_.AddBinding(this), 3);

    Connect();
  }

  void Connect() {
    if (!view_owner_request_ || !story_provider_) {
      // Not yet ready, wait for the other of CreateView() and
      // Initialize() to be called.
      return;
    }

    FXL_LOG(INFO) << "DevUserShell START " << settings_.root_module << " "
                  << settings_.root_link;

    view_ = std::make_unique<modular::ViewHost>(
        application_context()
            ->ConnectToEnvironmentService<mozart::ViewManager>(),
        std::move(view_owner_request_));

    if (settings_.story_id.empty()) {
      story_provider_->CreateStory(
          settings_.root_module,
          [this](const fidl::String& story_id) { StartStoryById(story_id); });
    } else {
      StartStoryById(settings_.story_id);
    }
  }

  void StartStoryById(const fidl::String& story_id) {
    story_provider_->GetController(story_id, story_controller_.NewRequest());
    story_controller_.set_connection_error_handler([this, story_id] {
      FXL_LOG(ERROR) << "Story controller for story " << story_id
                     << " died. Does this story exist?";
    });

    story_controller_->Watch(story_watcher_binding_.NewBinding());

    FXL_LOG(INFO) << "DevUserShell Starting story with id: " << story_id;
    fidl::InterfaceHandle<mozart::ViewOwner> root_module_view;
    story_controller_->Start(root_module_view.NewRequest());
    view_->ConnectView(std::move(root_module_view));
    focus_controller_->Set(story_id);
    auto visible_stories = fidl::Array<fidl::String>::New(0);
    visible_stories.push_back(story_id);
    visible_stories_controller_->Set(std::move(visible_stories));

    if (!settings_.root_link.empty()) {
      modular::LinkPtr root;
      story_controller_->GetLink(nullptr, "root", root.NewRequest());
      root->UpdateObject(nullptr, settings_.root_link);
    }
  }

  // |StoryWatcher|
  void OnStateChange(modular::StoryState state) override {
    if (state != modular::StoryState::DONE) {
      return;
    }

    FXL_LOG(INFO) << "DevUserShell DONE";
    story_controller_->Stop([this] {
      FXL_LOG(INFO) << "DevUserShell STOP";
      story_watcher_binding_.Close();
      story_controller_.reset();
      user_shell_context_->Logout();
    });
  }

  // |StoryWatcher|
  void OnModuleAdded(modular::ModuleDataPtr /*module_data*/) override {}

  // |SuggestionListener|
  void OnAdd(fidl::Array<maxwell::SuggestionPtr> suggestions) override {
    FXL_VLOG(4) << "DevUserShell/SuggestionListener::OnAdd()";
    for (auto& suggestion : suggestions) {
      FXL_LOG(INFO) << "  " << suggestion->uuid << " "
                    << suggestion->display->headline;
    }
  }

  // |SuggestionListener|
  void OnRemove(const fidl::String& suggestion_id) override {
    FXL_VLOG(4) << "DevUserShell/SuggestionListener::OnRemove() "
                << suggestion_id;
  }

  // |SuggestionListener|
  void OnRemoveAll() override {
    FXL_VLOG(4) << "DevUserShell/SuggestionListener::OnRemoveAll()";
  }

  // |SuggestionListener|
  void OnProcessingChange(bool processing) override {
    FXL_VLOG(4) << "DevUserShell/SuggestionListener::OnProcessingChange("
                << processing << ")";
  }

  const Settings settings_;

  fidl::InterfaceRequest<mozart::ViewOwner> view_owner_request_;
  std::unique_ptr<modular::ViewHost> view_;

  modular::UserShellContextPtr user_shell_context_;
  modular::StoryProviderPtr story_provider_;
  modular::StoryControllerPtr story_controller_;
  modular::FocusControllerPtr focus_controller_;
  modular::VisibleStoriesControllerPtr visible_stories_controller_;

  fidl::Binding<modular::StoryWatcher> story_watcher_binding_;

  maxwell::SuggestionProviderPtr suggestion_provider_;
  fidl::BindingSet<maxwell::SuggestionListener> suggestion_listener_bindings_;

  FXL_DISALLOW_COPY_AND_ASSIGN(DevUserShellApp);
};

}  // namespace

int main(int argc, const char** argv) {
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  Settings settings(command_line);

  fsl::MessageLoop loop;

  auto app_context = app::ApplicationContext::CreateFromStartupInfo();
  modular::AppDriver<DevUserShellApp> driver(
      app_context->outgoing_services(),
      std::make_unique<DevUserShellApp>(app_context.get(), std::move(settings)),
      [&loop] { loop.QuitNow(); });

  loop.Run();
  return 0;
}
