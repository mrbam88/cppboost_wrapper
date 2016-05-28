/*!
    \file "udp_jpeg_video_server.hpp"

    Formatting: 4 spaces/tab, 120 columns.
    Doc-tool: Doxygen (http://www.doxygen.com/)
*/


#ifndef UDP_JPEG_VIDEO_SERVER_HPP__43AC50C3_A1B0_4B8A_BB6A_699A42DCF293__INCLUDED
#define UDP_JPEG_VIDEO_SERVER_HPP__43AC50C3_A1B0_4B8A_BB6A_699A42DCF293__INCLUDED


#pragma once


#include <wmrobots/video/video_env.hpp>
#include <wmrobots/sync_domain_provider.hpp>


namespace wmrobots {
namespace video {


/**
 * Simple jpeg video stream [server] over UDP.
 */
class udp_jpeg_video_server
    : public boost::noncopyable
    , public sync_domain_provider
{
public:
    DECLARE_EXCEPTION(udp_jpeg_video_server_exception, "jpeg video server error");
    DECLARE_EXCEPTION(already_started_exception, "jpeg video server already started", udp_jpeg_video_server_exception);

    typedef boost::signals2::connection connection_t;
    typedef boost::signals2::signal<void (udp_jpeg_video_server& publisher)> operation_complete_signal_t;

public:
    void startup(string service_name);
    void shutdown();

    bool is_peer_connected() const;

    typedef unique_ptr<uint8_t[]> frame_data_ptr;

    void send_frame(frame_data_ptr frame_data, size_t frame_size_in_bytes); //!< Writes one frame to the stream.
    bool tx_in_pgrs() const { return 0 != mi_tx_in_pgrs; }
    size_t pending_frame_count() const; //!< Returns the number of pending frames (for transmission).

    void clear_frames(); //!< Erases/canceles/deletes all pending frames.

    bool enable_frame_transmission(bool value) { return (mb_frame_tx_enabled = value); } //!< Enables/disables frame transmission.
    bool frame_transmission_enabled() const { return mb_frame_tx_enabled; } //!< Queries frame transmission enable state.

    connection_t subscribe_to_operation_complete(operation_complete_signal_t::slot_type const& subscriber)
    {
        return m_operation_complete_publisher.connect(subscriber);
    }

private:
    void init_session_attrs();
    void transmit_next_packet();
    void on_async_tx_complete(boost::system::error_code const& error_code, size_t bytes_transferred);
    void on_async_rx_complete(boost::system::error_code const& error_code, size_t bytes_transferred);
    void reset_peer_disconnected_timer();
    void on_peer_disconnected_timer(boost::system::error_code const& error_code);
    bool handle_peer_disconnect();

    void notify_client_when_all_pending_operations_complete();

public:
    udp_jpeg_video_server(io_service& io_service, lock_type *sync_domain_lock = nullptr);
    virtual ~udp_jpeg_video_server();

private:
    mutable lock_type m_lock; // Synchronizes access to this when no lock is provided during construction.

    io_service& m_io_service;
    udp::socket m_socket;
    udp::endpoint m_remote_endpoint;
    deadline_timer m_peer_disconnected_timer;
    atomic<unsigned int> mi_peer_disconnected_timer_in_pgrs;

    struct frame_desc_t
    {
        frame_data_ptr mp_frame_data;
        size_t m_frame_data_size; // Number of bytes addressed by 'frame_data'.

        frame_desc_t() : mp_frame_data(nullptr), m_frame_data_size(0) {}
        frame_desc_t(frame_data_ptr frame_data, size_t frame_data_size)
            : mp_frame_data(move(frame_data)), m_frame_data_size(frame_data_size) {}
        frame_desc_t(frame_desc_t const&) = delete;
        frame_desc_t(frame_desc_t&& rhs)
            : mp_frame_data(move(rhs.mp_frame_data)), m_frame_data_size(rhs.m_frame_data_size) {}
        frame_desc_t& operator=(frame_desc_t const&) = delete;
        frame_desc_t& operator=(frame_desc_t&& rhs)
        {
            if (this != &rhs)
            {
                mp_frame_data = move(rhs.mp_frame_data);
                m_frame_data_size = rhs.m_frame_data_size;
            }
            return *this;
        }
    } mv_frames[2]; // [0] is being transmitted; [1] is pending.
    atomic<bool> mb_frame_tx_enabled; //!< true = frame transmission enabled; false = disabled.
    atomic<unsigned int> mi_tx_in_pgrs; //!< true = frame transmission in progress; false = idle.
    unsigned int mi_stream_frame_index; //!< Frame index/counter sent with frame data.
    unsigned int mi_stream_frame_part_index; //!< Frame part/segment/subsection index (within the frame).
    size_t mi_tx_buf_cap; //!< 'mp_tx_buf' capacity (number of bytes addressed by 'mp_tx_buf').
    unique_ptr<uint8_t[]> mp_tx_buf;

    atomic<unsigned int> mi_rx_in_pgrs; //!< true = receive in progress; false = idle.
    uint8_t m_rx_buf[1];

    operation_complete_signal_t m_operation_complete_publisher;
};


} // namespace video {
} // namespace wmrobots {


#endif // #ifndef UDP_JPEG_VIDEO_SERVER_HPP__43AC50C3_A1B0_4B8A_BB6A_699A42DCF293__INCLUDED


/*
    End of "udp_jpeg_video_server.hpp"
*/
