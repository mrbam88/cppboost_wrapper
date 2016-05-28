/*!
    \file "video_env.hpp"

    Formatting: 4 spaces/tab, 120 columns.
    Doc-tool: Doxygen (http://www.doxygen.com/)
*/


#ifndef VIDEO_ENV_HPP__ED71C361_408E_41F2_967F_A954E162875D__INCLUDED
#define VIDEO_ENV_HPP__ED71C361_408E_41F2_967F_A954E162875D__INCLUDED


#pragma once


#include <adapter/adapter_env.hpp>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion" // Prevent "implicit conversion loses integer precision" warning.
#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/placeholders.hpp>
#pragma GCC diagnostic pop


namespace adapter {
namespace video {


using namespace std;
using boost::asio::deadline_timer;
using boost::asio::ip::udp;
using boost::asio::io_service;


inline void video_nop(...) {}


#ifdef DEBUG

inline void video_vtrace(char const *format, va_list args) { adapter_vtrace(format, args); }
void video_trace(char const *format, ...);

#else // #ifdef DEBUG

inline void video_trace(char const *, ...) {}

#endif // #ifdef DEBUG


} // namespace video {
} // namespace adapter {


#endif // #ifndef VIDEO_ENV_HPP__ED71C361_408E_41F2_967F_A954E162875D__INCLUDED


/*
    End of "video_env.hpp"
*/
