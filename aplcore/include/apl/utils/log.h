/*
 * Copyright 2018 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *     http://aws.amazon.com/apache2.0/
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 *
 * Printf-style logging:     LOGF(LogLevel::ERROR, "%s -> %f", c_ptr, float);
 * iostream-style logging:   LOG(LogLevel::ERROR) << c_ptr << " -> " << float;
 */

#ifndef _APL_LOG_H
#define _APL_LOG_H

#include <cstdarg>
#include <memory>
#include <cstdio>
#include <vector>
#include <string>
#include <map>

#include "apl/utils/streamer.h"

namespace apl {

/// Logging levels
enum class LogLevel {
    NONE = -1,    /// Do not process log.
    TRACE = 0,    /// Trace
    DEBUG = 1,    /// Debug
    INFO = 2,     /// Info
    WARN = 3,     /// Warning
    ERROR = 5,    /// Error
    CRITICAL = 6, /// Critical
};

/// Log bridge interface
class LogBridge {
public:
    virtual ~LogBridge() {}

    virtual void transport(LogLevel level, const std::string& log) = 0;
};

/// Logger class.
class Logger
{
public:
    Logger(std::shared_ptr<LogBridge> bridge, LogLevel level, const std::string& file, const std::string& function);

    ~Logger();

    /**
     * @param format Log message format. Same format as for printf.
     * @param ... Variable arguments.
     */
    void log(const char *format, ...);

    /**
     * @param format Log message format. Same format as for printf.
     * @param args Variable arguments.
     */
    void log(const char *format, va_list args);

    template<class T>
    friend Logger& operator<<(Logger& os, T&& value)
    {
        os.mStringStream << std::forward<T>(value);
        return os;
    }

    template<class T>
    friend Logger& operator<<(Logger&& os, T&& value)
    {
        os.mStringStream << std::forward<T>(value);
        return os;
    }

private:
    const bool mUncaught;
    const std::shared_ptr<LogBridge> mBridge;
    const LogLevel mLevel;

    apl::streamer mStringStream;
};

// Hack to avoid compiler complaining
class LogVoidify {
public:
    LogVoidify() {}
    void operator&(Logger&) {}
};

/// Log creation and configuration class.
class LoggerFactory {
public:
    /**
     * @return singleton instance of LoggerFactory.
     */
    static LoggerFactory& instance();

    /**
     * Set consumer specific logger configuration.
     * @param bridge Consumer logging bridge.
     */
    void initialize(std::shared_ptr<LogBridge> bridge);

    /**
     * Reset Loggers state. Logging to reset to console.
     */
    void reset();

    /**
     * Create logger.
     * @param level Reporting log level.
     * @param file Origin file.
     * @param function Origin function.
     * @return Logger.
     */
    Logger getLogger(LogLevel level, const std::string& file, const std::string& function);

    LoggerFactory(const LoggerFactory&) = delete;
    LoggerFactory& operator=(const LoggerFactory&) = delete;

private:
    class DefaultLogBridge : public LogBridge {
    public:
        void transport(LogLevel level, const std::string& log) override {
            printf("%s %s\n", LEVEL_MAPPING.at(level).c_str(), log.c_str());
        }
    private:
        const std::map<LogLevel, std::string> LEVEL_MAPPING = {
                {LogLevel::TRACE, "T"},
                {LogLevel::DEBUG, "D"},
                {LogLevel::INFO, "I"},
                {LogLevel::WARN, "W"},
                {LogLevel::ERROR, "E"},
                {LogLevel::CRITICAL, "C"}
        };
    };

    LoggerFactory();

private:
    std::shared_ptr<LogBridge> mLogBridge;
    bool mInitialized;
    bool mWarned;
};

#ifdef APL_CORE_UWP
#define __FILENAME__ (strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 : __FILE__)
#else
#define __FILENAME__ (__builtin_strrchr(__FILE__, '/') ? __builtin_strrchr(__FILE__, '/') + 1 : __FILE__)
#endif
#define LOG(LEVEL) apl::LoggerFactory::instance().getLogger(LEVEL,__FILENAME__,__func__)
#define LOGF(LEVEL,FORMAT,...) apl::LoggerFactory::instance().getLogger(LEVEL,__FILENAME__,__func__).log(FORMAT,__VA_ARGS__)
#define LOG_IF(CONDITION) \
        !(CONDITION) ? (void) 0 : LogVoidify() & LOG(LogLevel::DEBUG)
#define LOGF_IF(CONDITION,FORMAT,...) \
        !(CONDITION) ? (void) 0 : LOGF(LogLevel::DEBUG,FORMAT,__VA_ARGS__)
} // namespace apl

#endif // _APL_LOG_H