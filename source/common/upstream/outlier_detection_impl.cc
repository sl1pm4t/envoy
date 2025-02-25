#include "common/upstream/outlier_detection_impl.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "envoy/data/cluster/v2alpha/outlier_detection_event.pb.h"
#include "envoy/event/dispatcher.h"
#include "envoy/stats/scope.h"

#include "common/common/assert.h"
#include "common/common/enum_to_int.h"
#include "common/common/fmt.h"
#include "common/common/utility.h"
#include "common/http/codes.h"
#include "common/protobuf/utility.h"

namespace Envoy {
namespace Upstream {
namespace Outlier {

DetectorSharedPtr DetectorImplFactory::createForCluster(
    Cluster& cluster, const envoy::api::v2::Cluster& cluster_config, Event::Dispatcher& dispatcher,
    Runtime::Loader& runtime, EventLoggerSharedPtr event_logger) {
  if (cluster_config.has_outlier_detection()) {

    return DetectorImpl::create(cluster, cluster_config.outlier_detection(), dispatcher, runtime,
                                dispatcher.timeSource(), std::move(event_logger));
  } else {
    return nullptr;
  }
}

DetectorHostMonitorImpl::DetectorHostMonitorImpl(std::shared_ptr<DetectorImpl> detector,
                                                 HostSharedPtr host)
    : detector_(detector), host_(host),
      // add Success Rate monitors
      external_origin_SR_monitor_(envoy::data::cluster::v2alpha::OutlierEjectionType::SUCCESS_RATE),
      local_origin_SR_monitor_(
          envoy::data::cluster::v2alpha::OutlierEjectionType::SUCCESS_RATE_LOCAL_ORIGIN) {
  // Setup method to call when putResult is invoked. Depending on the config's
  // split_external_local_origin_errors_ boolean value different method is called.
  put_result_func_ = detector->config().splitExternalLocalOriginErrors()
                         ? &DetectorHostMonitorImpl::putResultWithLocalExternalSplit
                         : &DetectorHostMonitorImpl::putResultNoLocalExternalSplit;
}

void DetectorHostMonitorImpl::eject(MonotonicTime ejection_time) {
  ASSERT(!host_.lock()->healthFlagGet(Host::HealthFlag::FAILED_OUTLIER_CHECK));
  host_.lock()->healthFlagSet(Host::HealthFlag::FAILED_OUTLIER_CHECK);
  num_ejections_++;
  last_ejection_time_ = ejection_time;
}

void DetectorHostMonitorImpl::uneject(MonotonicTime unejection_time) {
  last_unejection_time_ = (unejection_time);
}

void DetectorHostMonitorImpl::updateCurrentSuccessRateBucket() {
  external_origin_SR_monitor_.updateCurrentSuccessRateBucket();
  local_origin_SR_monitor_.updateCurrentSuccessRateBucket();
}

void DetectorHostMonitorImpl::putHttpResponseCode(uint64_t response_code) {
  external_origin_SR_monitor_.incTotalReqCounter();
  if (Http::CodeUtility::is5xx(response_code)) {
    std::shared_ptr<DetectorImpl> detector = detector_.lock();
    if (!detector) {
      // It's possible for the cluster/detector to go away while we still have a host in use.
      return;
    }
    if (Http::CodeUtility::isGatewayError(response_code)) {
      if (++consecutive_gateway_failure_ == detector->runtime().snapshot().getInteger(
                                                "outlier_detection.consecutive_gateway_failure",
                                                detector->config().consecutiveGatewayFailure())) {
        detector->onConsecutiveGatewayFailure(host_.lock());
      }
    } else {
      consecutive_gateway_failure_ = 0;
    }

    if (++consecutive_5xx_ ==
        detector->runtime().snapshot().getInteger("outlier_detection.consecutive_5xx",
                                                  detector->config().consecutive5xx())) {
      detector->onConsecutive5xx(host_.lock());
    }
  } else {
    external_origin_SR_monitor_.incSuccessReqCounter();
    consecutive_5xx_ = 0;
    consecutive_gateway_failure_ = 0;
  }
}

absl::optional<Http::Code> DetectorHostMonitorImpl::resultToHttpCode(Result result) {
  Http::Code http_code = Http::Code::InternalServerError;

  switch (result) {
  case Result::EXT_ORIGIN_REQUEST_SUCCESS:
  case Result::LOCAL_ORIGIN_CONNECT_SUCCESS_FINAL:
    http_code = Http::Code::OK;
    break;
  case Result::LOCAL_ORIGIN_TIMEOUT:
    http_code = Http::Code::GatewayTimeout;
    break;
  case Result::LOCAL_ORIGIN_CONNECT_FAILED:
    http_code = Http::Code::ServiceUnavailable;
    break;
  case Result::EXT_ORIGIN_REQUEST_FAILED:
    http_code = Http::Code::InternalServerError;
    break;
    // LOCAL_ORIGIN_CONNECT_SUCCESS  is used is 2-layer protocols, like HTTP.
    // First connection is established and then higher level protocol runs.
    // If error happens in higher layer protocol, it will be mapped to
    // HTTP code indicating error. In order not to intervene with result of
    // higher layer protocol, this code is not mapped to HTTP code.
  case Result::LOCAL_ORIGIN_CONNECT_SUCCESS:
    return absl::nullopt;
  }

  return {http_code};
}

// Method is called by putResult when external and local origin errors
// are not treated differently. All errors are mapped to HTTP codes.
// Depending on the value of the parameter *code* the function behaves differently:
// - if the *code* is not defined, mapping uses resultToHttpCode method to do mapping.
// - if *code* is defined, it is taken as HTTP code and reported as such to outlier detector.
void DetectorHostMonitorImpl::putResultNoLocalExternalSplit(Result result,
                                                            absl::optional<uint64_t> code) {
  if (code) {
    putHttpResponseCode(code.value());
  } else {
    absl::optional<Http::Code> http_code = resultToHttpCode(result);
    if (http_code) {
      putHttpResponseCode(enumToInt(http_code.value()));
    }
  }
}

// Method is called by putResult when external and local origin errors
// are treated separately. Local origin errors have separate counters and
// separate success rate monitor.
void DetectorHostMonitorImpl::putResultWithLocalExternalSplit(Result result,
                                                              absl::optional<uint64_t>) {
  switch (result) {
  // SUCCESS is used to report success for connection level. Server may still respond with
  // error, but connection to server was OK.
  case Result::LOCAL_ORIGIN_CONNECT_SUCCESS:
  case Result::LOCAL_ORIGIN_CONNECT_SUCCESS_FINAL:
    return localOriginNoFailure();
  // Connectivity related errors.
  case Result::LOCAL_ORIGIN_TIMEOUT:
  case Result::LOCAL_ORIGIN_CONNECT_FAILED:
    return localOriginFailure();
  // EXT_ORIGIN_REQUEST_FAILED is used when connection to server was successful, but transaction on
  // server level failed. Since it it similar to HTTP 5xx, map it to 5xx handler.
  case Result::EXT_ORIGIN_REQUEST_FAILED:
    // map it to http code and call http handler.
    return putHttpResponseCode(enumToInt(Http::Code::ServiceUnavailable));
  // EXT_ORIGIN_REQUEST_SUCCESS is used to report that transaction with non-http server was
  // completed successfully. This means that connection and server level transactions were
  // successful. Map it to http code 200 OK and indicate that there was no errors on connection
  // level.
  case Result::EXT_ORIGIN_REQUEST_SUCCESS:
    putHttpResponseCode(enumToInt(Http::Code::OK));
    localOriginNoFailure();
    break;
  }
}

// Method is used by other components to reports success or error.
// It calls putResultWithLocalExternalSplit or put putResultNoLocalExternalSplit via
// std::function. The setting happens in constructor based on split_external_local_origin_errors
// config parameter.
void DetectorHostMonitorImpl::putResult(Result result, absl::optional<uint64_t> code) {
  put_result_func_(this, result, code);
}

void DetectorHostMonitorImpl::localOriginFailure() {
  std::shared_ptr<DetectorImpl> detector = detector_.lock();
  if (!detector) {
    // It's possible for the cluster/detector to go away while we still have a host in use.
    return;
  }
  local_origin_SR_monitor_.incTotalReqCounter();
  if (++consecutive_local_origin_failure_ ==
      detector->runtime().snapshot().getInteger(
          "outlier_detection.consecutive_local_origin_failure",
          detector->config().consecutiveLocalOriginFailure())) {
    detector->onConsecutiveLocalOriginFailure(host_.lock());
  }
}

void DetectorHostMonitorImpl::localOriginNoFailure() {
  std::shared_ptr<DetectorImpl> detector = detector_.lock();
  if (!detector) {
    // It's possible for the cluster/detector to go away while we still have a host in use.
    return;
  }

  local_origin_SR_monitor_.incTotalReqCounter();
  local_origin_SR_monitor_.incSuccessReqCounter();

  resetConsecutiveLocalOriginFailure();
}

DetectorConfig::DetectorConfig(const envoy::api::v2::cluster::OutlierDetection& config)
    : interval_ms_(static_cast<uint64_t>(PROTOBUF_GET_MS_OR_DEFAULT(config, interval, 10000))),
      base_ejection_time_ms_(
          static_cast<uint64_t>(PROTOBUF_GET_MS_OR_DEFAULT(config, base_ejection_time, 30000))),
      consecutive_5xx_(
          static_cast<uint64_t>(PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, consecutive_5xx, 5))),
      consecutive_gateway_failure_(static_cast<uint64_t>(
          PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, consecutive_gateway_failure, 5))),
      max_ejection_percent_(
          static_cast<uint64_t>(PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, max_ejection_percent, 10))),
      success_rate_minimum_hosts_(static_cast<uint64_t>(
          PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, success_rate_minimum_hosts, 5))),
      success_rate_request_volume_(static_cast<uint64_t>(
          PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, success_rate_request_volume, 100))),
      success_rate_stdev_factor_(static_cast<uint64_t>(
          PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, success_rate_stdev_factor, 1900))),
      enforcing_consecutive_5xx_(static_cast<uint64_t>(
          PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, enforcing_consecutive_5xx, 100))),
      enforcing_consecutive_gateway_failure_(static_cast<uint64_t>(
          PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, enforcing_consecutive_gateway_failure, 0))),
      enforcing_success_rate_(static_cast<uint64_t>(
          PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, enforcing_success_rate, 100))),
      split_external_local_origin_errors_(config.split_external_local_origin_errors()),
      consecutive_local_origin_failure_(static_cast<uint64_t>(
          PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, consecutive_local_origin_failure, 5))),
      enforcing_consecutive_local_origin_failure_(
          static_cast<uint64_t>(PROTOBUF_GET_WRAPPED_OR_DEFAULT(
              config, enforcing_consecutive_local_origin_failure, 100))),
      enforcing_local_origin_success_rate_(static_cast<uint64_t>(
          PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, enforcing_local_origin_success_rate, 100))) {}

DetectorImpl::DetectorImpl(const Cluster& cluster,
                           const envoy::api::v2::cluster::OutlierDetection& config,
                           Event::Dispatcher& dispatcher, Runtime::Loader& runtime,
                           TimeSource& time_source, EventLoggerSharedPtr event_logger)
    : config_(config), dispatcher_(dispatcher), runtime_(runtime), time_source_(time_source),
      stats_(generateStats(cluster.info()->statsScope())),
      interval_timer_(dispatcher.createTimer([this]() -> void { onIntervalTimer(); })),
      event_logger_(event_logger) {
  // Insert success rate initial numbers for each type of SR detector
  external_origin_SR_num_ = {-1, -1};
  local_origin_SR_num_ = {-1, -1};
}

DetectorImpl::~DetectorImpl() {
  for (const auto& host : host_monitors_) {
    if (host.first->healthFlagGet(Host::HealthFlag::FAILED_OUTLIER_CHECK)) {
      ASSERT(stats_.ejections_active_.value() > 0);
      stats_.ejections_active_.dec();
    }
  }
}

std::shared_ptr<DetectorImpl>
DetectorImpl::create(const Cluster& cluster,
                     const envoy::api::v2::cluster::OutlierDetection& config,
                     Event::Dispatcher& dispatcher, Runtime::Loader& runtime,
                     TimeSource& time_source, EventLoggerSharedPtr event_logger) {
  std::shared_ptr<DetectorImpl> detector(
      new DetectorImpl(cluster, config, dispatcher, runtime, time_source, event_logger));
  detector->initialize(cluster);

  return detector;
}

void DetectorImpl::initialize(const Cluster& cluster) {
  for (auto& host_set : cluster.prioritySet().hostSetsPerPriority()) {
    for (const HostSharedPtr& host : host_set->hosts()) {
      addHostMonitor(host);
    }
  }
  cluster.prioritySet().addMemberUpdateCb(
      [this](const HostVector& hosts_added, const HostVector& hosts_removed) -> void {
        for (const HostSharedPtr& host : hosts_added) {
          addHostMonitor(host);
        }

        for (const HostSharedPtr& host : hosts_removed) {
          ASSERT(host_monitors_.count(host) == 1);
          if (host->healthFlagGet(Host::HealthFlag::FAILED_OUTLIER_CHECK)) {
            ASSERT(stats_.ejections_active_.value() > 0);
            stats_.ejections_active_.dec();
          }

          host_monitors_.erase(host);
        }
      });

  armIntervalTimer();
}

void DetectorImpl::addHostMonitor(HostSharedPtr host) {
  ASSERT(host_monitors_.count(host) == 0);
  DetectorHostMonitorImpl* monitor = new DetectorHostMonitorImpl(shared_from_this(), host);
  host_monitors_[host] = monitor;
  host->setOutlierDetector(DetectorHostMonitorPtr{monitor});
}

void DetectorImpl::armIntervalTimer() {
  interval_timer_->enableTimer(std::chrono::milliseconds(
      runtime_.snapshot().getInteger("outlier_detection.interval_ms", config_.intervalMs())));
}

void DetectorImpl::checkHostForUneject(HostSharedPtr host, DetectorHostMonitorImpl* monitor,
                                       MonotonicTime now) {
  if (!host->healthFlagGet(Host::HealthFlag::FAILED_OUTLIER_CHECK)) {
    return;
  }

  std::chrono::milliseconds base_eject_time =
      std::chrono::milliseconds(runtime_.snapshot().getInteger(
          "outlier_detection.base_ejection_time_ms", config_.baseEjectionTimeMs()));
  ASSERT(monitor->numEjections() > 0);
  if ((base_eject_time * monitor->numEjections()) <= (now - monitor->lastEjectionTime().value())) {
    stats_.ejections_active_.dec();
    host->healthFlagClear(Host::HealthFlag::FAILED_OUTLIER_CHECK);
    // Reset the consecutive failure counters to avoid re-ejection on very few new errors due
    // to the non-triggering counter being close to its trigger value.
    host_monitors_[host]->resetConsecutive5xx();
    host_monitors_[host]->resetConsecutiveGatewayFailure();
    monitor->uneject(now);
    runCallbacks(host);

    if (event_logger_) {
      event_logger_->logUneject(host);
    }
  }
}

bool DetectorImpl::enforceEjection(envoy::data::cluster::v2alpha::OutlierEjectionType type) {
  switch (type) {
  case envoy::data::cluster::v2alpha::OutlierEjectionType::CONSECUTIVE_5XX:
    return runtime_.snapshot().featureEnabled("outlier_detection.enforcing_consecutive_5xx",
                                              config_.enforcingConsecutive5xx());
  case envoy::data::cluster::v2alpha::OutlierEjectionType::CONSECUTIVE_GATEWAY_FAILURE:
    return runtime_.snapshot().featureEnabled(
        "outlier_detection.enforcing_consecutive_gateway_failure",
        config_.enforcingConsecutiveGatewayFailure());
  case envoy::data::cluster::v2alpha::OutlierEjectionType::SUCCESS_RATE:
    return runtime_.snapshot().featureEnabled("outlier_detection.enforcing_success_rate",
                                              config_.enforcingSuccessRate());
  case envoy::data::cluster::v2alpha::OutlierEjectionType::CONSECUTIVE_LOCAL_ORIGIN_FAILURE:
    return runtime_.snapshot().featureEnabled(
        "outlier_detection.enforcing_consecutive_local_origin_failure",
        config_.enforcingConsecutiveLocalOriginFailure());
  case envoy::data::cluster::v2alpha::OutlierEjectionType::SUCCESS_RATE_LOCAL_ORIGIN:
    return runtime_.snapshot().featureEnabled(
        "outlier_detection.enforcing_local_origin_success_rate",
        config_.enforcingLocalOriginSuccessRate());
  default:
    // Checked by schema.
    NOT_REACHED_GCOVR_EXCL_LINE;
  }

  NOT_REACHED_GCOVR_EXCL_LINE;
}

void DetectorImpl::updateEnforcedEjectionStats(
    envoy::data::cluster::v2alpha::OutlierEjectionType type) {
  stats_.ejections_enforced_total_.inc();
  switch (type) {
  case envoy::data::cluster::v2alpha::OutlierEjectionType::SUCCESS_RATE:
    stats_.ejections_enforced_success_rate_.inc();
    break;
  case envoy::data::cluster::v2alpha::OutlierEjectionType::CONSECUTIVE_5XX:
    stats_.ejections_enforced_consecutive_5xx_.inc();
    break;
  case envoy::data::cluster::v2alpha::OutlierEjectionType::CONSECUTIVE_GATEWAY_FAILURE:
    stats_.ejections_enforced_consecutive_gateway_failure_.inc();
    break;
  case envoy::data::cluster::v2alpha::OutlierEjectionType::CONSECUTIVE_LOCAL_ORIGIN_FAILURE:
    stats_.ejections_enforced_consecutive_local_origin_failure_.inc();
    break;
  case envoy::data::cluster::v2alpha::OutlierEjectionType::SUCCESS_RATE_LOCAL_ORIGIN:
    stats_.ejections_enforced_local_origin_success_rate_.inc();
    break;
  default:
    // Checked by schema.
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
}

void DetectorImpl::updateDetectedEjectionStats(
    envoy::data::cluster::v2alpha::OutlierEjectionType type) {
  switch (type) {
  case envoy::data::cluster::v2alpha::OutlierEjectionType::SUCCESS_RATE:
    stats_.ejections_detected_success_rate_.inc();
    break;
  case envoy::data::cluster::v2alpha::OutlierEjectionType::CONSECUTIVE_5XX:
    stats_.ejections_detected_consecutive_5xx_.inc();
    break;
  case envoy::data::cluster::v2alpha::OutlierEjectionType::CONSECUTIVE_GATEWAY_FAILURE:
    stats_.ejections_detected_consecutive_gateway_failure_.inc();
    break;
  case envoy::data::cluster::v2alpha::OutlierEjectionType::CONSECUTIVE_LOCAL_ORIGIN_FAILURE:
    stats_.ejections_detected_consecutive_local_origin_failure_.inc();
    break;
  case envoy::data::cluster::v2alpha::OutlierEjectionType::SUCCESS_RATE_LOCAL_ORIGIN:
    stats_.ejections_detected_local_origin_success_rate_.inc();
    break;
  default:
    // Checked by schema.
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
}

void DetectorImpl::ejectHost(HostSharedPtr host,
                             envoy::data::cluster::v2alpha::OutlierEjectionType type) {
  uint64_t max_ejection_percent = std::min<uint64_t>(
      100, runtime_.snapshot().getInteger("outlier_detection.max_ejection_percent",
                                          config_.maxEjectionPercent()));
  double ejected_percent = 100.0 * stats_.ejections_active_.value() / host_monitors_.size();
  // Note this is not currently checked per-priority level, so it is possible
  // for outlier detection to eject all hosts at any given priority level.
  if (ejected_percent < max_ejection_percent) {
    if (type == envoy::data::cluster::v2alpha::OutlierEjectionType::CONSECUTIVE_5XX ||
        type == envoy::data::cluster::v2alpha::OutlierEjectionType::SUCCESS_RATE) {
      // Deprecated counter, preserving old behaviour until it's removed.
      stats_.ejections_total_.inc();
    }
    if (enforceEjection(type)) {
      stats_.ejections_active_.inc();
      updateEnforcedEjectionStats(type);
      host_monitors_[host]->eject(time_source_.monotonicTime());
      runCallbacks(host);
      if (event_logger_) {
        event_logger_->logEject(host, *this, type, true);
      }
    } else {
      if (event_logger_) {
        event_logger_->logEject(host, *this, type, false);
      }
    }
  } else {
    stats_.ejections_overflow_.inc();
  }
}

DetectionStats DetectorImpl::generateStats(Stats::Scope& scope) {
  std::string prefix("outlier_detection.");
  return {ALL_OUTLIER_DETECTION_STATS(POOL_COUNTER_PREFIX(scope, prefix),
                                      POOL_GAUGE_PREFIX(scope, prefix))};
}

void DetectorImpl::notifyMainThreadConsecutiveError(
    HostSharedPtr host, envoy::data::cluster::v2alpha::OutlierEjectionType type) {
  // This event will come from all threads, so we synchronize with a post to the main thread.
  // NOTE: Unfortunately consecutive errors are complicated from a threading perspective because
  //       we catch consecutive errors on worker threads and then post back to the main thread.
  //       Clusters can get removed, and this means there is a race condition with this
  //       reverse post. The way we handle this is as follows:
  //       1) The only strong pointer to the detector is owned by the cluster.
  //       2) We post a weak pointer to the main thread.
  //       3) If when running on the main thread the weak pointer can be converted to a strong
  //          pointer, the detector/cluster must still exist so we can safely fire callbacks.
  //          Otherwise we do nothing since the detector/cluster is already gone.
  std::weak_ptr<DetectorImpl> weak_this = shared_from_this();
  dispatcher_.post([weak_this, host, type]() -> void {
    std::shared_ptr<DetectorImpl> shared_this = weak_this.lock();
    if (shared_this) {
      shared_this->onConsecutiveErrorWorker(host, type);
    }
  });
}

void DetectorImpl::onConsecutive5xx(HostSharedPtr host) {
  notifyMainThreadConsecutiveError(
      host, envoy::data::cluster::v2alpha::OutlierEjectionType::CONSECUTIVE_5XX);
}

void DetectorImpl::onConsecutiveGatewayFailure(HostSharedPtr host) {
  notifyMainThreadConsecutiveError(
      host, envoy::data::cluster::v2alpha::OutlierEjectionType::CONSECUTIVE_GATEWAY_FAILURE);
}

void DetectorImpl::onConsecutiveLocalOriginFailure(HostSharedPtr host) {
  notifyMainThreadConsecutiveError(
      host, envoy::data::cluster::v2alpha::OutlierEjectionType::CONSECUTIVE_LOCAL_ORIGIN_FAILURE);
}

void DetectorImpl::onConsecutiveErrorWorker(
    HostSharedPtr host, envoy::data::cluster::v2alpha::OutlierEjectionType type) {
  // Ejections come in cross thread. There is a chance that the host has already been removed from
  // the set. If so, just ignore it.
  if (host_monitors_.count(host) == 0) {
    return;
  }
  if (host->healthFlagGet(Host::HealthFlag::FAILED_OUTLIER_CHECK)) {
    return;
  }

  // We also reset the appropriate counter here to allow the monitor to detect a bout of consecutive
  // error responses even if the monitor is not charged with an interleaved non-error code.
  updateDetectedEjectionStats(type);
  ejectHost(host, type);

  // reset counters
  switch (type) {
  case envoy::data::cluster::v2alpha::OutlierEjectionType::CONSECUTIVE_5XX:
    stats_.ejections_consecutive_5xx_.inc(); // Deprecated
    host_monitors_[host]->resetConsecutive5xx();
    break;
  case envoy::data::cluster::v2alpha::OutlierEjectionType::CONSECUTIVE_GATEWAY_FAILURE:
    host_monitors_[host]->resetConsecutiveGatewayFailure();
    break;
  case envoy::data::cluster::v2alpha::OutlierEjectionType::CONSECUTIVE_LOCAL_ORIGIN_FAILURE:
    host_monitors_[host]->resetConsecutiveLocalOriginFailure();
    break;
  default:
    // Checked by schema.
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
}

DetectorImpl::EjectionPair DetectorImpl::successRateEjectionThreshold(
    double success_rate_sum, const std::vector<HostSuccessRatePair>& valid_success_rate_hosts,
    double success_rate_stdev_factor) {
  // This function is using mean and standard deviation as statistical measures for outlier
  // detection. First the mean is calculated by dividing the sum of success rate data over the
  // number of data points. Then variance is calculated by taking the mean of the
  // squared difference of data points to the mean of the data. Then standard deviation is
  // calculated by taking the square root of the variance. Then the outlier threshold is
  // calculated as the difference between the mean and the product of the standard
  // deviation and a constant factor.
  //
  // For example with a data set that looks like success_rate_data = {50, 100, 100, 100, 100} the
  // math would work as follows:
  // success_rate_sum = 450
  // mean = 90
  // variance = 400
  // stdev = 20
  // threshold returned = 52
  double mean = success_rate_sum / valid_success_rate_hosts.size();
  double variance = 0;
  std::for_each(valid_success_rate_hosts.begin(), valid_success_rate_hosts.end(),
                [&variance, mean](HostSuccessRatePair v) {
                  variance += std::pow(v.success_rate_ - mean, 2);
                });
  variance /= valid_success_rate_hosts.size();
  double stdev = std::sqrt(variance);

  return {mean, (mean - (success_rate_stdev_factor * stdev))};
}

void DetectorImpl::processSuccessRateEjections(
    DetectorHostMonitor::SuccessRateMonitorType monitor_type) {
  uint64_t success_rate_minimum_hosts = runtime_.snapshot().getInteger(
      "outlier_detection.success_rate_minimum_hosts", config_.successRateMinimumHosts());
  uint64_t success_rate_request_volume = runtime_.snapshot().getInteger(
      "outlier_detection.success_rate_request_volume", config_.successRateRequestVolume());
  std::vector<HostSuccessRatePair> valid_success_rate_hosts;
  double success_rate_sum = 0;

  // Reset the Detector's success rate mean and stdev.
  getSRNums(monitor_type) = {-1, -1};

  // Exit early if there are not enough hosts.
  if (host_monitors_.size() < success_rate_minimum_hosts) {
    return;
  }

  // reserve upper bound of vector size to avoid reallocation.
  valid_success_rate_hosts.reserve(host_monitors_.size());

  for (const auto& host : host_monitors_) {
    // Don't do work if the host is already ejected.
    if (!host.first->healthFlagGet(Host::HealthFlag::FAILED_OUTLIER_CHECK)) {
      absl::optional<double> host_success_rate = host.second->getSRMonitor(monitor_type)
                                                     .successRateAccumulator()
                                                     .getSuccessRate(success_rate_request_volume);

      if (host_success_rate) {
        valid_success_rate_hosts.emplace_back(
            HostSuccessRatePair(host.first, host_success_rate.value()));
        success_rate_sum += host_success_rate.value();
        host.second->successRate(monitor_type, host_success_rate.value());
      }
    }
  }

  if (!valid_success_rate_hosts.empty() &&
      valid_success_rate_hosts.size() >= success_rate_minimum_hosts) {
    const double success_rate_stdev_factor =
        runtime_.snapshot().getInteger("outlier_detection.success_rate_stdev_factor",
                                       config_.successRateStdevFactor()) /
        1000.0;
    getSRNums(monitor_type) = successRateEjectionThreshold(
        success_rate_sum, valid_success_rate_hosts, success_rate_stdev_factor);
    const double success_rate_ejection_threshold = getSRNums(monitor_type).ejection_threshold_;
    for (const auto& host_success_rate_pair : valid_success_rate_hosts) {
      if (host_success_rate_pair.success_rate_ < success_rate_ejection_threshold) {
        stats_.ejections_success_rate_.inc(); // Deprecated.
        const envoy::data::cluster::v2alpha::OutlierEjectionType type =
            host_monitors_[host_success_rate_pair.host_]
                ->getSRMonitor(monitor_type)
                .getEjectionType();
        updateDetectedEjectionStats(type);
        ejectHost(host_success_rate_pair.host_, type);
      }
    }
  }
}

void DetectorImpl::onIntervalTimer() {
  MonotonicTime now = time_source_.monotonicTime();

  for (auto host : host_monitors_) {
    checkHostForUneject(host.first, host.second, now);

    // Need to update the writer bucket to keep the data valid.
    host.second->updateCurrentSuccessRateBucket();
    // Refresh host success rate stat for the /clusters endpoint. If there is a new valid value, it
    // will get updated in processSuccessRateEjections().
    host.second->successRate(DetectorHostMonitor::SuccessRateMonitorType::LocalOrigin, -1);
    host.second->successRate(DetectorHostMonitor::SuccessRateMonitorType::ExternalOrigin, -1);
  }

  processSuccessRateEjections(DetectorHostMonitor::SuccessRateMonitorType::ExternalOrigin);
  processSuccessRateEjections(DetectorHostMonitor::SuccessRateMonitorType::LocalOrigin);

  armIntervalTimer();
}

void DetectorImpl::runCallbacks(HostSharedPtr host) {
  for (const ChangeStateCb& cb : callbacks_) {
    cb(host);
  }
}

void EventLoggerImpl::logEject(const HostDescriptionConstSharedPtr& host, Detector& detector,
                               envoy::data::cluster::v2alpha::OutlierEjectionType type,
                               bool enforced) {
  envoy::data::cluster::v2alpha::OutlierDetectionEvent event;
  event.set_type(type);

  absl::optional<MonotonicTime> time = host->outlierDetector().lastUnejectionTime();
  setCommonEventParams(event, host, time);

  event.set_action(envoy::data::cluster::v2alpha::Action::EJECT);

  event.set_enforced(enforced);

  if ((type == envoy::data::cluster::v2alpha::OutlierEjectionType::SUCCESS_RATE) ||
      (type == envoy::data::cluster::v2alpha::OutlierEjectionType::SUCCESS_RATE_LOCAL_ORIGIN)) {
    const DetectorHostMonitor::SuccessRateMonitorType monitor_type =
        (type == envoy::data::cluster::v2alpha::OutlierEjectionType::SUCCESS_RATE)
            ? DetectorHostMonitor::SuccessRateMonitorType::ExternalOrigin
            : DetectorHostMonitor::SuccessRateMonitorType::LocalOrigin;
    event.mutable_eject_success_rate_event()->set_cluster_average_success_rate(
        detector.successRateAverage(monitor_type));
    event.mutable_eject_success_rate_event()->set_cluster_success_rate_ejection_threshold(
        detector.successRateEjectionThreshold(monitor_type));
    event.mutable_eject_success_rate_event()->set_host_success_rate(
        host->outlierDetector().successRate(monitor_type));
  } else {
    event.mutable_eject_consecutive_event();
  }

  const auto json = MessageUtil::getJsonStringFromMessage(event, /* pretty_print */ false,
                                                          /* always_print_primitive_fields */ true);
  file_->write(fmt::format("{}\n", json));
}

void EventLoggerImpl::logUneject(const HostDescriptionConstSharedPtr& host) {
  envoy::data::cluster::v2alpha::OutlierDetectionEvent event;

  absl::optional<MonotonicTime> time = host->outlierDetector().lastEjectionTime();
  setCommonEventParams(event, host, time);

  event.set_action(envoy::data::cluster::v2alpha::Action::UNEJECT);

  const auto json = MessageUtil::getJsonStringFromMessage(event, /* pretty_print */ false,
                                                          /* always_print_primitive_fields */ true);
  file_->write(fmt::format("{}\n", json));
}

void EventLoggerImpl::setCommonEventParams(
    envoy::data::cluster::v2alpha::OutlierDetectionEvent& event,
    const HostDescriptionConstSharedPtr& host, absl::optional<MonotonicTime> time) {
  MonotonicTime monotonic_now = time_source_.monotonicTime();
  if (time) {
    std::chrono::seconds secsFromLastAction =
        std::chrono::duration_cast<std::chrono::seconds>(monotonic_now - time.value());
    event.mutable_secs_since_last_action()->set_value(secsFromLastAction.count());
  }
  event.set_cluster_name(host->cluster().name());
  event.set_upstream_url(host->address()->asString());
  event.set_num_ejections(host->outlierDetector().numEjections());
  TimestampUtil::systemClockToTimestamp(time_source_.systemTime(), *event.mutable_timestamp());
}

SuccessRateAccumulatorBucket* SuccessRateAccumulator::updateCurrentWriter() {
  // Right now current is being written to and backup is not. Flush the backup and swap.
  backup_success_rate_bucket_->success_request_counter_ = 0;
  backup_success_rate_bucket_->total_request_counter_ = 0;

  current_success_rate_bucket_.swap(backup_success_rate_bucket_);

  return current_success_rate_bucket_.get();
}

absl::optional<double>
SuccessRateAccumulator::getSuccessRate(uint64_t success_rate_request_volume) {
  if (backup_success_rate_bucket_->total_request_counter_ < success_rate_request_volume) {
    return absl::optional<double>();
  }

  return {backup_success_rate_bucket_->success_request_counter_ * 100.0 /
          backup_success_rate_bucket_->total_request_counter_};
}

} // namespace Outlier
} // namespace Upstream
} // namespace Envoy
