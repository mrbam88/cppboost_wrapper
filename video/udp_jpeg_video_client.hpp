/*!
    \file "udp_jpeg_video_client.hpp"

    Formatting: 4 spaces/tab, 120 columns.
    Doc-tool: Doxygen (http://www.doxygen.com/)
*/


#ifndef UDP_JPEG_VIDEO_CLIENT_HPP__698B6D1B_88CF_4E49_A781_79799C4B37B7__INCLUDED
#define UDP_JPEG_VIDEO_CLIENT_HPP__698B6D1B_88CF_4E49_A781_79799C4B37B7__INCLUDED


#pragma once


#include <adapter/video/video_env.hpp>
#include <adapter/sync_domain_provider.hpp>


namespace adapter {
namespace video {


/**
 * Simple jpeg video stream [server] over UDP.
 */
class udp_jpeg_video_client
    : public boost::noncopyable
    , public sync_domain_provider
{
public:
    DECLARE_EXCEPTION(udp_jpeg_video_client_exception, "jpeg video server error");
    DECLARE_EXCEPTION(already_started_exception, "jpeg video server already started", udp_jpeg_video_client_exception);

    typedef boost::signals2::connection connection_t;
    typedef boost::signals2::signal<void (udp_jpeg_video_client& publisher)> operation_complete_signal_t;
    typedef boost::signals2::signal<void (udp_jpeg_video_client& publisher)> change_signal_t;

    typedef unique_ptr<uint8_t[]> frame_data_ptr;
    typedef pair<frame_data_ptr, size_t> video_frame;

public:
    void startup(string host_name, string service_name);
    void shutdown();
    bool is_running() const;

    video_frame get_frame(); //!< Reads one frame from the stream.
    bool is_frame_pending() const; //!< Indicates whether or not a new frame has been received and is ready for retrieval.
    bool rx_in_pgrs() const { return 0 < mi_rx_in_pgrs; }

    void clear_frames();

    bool enable_frame_reception(bool value); //!< Enables/disables frame reception.
    bool frame_reception_enabled() const { return mb_frame_rx_enabled; } //!< Queries frame reception enable state.

    connection_t subscribe_to_operation_complete(operation_complete_signal_t::slot_type const& subscriber)
    {
        return m_operation_complete_publisher.connect(subscriber);
    }
    
    connection_t subscribe_to_change(change_signal_t::slot_type const& subscriber)
    {
        return m_change_publisher.connect(subscriber);
    }

    udp_jpeg_video_client(io_service& io_service, lock_type *sync_domain_lock = nullptr);
    virtual ~udp_jpeg_video_client();

private:
    void init_session_attrs();
    typedef unique_ptr<uint8_t[]> frame_part_flags_data_ptr;
    typedef pair<frame_part_flags_data_ptr, size_t> frame_part_flags;
    void alloc_frame_and_part_flags(video_frame& frame, size_t frame_size, size_t max_part_size);
    bool sync_frame_buffers_with_received_frame(unsigned int frame_index, unsigned int frame_size,
                                                size_t max_part_size);
    void handle_frame_completion(video_frame& frame);
    void receive_next_packet();
    void on_async_rx_complete(boost::system::error_code const& error_code, size_t bytes_transferred);
    void on_transmit_ping(boost::system::error_code const& error_code);
    void on_async_tx_complete(boost::system::error_code const& error_code, size_t bytes_transferred);

    void notify_client_when_all_pending_operations_complete();

private:
    mutable lock_type m_lock; // Synchronizes access to this when no lock is provided during construction.

    io_service& m_io_service;
    udp::socket m_socket;
    udp::endpoint m_remote_endpoint;
    deadline_timer m_ping_timer; // Transmits video enable and other periodic data.
    atomic<unsigned int> mi_ping_timer_in_pgrs; //!< 0 < frame receive in progress; 0 = idle.

    video_frame m_video_frame;
    video_frame mv_receiving_frames[2]; //!< Two frames are required to handle packets received out of order on frame boundaries.
    frame_part_flags mv_part_flags[2]; //!< Bit fields.  '1' == part received.
    atomic<bool> mb_frame_rx_enabled; //!< true = frame reception enabled; false = disabled.
    atomic<unsigned int> mi_rx_in_pgrs; //!< 0 < frame receive in progress; 0 = idle.
    unsigned int mi_stream_frame_index; //!< Frame index/counter of first frame in 'mv_receiving_frames'.
    size_t mi_rx_buf_cap; //!< 'mp_rx_buf' capacity (number of bytes addressed by 'mp_rx_buf').
    unique_ptr<uint8_t[]> mp_rx_buf;
    size_t mi_rx_buf_size; //!< Number of bytes [received] in buffer addressed by 'mp_rx_buf'.
    
    uint8_t mv_tx_buf[1];
    size_t mi_tx_buf_size;
    atomic<unsigned int> mi_tx_in_pgrs;

    change_signal_t m_change_publisher;
    operation_complete_signal_t m_operation_complete_publisher;
};


} // namespace video {
} // namespace adapter {


#endif // #ifndef UDP_JPEG_VIDEO_CLIENT_HPP__698B6D1B_88CF_4E49_A781_79799C4B37B7__INCLUDED


/*
    End of "udp_jpeg_video_client.hpp"
*/
