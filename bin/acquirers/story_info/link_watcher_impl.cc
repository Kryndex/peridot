// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/acquirers/story_info/link_watcher_impl.h"

#include <sstream>

#include "garnet/public/lib/fxl/functional/make_copyable.h"
#include "lib/context/cpp/context_metadata_builder.h"
#include "lib/context/cpp/formatting.h"
#include "lib/entity/cpp/json.h"
#include "peridot/bin/acquirers/story_info/story_watcher_impl.h"
#include "peridot/lib/fidl/json_xdr.h"
#include "peridot/lib/ledger_client/storage.h"
#include "peridot/lib/rapidjson/rapidjson.h"

namespace maxwell {

namespace {

constexpr char kContextProperty[] = "@context";
constexpr char kSourceProperty[] = "@source";

struct Context {
  fidl::String topic;
};

struct Source {
  fidl::String story_id;
  fidl::Array<fidl::String> module_path;
  fidl::String link_name;
};

void XdrContext(modular::XdrContext* const xdr, Context* const data) {
  xdr->Field("topic", &data->topic);
}

void XdrSource(modular::XdrContext* const xdr, Source* const data) {
  xdr->Field("story_id", &data->story_id);
  xdr->Field("module_path", &data->module_path);
  xdr->Field("link_name", &data->link_name);
}

std::string MakeLinkTopic(const fidl::String& base_topic) {
  std::stringstream s;
  s << "link/" << base_topic;
  return s.str();
}

}  // namespace

LinkWatcherImpl::LinkWatcherImpl(
    StoryWatcherImpl* const owner,
    modular::StoryController* const story_controller,
    const std::string& story_id,
    ContextValueWriter* const story_value,
    const modular::LinkPathPtr& link_path)
    : owner_(owner),
      story_controller_(story_controller),
      story_id_(story_id),
      link_path_(link_path->Clone()),
      link_watcher_binding_(this) {
  modular::LinkPtr link;
  story_controller_->GetLink(link_path_->module_path.Clone(),
                             link_path_->link_name, link.NewRequest());

  story_value->CreateChildValue(link_node_writer_.NewRequest(),
                                ContextValueType::LINK);
  link_node_writer_->Set(
      nullptr, ContextMetadataBuilder()
                   .SetLinkPath(link_path_->module_path, link_path_->link_name)
                   .Build());

  link->Watch(link_watcher_binding_.NewBinding());

  // If the link becomes inactive, we stop watching it. It might still receive
  // updates from other devices, but nothing can tell us as it isn't kept in
  // memory on the current device.
  //
  // The Link itself is not kept here, because otherwise it never becomes
  // inactive (i.e. loses all its Link connections).
  link_watcher_binding_.set_connection_error_handler(
      [this] { owner_->DropLink(MakeLinkKey(link_path_)); });
}

LinkWatcherImpl::~LinkWatcherImpl() = default;

void LinkWatcherImpl::Notify(const fidl::String& json) {
  ProcessNewValue(json);
  // TODO(thatguy): Deprecate this method once every Link is a "context link".
  MaybeProcessContextLink(json);
}

void LinkWatcherImpl::ProcessNewValue(const fidl::String& value) {
  // We are looking for the following |value| structures:
  //
  // 1) |value| contains a JSON-style entity:
  //   { "@type": ..., ... }
  // 2) |value| contains a JSON-encoded Entity reference
  // (EntityReferenceFromJson() will return true).
  // 3) |value| is a JSON dictionary, and any of the members satisfies either
  // (1) or (2).
  //
  // TODO(thatguy): Moving to Bundles allows us to ignore (3), and using
  // Entities everywhere allows us to ignore (1).
  modular::JsonDoc doc;
  doc.Parse(value);
  FXL_CHECK(!doc.HasParseError());

  if (!doc.IsObject()) {
    return;
  }

  // (1) & (2)
  std::vector<std::string> types;
  std::string ref;
  if (modular::ExtractEntityTypesFromJson(doc, &types) ||
      modular::EntityReferenceFromJson(doc, &ref)) {
    // There is only *one* Entity in this Link.
    entity_node_writers_.clear();
    if (!single_entity_node_writer_.is_bound()) {
      link_node_writer_->CreateChildValue(
          single_entity_node_writer_.NewRequest(), ContextValueType::ENTITY);
    }
    single_entity_node_writer_->Set(value, nullptr);
    return;
  } else {
    // There is not simply a *single* Entity in this Link. There may be
    // multiple Entities (see below).
    single_entity_node_writer_.reset();
  }

  // (3)
  std::set<std::string> keys_that_have_entities;
  for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
    if (modular::ExtractEntityTypesFromJson(it->value, &types) ||
        modular::EntityReferenceFromJson(it->value, &ref)) {
      keys_that_have_entities.insert(it->name.GetString());

      auto value_it = entity_node_writers_.find(it->name.GetString());
      if (value_it == entity_node_writers_.end()) {
        ContextValueWriterPtr writer;
        link_node_writer_->CreateChildValue(writer.NewRequest(),
                                            ContextValueType::ENTITY);
        value_it = entity_node_writers_
                       .emplace(it->name.GetString(), std::move(writer))
                       .first;
      }
      value_it->second->Set(modular::JsonValueToString(it->value), nullptr);
    }
  }

  // Clean up any old entries in |entity_node_writers_|.
  std::set<std::string> to_remove;
  for (const auto& entry : entity_node_writers_) {
    if (keys_that_have_entities.count(entry.first) == 0) {
      to_remove.insert(entry.first);
    }
  }
  for (const auto& key : to_remove) {
    entity_node_writers_.erase(key);
  }
}

void LinkWatcherImpl::MaybeProcessContextLink(const fidl::String& value) {
  modular::JsonDoc doc;
  doc.Parse(value);
  FXL_CHECK(!doc.HasParseError());

  if (!doc.IsObject()) {
    return;
  }

  auto i = doc.FindMember(kContextProperty);
  if (i == doc.MemberEnd()) {
    return;
  }

  modular::JsonDoc context_doc;
  context_doc.CopyFrom(i->value, context_doc.GetAllocator());
  doc.RemoveMember(i);

  if (!context_doc.IsObject()) {
    return;
  }

  Context context;
  if (!modular::XdrRead(&context_doc, &context, XdrContext)) {
    return;
  }

  Source source;
  source.story_id = story_id_;
  source.module_path = link_path_->module_path.Clone();
  source.link_name = link_path_->link_name;

  modular::JsonDoc source_doc;
  modular::XdrWrite(&source_doc, &source, XdrSource);
  doc.AddMember(kSourceProperty, source_doc, doc.GetAllocator());

  std::string json = modular::JsonValueToString(doc);

  auto it = topic_node_writers_.find(context.topic);
  if (it == topic_node_writers_.end()) {
    ContextValueWriterPtr topic_node_writer;
    link_node_writer_->CreateChildValue(topic_node_writer.NewRequest(),
                                        ContextValueType::ENTITY);
    it =
        topic_node_writers_.emplace(context.topic, std::move(topic_node_writer))
            .first;
  }
  it->second->Set(json, ContextMetadataBuilder()
                            .SetEntityTopic(MakeLinkTopic(context.topic))
                            .Build());
}

}  // namespace maxwell
