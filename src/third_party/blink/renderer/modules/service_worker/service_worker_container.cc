/*
 * Copyright (C) 2013 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "third_party/blink/renderer/modules/service_worker/service_worker_container.h"

#include <memory>
#include <utility>

#include "base/macros.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_error_type.mojom-blink.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/renderer/bindings/core/v8/callback_promise_adapter.h"
#include "third_party/blink/renderer/bindings/core/v8/script_promise.h"
#include "third_party/blink/renderer/bindings/core/v8/script_promise_resolver.h"
#include "third_party/blink/renderer/bindings/core/v8/serialization/serialized_script_value.h"
#include "third_party/blink/renderer/bindings/core/v8/serialization/serialized_script_value_factory.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/dom_exception.h"
#include "third_party/blink/renderer/core/dom/events/native_event_listener.h"
#include "third_party/blink/renderer/core/events/message_event.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/frame/csp/content_security_policy.h"
#include "third_party/blink/renderer/core/frame/deprecation.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_client.h"
#include "third_party/blink/renderer/core/frame/use_counter.h"
#include "third_party/blink/renderer/core/inspector/console_message.h"
#include "third_party/blink/renderer/core/messaging/blink_transferable_message.h"
#include "third_party/blink/renderer/core/messaging/message_port.h"
#include "third_party/blink/renderer/modules/event_target_modules.h"
#include "third_party/blink/renderer/modules/service_worker/service_worker.h"
#include "third_party/blink/renderer/modules/service_worker/service_worker_error.h"
#include "third_party/blink/renderer/modules/service_worker/service_worker_registration.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"
#include "third_party/blink/renderer/platform/bindings/v8_throw_exception.h"
#include "third_party/blink/renderer/platform/weborigin/scheme_registry.h"
#include "third_party/blink/renderer/platform/weborigin/security_violation_reporting_policy.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

namespace blink {

namespace {

bool HasFiredDomContentLoaded(const Document& document) {
  return !document.GetTiming().DomContentLoadedEventStart().is_null();
}

mojom::ServiceWorkerUpdateViaCache ParseUpdateViaCache(const String& value) {
  if (value == "imports")
    return mojom::ServiceWorkerUpdateViaCache::kImports;
  if (value == "all")
    return mojom::ServiceWorkerUpdateViaCache::kAll;
  if (value == "none")
    return mojom::ServiceWorkerUpdateViaCache::kNone;
  // Default value.
  return mojom::ServiceWorkerUpdateViaCache::kImports;
}

mojom::ScriptType ParseScriptType(const String& type) {
  if (type == "classic")
    return mojom::ScriptType::kClassic;
  if (type == "module")
    return mojom::ScriptType::kModule;
  NOTREACHED() << "Invalid type: " << type;
  return mojom::ScriptType::kClassic;
}

class GetRegistrationCallback : public WebServiceWorkerProvider::
                                    WebServiceWorkerGetRegistrationCallbacks {
 public:
  explicit GetRegistrationCallback(ScriptPromiseResolver* resolver)
      : resolver_(resolver) {}
  ~GetRegistrationCallback() override = default;

  void OnSuccess(WebServiceWorkerRegistrationObjectInfo info) override {
    if (!resolver_->GetExecutionContext() ||
        resolver_->GetExecutionContext()->IsContextDestroyed())
      return;
    if (info.registration_id ==
        mojom::blink::kInvalidServiceWorkerRegistrationId) {
      // Resolve the promise with undefined.
      resolver_->Resolve();
      return;
    }
    resolver_->Resolve(
        ServiceWorkerRegistration::Take(resolver_, std::move(info)));
  }

  void OnError(const WebServiceWorkerError& error) override {
    if (!resolver_->GetExecutionContext() ||
        resolver_->GetExecutionContext()->IsContextDestroyed())
      return;
    resolver_->Reject(ServiceWorkerError::Take(resolver_.Get(), error));
  }

 private:
  Persistent<ScriptPromiseResolver> resolver_;
  DISALLOW_COPY_AND_ASSIGN(GetRegistrationCallback);
};

}  // namespace

class ServiceWorkerContainer::DomContentLoadedListener final
    : public NativeEventListener {
 public:
  void Invoke(ExecutionContext* execution_context, Event* event) override {
    DCHECK_EQ(event->type(), "DOMContentLoaded");

    Document& document = *To<Document>(execution_context);
    DCHECK(HasFiredDomContentLoaded(document));

    auto* container =
        Supplement<Document>::From<ServiceWorkerContainer>(document);
    if (!container) {
      // There is no container for some reason, which means there's no message
      // queue to start. Just abort.
      return;
    }

    container->EnableClientMessageQueue();
  }
};

const char ServiceWorkerContainer::kSupplementName[] = "ServiceWorkerContainer";

ServiceWorkerContainer* ServiceWorkerContainer::From(Document* document) {
  if (!document)
    return nullptr;

  ServiceWorkerContainer* container =
      Supplement<Document>::From<ServiceWorkerContainer>(document);
  if (!container) {
    // TODO(leonhsl): Figure out whether it's really necessary to create an
    // instance when there's no frame or frame client for |document|.
    container = MakeGarbageCollected<ServiceWorkerContainer>(document);
    Supplement<Document>::ProvideTo(*document, container);
    if (document->GetFrame() && document->GetFrame()->Client()) {
      std::unique_ptr<WebServiceWorkerProvider> provider =
          document->GetFrame()->Client()->CreateServiceWorkerProvider();
      if (provider) {
        provider->SetClient(container);
        container->provider_ = std::move(provider);
      }
    }
  }
  return container;
}

ServiceWorkerContainer* ServiceWorkerContainer::CreateForTesting(
    Document* document,
    std::unique_ptr<WebServiceWorkerProvider> provider) {
  ServiceWorkerContainer* container =
      MakeGarbageCollected<ServiceWorkerContainer>(document);
  container->provider_ = std::move(provider);
  return container;
}

ServiceWorkerContainer::~ServiceWorkerContainer() {
  DCHECK(!provider_);
}

void ServiceWorkerContainer::ContextDestroyed(ExecutionContext*) {
  if (provider_) {
    provider_->SetClient(nullptr);
    provider_ = nullptr;
  }
  controller_ = nullptr;
}

void ServiceWorkerContainer::Trace(blink::Visitor* visitor) {
  visitor->Trace(controller_);
  visitor->Trace(ready_);
  visitor->Trace(dom_content_loaded_observer_);
  visitor->Trace(service_worker_registration_objects_);
  visitor->Trace(service_worker_objects_);
  EventTargetWithInlineData::Trace(visitor);
  Supplement<Document>::Trace(visitor);
  ContextLifecycleObserver::Trace(visitor);
}

ScriptPromise ServiceWorkerContainer::registerServiceWorker(
    ScriptState* script_state,
    const String& url,
    const RegistrationOptions* options) {
  auto* resolver = MakeGarbageCollected<ScriptPromiseResolver>(script_state);
  ScriptPromise promise = resolver->Promise();

  // TODO(asamidoi): Remove this check after module loading for
  // ServiceWorker is enabled by default (https://crbug.com/824647).
  if (options->type() == "module" &&
      !RuntimeEnabledFeatures::ModuleServiceWorkerEnabled()) {
    resolver->Reject(DOMException::Create(
        DOMExceptionCode::kNotSupportedError,
        "type 'module' in RegistrationOptions is not implemented yet."
        "See https://crbug.com/824647 for details."));
    return promise;
  }

  auto callbacks = std::make_unique<CallbackPromiseAdapter<
      ServiceWorkerRegistration, ServiceWorkerErrorForUpdate>>(resolver);

  ExecutionContext* execution_context = ExecutionContext::From(script_state);

  // The IDL definition is expected to restrict service worker to secure
  // contexts.
  CHECK(execution_context->IsSecureContext());

  scoped_refptr<const SecurityOrigin> document_origin =
      execution_context->GetSecurityOrigin();
  KURL page_url = KURL(NullURL(), document_origin->ToString());
  if (!SchemeRegistry::ShouldTreatURLSchemeAsAllowingServiceWorkers(
          page_url.Protocol())) {
    callbacks->OnError(WebServiceWorkerError(
        mojom::blink::ServiceWorkerErrorType::kType,
        String("Failed to register a ServiceWorker: The URL protocol of the "
               "current origin ('" +
               document_origin->ToString() + "') is not supported.")));
    return promise;
  }

  KURL script_url = execution_context->CompleteURL(url);
  script_url.RemoveFragmentIdentifier();

  if (!SchemeRegistry::ShouldTreatURLSchemeAsAllowingServiceWorkers(
          script_url.Protocol())) {
    callbacks->OnError(WebServiceWorkerError(
        mojom::blink::ServiceWorkerErrorType::kType,
        String("Failed to register a ServiceWorker: The URL protocol of the "
               "script ('" +
               script_url.GetString() + "') is not supported.")));
    return promise;
  }

  if (!document_origin->CanRequest(script_url)) {
    scoped_refptr<const SecurityOrigin> script_origin =
        SecurityOrigin::Create(script_url);
    callbacks->OnError(
        WebServiceWorkerError(mojom::blink::ServiceWorkerErrorType::kSecurity,
                              String("Failed to register a ServiceWorker: The "
                                     "origin of the provided scriptURL ('" +
                                     script_origin->ToString() +
                                     "') does not match the current origin ('" +
                                     document_origin->ToString() + "').")));
    return promise;
  }

  KURL scope_url;
  if (options->scope().IsNull())
    scope_url = KURL(script_url, "./");
  else
    scope_url = execution_context->CompleteURL(options->scope());
  scope_url.RemoveFragmentIdentifier();

  if (!SchemeRegistry::ShouldTreatURLSchemeAsAllowingServiceWorkers(
          scope_url.Protocol())) {
    callbacks->OnError(WebServiceWorkerError(
        mojom::blink::ServiceWorkerErrorType::kType,
        String("Failed to register a ServiceWorker: The URL protocol of the "
               "scope ('" +
               scope_url.GetString() + "') is not supported.")));
    return promise;
  }

  if (!document_origin->CanRequest(scope_url)) {
    scoped_refptr<const SecurityOrigin> scope_origin =
        SecurityOrigin::Create(scope_url);
    callbacks->OnError(
        WebServiceWorkerError(mojom::blink::ServiceWorkerErrorType::kSecurity,
                              String("Failed to register a ServiceWorker: The "
                                     "origin of the provided scope ('" +
                                     scope_origin->ToString() +
                                     "') does not match the current origin ('" +
                                     document_origin->ToString() + "').")));
    return promise;
  }

  if (!provider_) {
    resolver->Reject(DOMException::Create(DOMExceptionCode::kInvalidStateError,
                                          "Failed to register a ServiceWorker: "
                                          "The document is in an invalid "
                                          "state."));
    return promise;
  }
  WebString web_error_message;
  if (!provider_->ValidateScopeAndScriptURL(scope_url, script_url,
                                            &web_error_message)) {
    callbacks->OnError(WebServiceWorkerError(
        mojom::blink::ServiceWorkerErrorType::kType,
        WebString::FromUTF8("Failed to register a ServiceWorker: " +
                            web_error_message.Utf8())));
    return promise;
  }

  ContentSecurityPolicy* csp = execution_context->GetContentSecurityPolicy();
  if (csp) {
    if (!csp->AllowRequestWithoutIntegrity(
            mojom::RequestContextType::SERVICE_WORKER, script_url) ||
        !csp->AllowWorkerContextFromSource(script_url)) {
      callbacks->OnError(WebServiceWorkerError(
          mojom::blink::ServiceWorkerErrorType::kSecurity,
          String(
              "Failed to register a ServiceWorker: The provided scriptURL ('" +
              script_url.GetString() +
              "') violates the Content Security Policy.")));
      return promise;
    }
  }

  mojom::ServiceWorkerUpdateViaCache update_via_cache =
      ParseUpdateViaCache(options->updateViaCache());
  mojom::ScriptType type = ParseScriptType(options->type());

  provider_->RegisterServiceWorker(scope_url, script_url, type,
                                   update_via_cache, std::move(callbacks));
  return promise;
}

ScriptPromise ServiceWorkerContainer::getRegistration(
    ScriptState* script_state,
    const String& document_url) {
  auto* resolver = MakeGarbageCollected<ScriptPromiseResolver>(script_state);
  ScriptPromise promise = resolver->Promise();

  ExecutionContext* execution_context = ExecutionContext::From(script_state);

  // The IDL definition is expected to restrict service worker to secure
  // contexts.
  CHECK(execution_context->IsSecureContext());

  scoped_refptr<const SecurityOrigin> document_origin =
      execution_context->GetSecurityOrigin();
  KURL page_url = KURL(NullURL(), document_origin->ToString());
  if (!SchemeRegistry::ShouldTreatURLSchemeAsAllowingServiceWorkers(
          page_url.Protocol())) {
    resolver->Reject(DOMException::Create(
        DOMExceptionCode::kSecurityError,
        "Failed to get a ServiceWorkerRegistration: The URL protocol of the "
        "current origin ('" +
            document_origin->ToString() + "') is not supported."));
    return promise;
  }

  KURL completed_url = execution_context->CompleteURL(document_url);
  completed_url.RemoveFragmentIdentifier();
  if (!document_origin->CanRequest(completed_url)) {
    scoped_refptr<const SecurityOrigin> document_url_origin =
        SecurityOrigin::Create(completed_url);
    resolver->Reject(
        DOMException::Create(DOMExceptionCode::kSecurityError,
                             "Failed to get a ServiceWorkerRegistration: The "
                             "origin of the provided documentURL ('" +
                                 document_url_origin->ToString() +
                                 "') does not match the current origin ('" +
                                 document_origin->ToString() + "')."));
    return promise;
  }

  if (!provider_) {
    resolver->Reject(DOMException::Create(DOMExceptionCode::kInvalidStateError,
                                          "Failed to get a "
                                          "ServiceWorkerRegistration: The "
                                          "document is in an invalid state."));
    return promise;
  }
  provider_->GetRegistration(
      completed_url, std::make_unique<GetRegistrationCallback>(resolver));

  return promise;
}

ScriptPromise ServiceWorkerContainer::getRegistrations(
    ScriptState* script_state) {
  auto* resolver = MakeGarbageCollected<ScriptPromiseResolver>(script_state);
  ScriptPromise promise = resolver->Promise();

  if (!provider_) {
    resolver->Reject(
        DOMException::Create(DOMExceptionCode::kInvalidStateError,
                             "Failed to get ServiceWorkerRegistration objects: "
                             "The document is in an invalid state."));
    return promise;
  }

  ExecutionContext* execution_context = ExecutionContext::From(script_state);

  // The IDL definition is expected to restrict service worker to secure
  // contexts.
  CHECK(execution_context->IsSecureContext());

  scoped_refptr<const SecurityOrigin> document_origin =
      execution_context->GetSecurityOrigin();
  KURL page_url = KURL(NullURL(), document_origin->ToString());
  if (!SchemeRegistry::ShouldTreatURLSchemeAsAllowingServiceWorkers(
          page_url.Protocol())) {
    resolver->Reject(DOMException::Create(
        DOMExceptionCode::kSecurityError,
        "Failed to get ServiceWorkerRegistration objects: The URL protocol of "
        "the current origin ('" +
            document_origin->ToString() + "') is not supported."));
    return promise;
  }

  provider_->GetRegistrations(
      std::make_unique<CallbackPromiseAdapter<ServiceWorkerRegistrationArray,
                                              ServiceWorkerError>>(resolver));

  return promise;
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkercontainer-startmessages
void ServiceWorkerContainer::startMessages() {
  // "startMessages() method must enable the context object’s client message
  // queue if it is not enabled."
  EnableClientMessageQueue();
}

ScriptPromise ServiceWorkerContainer::ready(ScriptState* caller_state) {
  if (!GetExecutionContext())
    return ScriptPromise();

  if (!caller_state->World().IsMainWorld()) {
    // FIXME: Support .ready from isolated worlds when
    // ScriptPromiseProperty can vend Promises in isolated worlds.
    return ScriptPromise::RejectWithDOMException(
        caller_state,
        DOMException::Create(DOMExceptionCode::kNotSupportedError,
                             "'ready' is only supported in pages."));
  }

  if (!ready_) {
    ready_ = CreateReadyProperty();
    if (provider_) {
      provider_->GetRegistrationForReady(
          WTF::Bind(&ServiceWorkerContainer::OnGetRegistrationForReady,
                    WrapPersistent(this)));
    }
  }

  return ready_->Promise(caller_state->World());
}

void ServiceWorkerContainer::SetController(
    WebServiceWorkerObjectInfo info,
    bool should_notify_controller_change) {
  if (!GetExecutionContext())
    return;
  controller_ = ServiceWorker::From(GetExecutionContext(), std::move(info));
  if (controller_) {
    UseCounter::Count(GetExecutionContext(),
                      WebFeature::kServiceWorkerControlledPage);
    GetExecutionContext()->GetScheduler()->RegisterStickyFeature(
        SchedulingPolicy::Feature::kServiceWorkerControlledPage,
        {SchedulingPolicy::RecordMetricsForBackForwardCache()});
  }
  if (should_notify_controller_change)
    DispatchEvent(*Event::Create(event_type_names::kControllerchange));
}

void ServiceWorkerContainer::ReceiveMessage(WebServiceWorkerObjectInfo source,
                                            TransferableMessage message) {
  auto* context = GetExecutionContext();
  if (!context || !context->ExecutingWindow())
    return;
  // ServiceWorkerContainer is only supported on documents.
  auto* document = DynamicTo<Document>(context);
  DCHECK(document);

  if (!is_client_message_queue_enabled_) {
    if (!HasFiredDomContentLoaded(*document)) {
      // Wait for DOMContentLoaded. This corresponds to the specification steps
      // for "Parsing HTML documents": "The end" at
      // https://html.spec.whatwg.org/C/#the-end:
      //
      // 1. Fire an event named DOMContentLoaded at the Document object, with
      // its bubbles attribute initialized to true.
      // 2. Enable the client message queue of the ServiceWorkerContainer object
      // whose associated service worker client is the Document object's
      // relevant settings object.
      if (!dom_content_loaded_observer_) {
        dom_content_loaded_observer_ =
            MakeGarbageCollected<DomContentLoadedListener>();
        document->addEventListener(event_type_names::kDOMContentLoaded,
                                   dom_content_loaded_observer_.Get(), false);
      }
      queued_messages_.emplace_back(std::make_unique<MessageFromServiceWorker>(
          std::move(source), std::move(message)));
      // The messages will be dispatched once EnableClientMessageQueue() is
      // called.
      return;
    }

    // DOMContentLoaded was fired already, so enable the queue.
    EnableClientMessageQueue();
  }

  DispatchMessageEvent(std::move(source), std::move(message));
}

void ServiceWorkerContainer::CountFeature(mojom::WebFeature feature) {
  if (!GetExecutionContext())
    return;
  if (Deprecation::DeprecationMessage(feature).IsEmpty())
    UseCounter::Count(GetExecutionContext(), feature);
  else
    Deprecation::CountDeprecation(GetExecutionContext(), feature);
}

ExecutionContext* ServiceWorkerContainer::GetExecutionContext() const {
  return GetSupplementable();
}

const AtomicString& ServiceWorkerContainer::InterfaceName() const {
  return event_target_names::kServiceWorkerContainer;
}

void ServiceWorkerContainer::setOnmessage(EventListener* listener) {
  SetAttributeEventListener(event_type_names::kMessage, listener);
  // https://w3c.github.io/ServiceWorker/#dom-serviceworkercontainer-onmessage:
  // "The first time the context object’s onmessage IDL attribute is set, its
  // client message queue must be enabled."
  EnableClientMessageQueue();
}

EventListener* ServiceWorkerContainer::onmessage() {
  return GetAttributeEventListener(event_type_names::kMessage);
}

ServiceWorkerRegistration*
ServiceWorkerContainer::GetOrCreateServiceWorkerRegistration(
    WebServiceWorkerRegistrationObjectInfo info) {
  if (info.registration_id == mojom::blink::kInvalidServiceWorkerRegistrationId)
    return nullptr;

  ServiceWorkerRegistration* registration =
      service_worker_registration_objects_.at(info.registration_id);
  if (registration) {
    registration->Attach(std::move(info));
    return registration;
  }

  registration = MakeGarbageCollected<ServiceWorkerRegistration>(
      GetSupplementable(), std::move(info));
  service_worker_registration_objects_.Set(info.registration_id, registration);
  return registration;
}

ServiceWorker* ServiceWorkerContainer::GetOrCreateServiceWorker(
    WebServiceWorkerObjectInfo info) {
  if (info.version_id == mojom::blink::kInvalidServiceWorkerVersionId)
    return nullptr;
  ServiceWorker* worker = service_worker_objects_.at(info.version_id);
  if (!worker) {
    worker = MakeGarbageCollected<ServiceWorker>(GetSupplementable(),
                                                 std::move(info));
    service_worker_objects_.Set(info.version_id, worker);
  }
  return worker;
}

ServiceWorkerContainer::ServiceWorkerContainer(Document* document)
    : Supplement<Document>(*document), ContextLifecycleObserver(document) {}

ServiceWorkerContainer::ReadyProperty*
ServiceWorkerContainer::CreateReadyProperty() {
  return MakeGarbageCollected<ReadyProperty>(GetExecutionContext(), this,
                                             ReadyProperty::kReady);
}

void ServiceWorkerContainer::EnableClientMessageQueue() {
  dom_content_loaded_observer_ = nullptr;
  if (is_client_message_queue_enabled_) {
    DCHECK(queued_messages_.empty());
    return;
  }
  is_client_message_queue_enabled_ = true;
  std::vector<std::unique_ptr<MessageFromServiceWorker>> messages;
  messages.swap(queued_messages_);
  for (auto& message : messages) {
    DispatchMessageEvent(std::move(message->source),
                         std::move(message->message));
  }
}

void ServiceWorkerContainer::DispatchMessageEvent(
    WebServiceWorkerObjectInfo source,
    TransferableMessage message) {
  DCHECK(is_client_message_queue_enabled_);

  auto msg = ToBlinkTransferableMessage(std::move(message));
  MessagePortArray* ports =
      MessagePort::EntanglePorts(*GetExecutionContext(), std::move(msg.ports));
  ServiceWorker* service_worker =
      ServiceWorker::From(GetExecutionContext(), std::move(source));
  MessageEvent* event;
  if (!msg.locked_agent_cluster_id ||
      GetExecutionContext()->IsSameAgentCluster(*msg.locked_agent_cluster_id)) {
    event = MessageEvent::Create(
        ports, std::move(msg.message),
        GetExecutionContext()->GetSecurityOrigin()->ToString(),
        String() /* lastEventId */, service_worker);
  } else {
    event = MessageEvent::CreateError(
        GetExecutionContext()->GetSecurityOrigin()->ToString(), service_worker);
  }
  // Schedule the event to be dispatched on the correct task source:
  // https://w3c.github.io/ServiceWorker/#dfn-client-message-queue
  EnqueueEvent(*event, TaskType::kServiceWorkerClientMessage);
}

void ServiceWorkerContainer::OnGetRegistrationForReady(
    WebServiceWorkerRegistrationObjectInfo info) {
  DCHECK_EQ(ready_->GetState(), ReadyProperty::kPending);

  if (ready_->GetExecutionContext() &&
      !ready_->GetExecutionContext()->IsContextDestroyed()) {
    ready_->Resolve(
        ServiceWorkerContainer::From(
            To<Document>(ready_->GetExecutionContext()))
            ->GetOrCreateServiceWorkerRegistration(std::move(info)));
  }
}

}  // namespace blink