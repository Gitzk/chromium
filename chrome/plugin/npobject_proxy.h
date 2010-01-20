// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// A proxy for NPObject that sends all calls to the object to an NPObjectStub
// running in a different process.

#ifndef CHROME_PLUGIN_NPOBJECT_PROXY_H_
#define CHROME_PLUGIN_NPOBJECT_PROXY_H_

#include "app/gfx/native_widget_types.h"
#include "base/ref_counted.h"
#include "chrome/plugin/npobject_base.h"
#include "googleurl/src/gurl.h"
#include "ipc/ipc_channel.h"
#include "third_party/npapi/bindings/npruntime.h"

class PluginChannelBase;
struct NPObject;

// When running a plugin in a different process from the renderer, we need to
// proxy calls to NPObjects across process boundaries.  This happens both ways,
// as a plugin can get an NPObject for the window, and a page can get an
// NPObject for the plugin.  In the process that interacts with the NPobject we
// give it an NPObjectProxy instead.  All calls to it are sent across an IPC
// channel (specifically, a PluginChannelBase).  The NPObjectStub on the other
// side translates the IPC messages into calls to the actual NPObject, and
// returns the marshalled result.
class NPObjectProxy : public IPC::Channel::Listener,
                      public IPC::Message::Sender,
                      public NPObjectBase {
 public:
  ~NPObjectProxy();

  static NPObject* Create(PluginChannelBase* channel,
                          int route_id,
                          gfx::NativeViewId containing_window,
                          const GURL& page_url);

  // IPC::Message::Sender implementation:
  bool Send(IPC::Message* msg);
  int route_id() { return route_id_; }
  PluginChannelBase* channel() { return channel_; }

  // The next 9 functions are called on NPObjects from the plugin and browser.
  static bool NPHasMethod(NPObject *obj,
                          NPIdentifier name);
  static bool NPInvoke(NPObject *obj,
                       NPIdentifier name,
                       const NPVariant *args,
                       uint32_t arg_count,
                       NPVariant *result);
  static bool NPInvokeDefault(NPObject *npobj,
                              const NPVariant *args,
                              uint32_t arg_count,
                              NPVariant *result);
  static bool NPHasProperty(NPObject *obj,
                            NPIdentifier name);
  static bool NPGetProperty(NPObject *obj,
                            NPIdentifier name,
                            NPVariant *result);
  static bool NPSetProperty(NPObject *obj,
                            NPIdentifier name,
                            const NPVariant *value);
  static bool NPRemoveProperty(NPObject *obj,
                               NPIdentifier name);
  static bool NPNEnumerate(NPObject *obj,
                           NPIdentifier **value,
                           uint32_t *count);
  static bool NPNConstruct(NPObject *npobj,
                           const NPVariant *args,
                           uint32_t arg_count,
                           NPVariant *result);

  // The next two functions are only called on NPObjects from the browser.
  static bool NPNEvaluate(NPP npp,
                          NPObject *obj,
                          NPString *script,
                          NPVariant *result);

  static bool NPInvokePrivate(NPP npp,
                              NPObject *obj,
                              bool is_default,
                              NPIdentifier name,
                              const NPVariant *args,
                              uint32_t arg_count,
                              NPVariant *result);

  static NPObjectProxy* GetProxy(NPObject* object);
  static const NPClass* npclass() { return &npclass_proxy_; }

  // NPObjectBase implementation.
  virtual NPObject* GetUnderlyingNPObject() {
    return NULL;
  }

  IPC::Channel::Listener* GetChannelListener() {
    return static_cast<IPC::Channel::Listener*>(this);
  }

 private:
  NPObjectProxy(PluginChannelBase* channel,
                int route_id,
                gfx::NativeViewId containing_window,
                const GURL& page_url);

  // IPC::Channel::Listener implementation:
  void OnMessageReceived(const IPC::Message& msg);
  void OnChannelError();

  static NPObject* NPAllocate(NPP, NPClass*);
  static void NPDeallocate(NPObject* npObj);

  // This function is only caled on NPObjects from the plugin.
  static void NPPInvalidate(NPObject *obj);
  static NPClass npclass_proxy_;

  scoped_refptr<PluginChannelBase> channel_;
  int route_id_;
  gfx::NativeViewId containing_window_;

  // The url of the main frame hosting the plugin.
  GURL page_url_;
};

#endif  // CHROME_PLUGIN_NPOBJECT_PROXY_H_
