#include "source/common/tracing/tracer_manager_impl.h"

#include "source/common/config/utility.h"

namespace Envoy {
namespace Tracing {

TracerManagerImpl::TracerManagerImpl(Server::Configuration::TracerFactoryContextPtr factory_context)
    : factory_context_(std::move(factory_context)) {}

TracerSharedPtr
TracerManagerImpl::getOrCreateTracer(const envoy::config::trace::v3::Tracing_Http* config) {
  if (!config) {
    return null_tracer_;
  }

  const auto cache_key = MessageUtil::hash(*config);
  const auto it = tracers_.find(cache_key);
  if (it != tracers_.end()) {
    auto http_tracer = it->second.lock();
    if (http_tracer) { // HttpTracer might have been released since it's a weak reference
      return http_tracer;
    }
  }

  // Free memory held by expired weak references.
  //
  // Given that:
  //
  // * HttpTracer is obtained only once per listener lifecycle
  // * in a typical case, all listeners will have identical tracing configuration and, consequently,
  //   will share the same HttpTracer instance
  // * amount of memory held by an expired weak reference is minimal
  //
  // it seems reasonable to avoid introducing an external sweeper and only reclaim memory at
  // the moment when a new HttpTracer instance is about to be created.
  removeExpiredCacheEntries();

  // Initialize a new tracer.
  ENVOY_LOG(info, "instantiating a new tracer: {}", config->name());

  // Now see if there is a factory that will accept the config.
  auto& factory =
      Envoy::Config::Utility::getAndCheckFactory<Server::Configuration::TracerFactory>(*config);
  ProtobufTypes::MessagePtr message = Envoy::Config::Utility::translateToFactoryConfig(
      *config, factory_context_->messageValidationVisitor(), factory);

  HttpTracerSharedPtr tracer =
      std::make_shared<Tracing::TracerImpl>(factory.createTracerDriver(*message, *factory_context_),
                                            factory_context_->serverFactoryContext().localInfo());
  tracers_.emplace(cache_key, tracer); // cache a weak reference
  return tracer;
}

void TracerManagerImpl::removeExpiredCacheEntries() {
  absl::erase_if(tracers_,
                 [](const std::pair<const std::size_t, std::weak_ptr<HttpTracer>>& entry) {
                   return entry.second.expired();
                 });
}

} // namespace Tracing
} // namespace Envoy