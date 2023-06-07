#pragma once

#include <ripple/basics/Log.h>
#include <ripple/json/json_writer.h>

namespace xbwd {

[[nodiscard]] inline std::string
string_for_log(Json::Value const& v)
{
    ::Json::FastWriter w;
    return w.write(v);
}

// terse "std::tie"
template <class T1, class T2>
std::tuple<T1 const&, T2 const&>
jv(T1 const& t1, T2 const& t2)
{
    return std::tie(t1, t2);
}

template <class Stream>
void
jlogv_fields(Stream&& stream, char const* sep)
{
}

template <class Stream, class T1, class T2, class... Ts>
void
jlogv_fields(
    Stream&& stream,
    char const* sep,
    std::tuple<T1 const&, T2 const&> const& nameValue,
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

template <class Stream, class... Ts>
void
jlogv(
    Stream&& stream,
    std::size_t lineNo,
    std::string_view const& msg,
    Ts&&... nameValues)
{
    beast::Journal::ScopedStream s{std::forward<Stream>(stream), msg};
    s << " {";
    jlogv_fields(s, "", std::forward<Ts>(nameValues)..., jv("jlogId", lineNo));
    s << '}';
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
