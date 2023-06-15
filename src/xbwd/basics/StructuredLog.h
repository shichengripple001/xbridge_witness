#pragma once

#include <ripple/basics/Log.h>
#include <ripple/beast/core/CurrentThreadName.h>
#include <ripple/json/json_writer.h>

#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <tuple>

namespace xbwd {

[[nodiscard]] inline std::string
string_for_log(Json::Value const& v)
{
    ::Json::FastWriter w;
    return w.write(v);
}

// terse "std::tie"
template <class T2>
std::tuple<std::string, T2>
jv(std::string_view const t1, T2 const& t2)
{
    return std::make_tuple(std::string(t1), t2);
}

inline std::tuple<std::string, std::string>
jv(std::string_view const t1, char const* t2)
{
    return std::make_tuple(std::string(t1), std::string(t2));
}

inline std::tuple<std::string, std::string>
jv(std::string_view const t1, std::string_view const t2)
{
    return std::make_tuple(std::string(t1), std::string(t2));
}

template <class Stream>
void
jlogv_fields(Stream&& stream, std::string const& sep)
{
}

template <class Stream, class T1, class T2, class... Ts>
void
jlogv_fields(
    Stream&& stream,
    std::string const& sep,
    std::tuple<T1, T2> const& nameValue,
    Ts&&... nameValues)
{
    bool const withQuotes = [&] {
        if constexpr (std::is_arithmetic_v<T2>)
        {
            return false;
        }
        if constexpr (std::is_same_v<std::decay_t<T2>, Json::Value>)
        {
            auto const& v = std::get<1>(nameValue);
            return !v.isObject() && !v.isNumeric() && !v.isBool();
        }
        if constexpr (std::is_same_v<std::decay_t<T2>, ::Json::Compact>)
        {
            return false;
        }
        return true;
    }();

    stream << sep << '"' << std::get<0>(nameValue) << "\": ";
    if constexpr (std::is_same_v<std::decay_t<T2>, Json::Value>)
    {
        stream << string_for_log(std::get<1>(nameValue));
    }
    else
    {
        if (withQuotes)
        {
            // print the value with quotes
            stream << '"' << std::get<1>(nameValue) << '"';
        }
        else
        {
            stream << std::get<1>(nameValue);
        }
    }

    jlogv_fields(
        std::forward<Stream>(stream), ", ", std::forward<Ts>(nameValues)...);
}

class LoggerQueue
{
    std::deque<std::function<void()>> que_;
    std::mutex m_;
    std::atomic_bool fExit_;
    std::thread t_;

public:
    LoggerQueue(LoggerQueue const&) = delete;
    LoggerQueue(LoggerQueue&&) = delete;
    LoggerQueue&
    operator=(LoggerQueue const&) = delete;
    LoggerQueue&
    operator=(LoggerQueue&&) = delete;

    LoggerQueue() : fExit_(false), t_(&LoggerQueue::run, this)
    {
    }

    static LoggerQueue&
    instance()
    {
        static LoggerQueue l;
        return l;
    }

    void
    shutdown()
    {
        fExit_ = true;
        if (t_.joinable())
            t_.join();
    }

    void
    run()
    {
        using namespace std::chrono_literals;
        decltype(que_) q;

        beast::setCurrentThreadName("LoggerQue");

        for (; !fExit_; std::this_thread::sleep_for(50ms))
        {
            {
                std::lock_guard l(m_);
                q.swap(que_);
            }

            for (auto& f : q)
            {
                f();
                if (fExit_)
                    break;
            }
            q.clear();
        }
    }

    void
    push_back(std::function<void()>&& f)
    {
        std::lock_guard l(m_);
        que_.push_back(std::move(f));
    }
};

template <class Stream, class... Ts>
void
jlogv(
    Stream&& stream,
    std::size_t lineNo,
    std::string_view const& msg,
    Ts&&... nameValues)
{
    std::string cp_msg(msg);
    auto tupleArgs = std::make_tuple(std::forward<Ts>(nameValues)...);
    auto f = [stream = std::move(stream),
              lineNo,
              cp_msg = std::move(cp_msg),
              tupleArgs = std::move(tupleArgs)]() {
        beast::Journal::ScopedStream s{stream, cp_msg};
        s << " {";
        //
        std::apply(
            [&s, lineNo]<class... T2>(T2&&... nameValues) {
                jlogv_fields(
                    s,
                    std::string(),
                    std::forward<T2>(nameValues)...,
                    jv("jlogId", lineNo));
            },
            tupleArgs);
        s << '}';
    };

    LoggerQueue::instance().push_back(std::move(f));
}

// Wraps a Journal::Stream to skip evaluation of
// expensive argument lists if the stream is not active.
#ifndef JLOGV
#define JLOGV(x, msg, ...) \
    if (!x)                \
    {                      \
    }                      \
    else                   \
        xbwd::jlogv(x, __LINE__, msg, __VA_ARGS__)
#endif
}  // namespace xbwd
