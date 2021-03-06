#include <exception>
#include "proto/onnx/core/common/CommonSTD.h"
#include "proto/onnx/core/common/exceptions.h"
#include "proto/onnx/core/common/logging/isink.h"
#include "proto/onnx/core/common/logging/logging.h"

namespace ONNX
{
namespace Logging
{
const char *Category::Lotus = "Lotus";
const char *Category::System = "System";

using namespace std::chrono;

/*
As LoggingManager can be a static, we need to wrap the default instance and mutex in functions
to ensure they're initialized before use in LoggingManager::LoggingManager. If we don't, and
a static LoggingManager is created at startup, the file scope statics here may not have been
initialized.

We use 'new' to avoid static initialization issues
  https://isocpp.org/wiki/faq/ctors#static-init-order-on-first-use
Suppressing r.11 due to that
   Warning C26409 Avoid calling new and delete explicitly, use std::make_unique<T> instead
   r.11: http://go.microsoft.com/fwlink/?linkid=845485

*/

static std::atomic<void *> &DefaultLoggerManagerInstance() noexcept
{
    // this atomic is to protect against attempts to log being made after the default LoggingManager is destroyed.
    // Theoretically this can happen if a Logger instance is still alive and calls Log via its internal
    // pointer to the LoggingManager.
    // As the first thing LoggingManager::Log does is check the static DefaultLoggerManagerInstance() is not null,
    // any further damage should be prevented (in theory).
    static std::atomic<void *> default_instance;
    return default_instance;
}

static std::mutex &DefaultLoggerMutex() noexcept
{
    static std::mutex mutex;
    return mutex;
}

std::unique_ptr<Logger> &LoggingManager::GetDefaultLogger() noexcept
{
    static std::unique_ptr<Logger> default_logger;
    return default_logger;
}

static minutes InitLocaltimeOffset(const time_point<system_clock> &epoch) noexcept;

const LoggingManager::Epochs &LoggingManager::GetEpochs() noexcept
{
    // we save the value from system clock (which we can convert to a timestamp) as well as the high_resolution_clock.
    // from then on, we use the delta from the high_resolution_clock and apply that to the
    // system clock value.
    static Epochs epochs{high_resolution_clock::now(),
                         system_clock::now(),
                         InitLocaltimeOffset(system_clock::now())};
    return epochs;
}

LoggingManager::LoggingManager(std::unique_ptr<ISink> sink, Severity default_min_severity, bool filter_user_data,
                               const InstanceType instance_type, const std::string *default_logger_id,
                               int default_max_vlog_level)
    : sink_{std::move(sink)},
      default_min_severity_{default_min_severity},
      default_filter_user_data_{filter_user_data},
      default_max_vlog_level_{default_max_vlog_level},
      owns_default_logger_{false}
{
    if (!sink_)
    {
        throw std::logic_error("ISink must be provided.");
    }

    if (instance_type == InstanceType::Default)
    {
        if (default_logger_id == nullptr)
        {
            throw std::logic_error("default_logger_id must be provided if instance_type is InstanceType::Default");
        }

        // lock mutex to create instance, and enable logging
        // this matches the mutex usage in Shutdown
        std::lock_guard<std::mutex> guard(DefaultLoggerMutex());

        if (DefaultLoggerManagerInstance().load() != nullptr)
        {
            throw std::logic_error("Only one instance of LoggingManager created with InstanceType::Default can exist at any point in time.");
        }

        // This assertion passes, so using the atomic to validate calls to Log should
        // be reasonably economical.
        // assert(DefaultLoggerManagerInstance().is_lock_free());
        DefaultLoggerManagerInstance().store(this);

        CreateDefaultLogger(*default_logger_id);

        owns_default_logger_ = true;
    }
}

LoggingManager::~LoggingManager()
{
    if (owns_default_logger_)
    {
        // lock mutex to reset DefaultLoggerManagerInstance() and free default logger from this instance.
        std::lock_guard<std::mutex> guard(DefaultLoggerMutex());

        DefaultLoggerManagerInstance().store(nullptr, std::memory_order::memory_order_release);

        GetDefaultLogger().reset();
    }
}

void LoggingManager::CreateDefaultLogger(const std::string &logger_id)
{
    // this method is only called from ctor in scope where DefaultLoggerMutex() is already locked

    unique_ptr<Logger> &default_logger{GetDefaultLogger()};

    if (default_logger != nullptr)
    {
        throw std::logic_error("Default logger already set. ");
    }

    default_logger = CreateLogger(logger_id);
}

std::unique_ptr<Logger> LoggingManager::CreateLogger(std::string logger_id)
{
    return CreateLogger(logger_id, default_min_severity_, default_filter_user_data_, default_max_vlog_level_);
}

std::unique_ptr<Logger> LoggingManager::CreateLogger(std::string logger_id,
                                                     const Severity severity,
                                                     bool filter_user_data,
                                                     int vlog_level)
{
    auto logger = std::make_unique<Logger>(*this, logger_id, severity, filter_user_data, vlog_level);
    return logger;
}

void LoggingManager::Log(const std::string &logger_id, const Capture &message) const
{
    sink_->Send(GetTimestamp(), logger_id, message);
}

static minutes InitLocaltimeOffset(const time_point<system_clock> &epoch) noexcept
{
    // convert the system_clock time_point (UTC) to localtime and gmtime to calculate the difference.
    // we do this once, and apply that difference in GetTimestamp().
    // NOTE: If we happened to be running over a period where the time changed (e.g. daylight saving started)
    // we won't pickup the change. Not worth the extra cost to be 100% accurate 100% of the time.

    const time_t system_time_t = system_clock::to_time_t(epoch);
    tm local_tm;
    tm utc_tm;

#ifdef _WIN32
    localtime_s(&local_tm, &system_time_t);
    gmtime_s(&utc_tm, &system_time_t);
#else
    localtime_r(&system_time_t, &local_tm);
    gmtime_r(&system_time_t, &utc_tm);
#endif

    const double seconds = difftime(mktime(&local_tm), mktime(&utc_tm));

    // minutes should be accurate enough for timezone conversion
    return minutes{static_cast<int64_t>(seconds / 60)};
}

std::exception LoggingManager::LogFatalAndCreateException(const char *category,
                                                          const CodeLocation &location,
                                                          const char *format_str, ...)
{
    std::string exception_msg;

    // create Capture in separate scope so it gets destructed (leading to log output) before we throw.
    {
        ONNX::Logging::Capture c{ONNX::Logging::LoggingManager::DefaultLogger(),
                                  ONNX::Logging::Severity::kFATAL, category, ONNX::Logging::DataType::SYSTEM, location};
        va_list args;
        va_start(args, format_str);

        c.ProcessPrintf(format_str, args);
        va_end(args);

        exception_msg = c.Message();
    }

    return LotusException(location, exception_msg);
}
} // namespace Logging
} // namespace ONNX
