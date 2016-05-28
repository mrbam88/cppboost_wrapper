/*!
    \file "tftp_client.hpp"

    Formatting: 4 spaces/tab, 120 columns.
    Doc-tool: Doxygen (http://www.doxygen.com/)
*/


#ifndef TFTP_CLIENT_HPP__66751E59_CF24_4D48_940A_2F9BB269E274__INCLUDED
#define TFTP_CLIENT_HPP__66751E59_CF24_4D48_940A_2F9BB269E274__INCLUDED


#pragma once


#include <fstream>
#include <adapter/sync_domain_provider.hpp>
#include <adapter/net/asio_udp_receiver.hpp>
#include <adapter/tftp/tftp_env.hpp>


namespace adapter {
namespace tftp {


/**
 * TFTP client based on boost asio.
 */
class tftp_client
    : public boost::noncopyable
    , public sync_domain_provider
{
public:
    DECLARE_EXCEPTION(tftp_client_exception, "tftp client error");
    DECLARE_EXCEPTION(operation_in_progress_exception, "operation already in progress", tftp_client_exception);
    DECLARE_EXCEPTION(unresolved_endpoint_exception, "unresolved endpoint", tftp_client_exception);
    DECLARE_EXCEPTION(serialization_exception, "serialization error", tftp_client_exception);
    DECLARE_EXCEPTION(serialization_overflow_exception, "serialization overflow", serialization_exception);
    DECLARE_EXCEPTION(local_file_exception, "local file error", tftp_client_exception);

public:
    typedef boost::signals2::signal<void (tftp_client& publisher)> operation_complete_signal_t;
    typedef boost::signals2::connection connection_t;

public:
    template <typename op_complete_functor_type>
    void get(string host_name, string service_name, string remote_pathname, string local_pathname,
             op_complete_functor_type callback);

    void get(string host_name, string service_name, string remote_pathname, string local_pathname);

#if 0
    void put(string host_name, string service_name, string local_pathname, string remote_pathname);
#endif // #if 0

    bool in_progress() const;
    boost::system::error_code transfer_status() const;

    uint_least16_t set_blocksize_in_bytes(uint_least16_t value); // 512 bytes, by default.
    uint_least16_t blocksize_in_bytes() const;

    uint_least32_t set_transfersize_in_bytes(uint_least32_t value); // Zero (0) = use default.
    uint_least32_t transfersize_in_bytes() const;

    uint_least32_t set_timeout_in_seconds(uint_least32_t value); // Zero (0) = use default, i.e. 1 ( same as tftpd).
    uint_least32_t timeout_in_seconds() const;

    connection_t subscribe_to_operation_complete(operation_complete_signal_t::slot_type const& subscriber)
    {
        return m_operation_complete_publisher.connect(subscriber);
    }

    tftp_client(io_service& io_service, lock_type *sync_domain_lock = nullptr);
    virtual ~tftp_client();

protected:
    typedef boost::signals2::signal<void (boost::system::error_code)> startup_callback_type;
    typedef shared_ptr<startup_callback_type> startup_callback_ptr_type;

    void untyped_get(string host_name, string service_name, string remote_pathname, string local_pathname,
                     startup_callback_ptr_type callback_ptr);
    virtual void transfer_complete(boost::system::error_code status);

private:
    void notify_client_when_all_pending_operations_complete();

    void tx_read_rqst();
    void on_read_rqst_rply(boost::system::error_code const& error_code);
    void on_read_rqst_rply_oack(boost::system::error_code const& error_code);
    void on_read_rqst_rply_data(boost::system::error_code const& error_code);
    void on_ack_sent(boost::system::error_code const& error_code);
    void on_final_ack_sent(boost::system::error_code const& error_code);
    void on_error_sent(boost::system::error_code const& error_code);
    void tx_ack(unsigned int block_number, bool more_packets_coming);
    void tx_error(unsigned int error_code, string message = string());

// Serialization logic.
private:
    void set_tftp_options_to_defaults();
    void reset_tx_buf();

    uint8_t * tx_buf() { return mv_tx_buf; }
    size_t tx_buf_capacity() { return (std::min)(arycap(mv_tx_buf), static_cast<size_t>(m_blocksize_in_bytes)); }
    size_t tx_buf_capacity_remaining() { return tx_buf_capacity() - tx_buf_size(); }
    size_t& tx_buf_size() { return mi_tx_buf_size; }
    size_t tx_buf_size_remaining() { return tx_buf_size() - mi_rx_buf_iter; }

    uint8_t * rx_buf() { return mv_rx_buf; }
    size_t rx_buf_capacity() { return /*(std::min)(*/arycap(mv_rx_buf)/*, static_cast<size_t>(m_blocksize_in_bytes))*/; }
    size_t rx_buf_capacity_remaining() { return rx_buf_capacity() - rx_buf_size(); }
    size_t& rx_buf_size() { return mi_rx_buf_size; }
    size_t rx_buf_size_remaining() { return rx_buf_size() - mi_rx_buf_iter; }

    void serialize_uint16(uint value);
    uint deserialize_uint16();

    void serialize_opcode(uint value) { serialize_uint16(value); }
    uint deserialize_opcode() { return deserialize_uint16(); }

    void serialize_block_number(uint value) { serialize_uint16(value); }
    uint deserialize_block_number() { return deserialize_uint16(); }

    void serialize_string(string const& value);
    string deserialize_string();

    template <typename T> void serialize_integer_as_string(T value)
    {
        ostringstream oss;
        oss << dec << fixed << value;
        serialize_string(oss.str());
    }

    template <typename T> T deserialize_integer_as_string()
    {
        string value(deserialize_string());
        for (string::iterator iter = value.begin(); value.end() != iter; ++iter)
        {
            if (!isdigit(*iter)) { throw serialization_exception(); }
        }
        istringstream iss(value);
        T integer = 0;
        iss >> dec >> fixed >> integer;
        return integer;
    }

// Simple state machine for tx/rx logic.
private:
    typedef void (tftp_client::*io_completion_callback_t)(boost::system::error_code const&);

    void transmit(io_completion_callback_t io_completion_callback, bool expect_reply = true);
    void attempt_request(io_completion_callback_t io_completion_callback, bool expect_reply);
    void on_tx_complete(boost::system::error_code const& error_code, size_t bytes_transferred,
                        io_completion_callback_t io_completion_callback, bool expect_reply);
    void on_rx_complete(boost::system::error_code const& error_code, size_t bytes_transferred,
                        io_completion_callback_t io_completion_callback, bool expect_reply);

private:
    mutable lock_type m_lock; // Synchronizes access to this.
    io_service& m_io_service;
    deadline_timer m_io_timer;
    uint_least16_t m_blocksize_in_bytes;
    uint_least32_t m_transfersize_in_bytes;
    uint_least32_t m_timeout_in_seconds;
    operation_complete_signal_t m_operation_complete_publisher;
    startup_callback_ptr_type startup_callback_;

    udp::socket m_socket;
    udp::endpoint m_remote_endpoint; // The peer (for pilot - MAV IP and port; for MAV - pilot IP and port).

    fstream m_local_file;
    string m_remote_pathname;
    net::asio_udp_receiver m_receiver;
    unsigned int mi_async_io_timer_in_pgrs;
    unsigned int mi_async_rx_in_pgrs;
    unsigned int mi_async_tx_in_pgrs;
    uint8_t mv_tx_buf[4096];
    size_t mi_tx_buf_size;
    uint8_t mv_rx_buf[4096];
    size_t mi_rx_buf_size;
    size_t mi_rx_buf_iter;
    unsigned int mi_tx_attempt_counter;
    unsigned int mi_io_attempt_counter;
    unsigned int mi_io_attempt_max_count;
    bool mb_sent_options;
    uint16_t mi_expected_block_number;
    boost::system::error_code m_transfer_status;
};


template <typename op_complete_functor_type> void
tftp_client::get(string host_name, string service_name, string remote_pathname,
                 string local_pathname, op_complete_functor_type callback)
{
    startup_callback_ptr_type callback_ptr(new startup_callback_type);
    callback_ptr->connect(std::move(callback));
    untyped_get(host_name, service_name, remote_pathname, local_pathname, std::move(callback_ptr));
}


inline void
tftp_client::get(string host_name, string service_name, string remote_pathname, string local_pathname)
{
    untyped_get(host_name, service_name, remote_pathname, local_pathname, nullptr);
}


} // namespace tftp {
} // namespace adapter {


#endif // #ifndef TFTP_CLIENT_HPP__66751E59_CF24_4D48_940A_2F9BB269E274__INCLUDED


/*
    End of "tftp_client.hpp"
*/
