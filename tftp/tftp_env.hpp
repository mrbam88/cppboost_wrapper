/*!
    \file "tftp_env.hpp"

    Formatting: 4 spaces/tab, 120 columns.
    Doc-tool: Doxygen (http://www.doxygen.com/)
*/


#ifndef TFTP_ENV_HPP__E79EC6E9_9CCA_4671_9C15_949AB4C45659__INCLUDED
#define TFTP_ENV_HPP__E79EC6E9_9CCA_4671_9C15_949AB4C45659__INCLUDED


#pragma once


#include <adapter/adapter_env.hpp>
#include <adapter/net/net_env.hpp>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion" // Prevent "implicit conversion loses integer precision" warning.
#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/placeholders.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/array.hpp>
#pragma GCC diagnostic pop


namespace adapter {
namespace tftp {


using boost::asio::deadline_timer;
using boost::asio::ip::udp;
using boost::asio::io_service;


inline void tftp_nop(...) {}


#ifdef DEBUG

inline void tftp_vtrace(char const *format, va_list args) { adapter_vtrace(format, args); }
void tftp_trace(char const *format, ...);

#else // #ifdef DEBUG

inline void tftp_trace(char const *, ...) {}

#endif // #ifdef DEBUG


} // namespace tftp {
} // namespace adapter {


#endif // #ifndef TFTP_ENV_HPP__E79EC6E9_9CCA_4671_9C15_949AB4C45659__INCLUDED


/*
    End of "tftp_env.hpp"
*/
