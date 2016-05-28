/*!
    \file "udp_jpeg_video_server.cpp"

    Formatting: 4 spaces/tab, 120 columns.
    Doc-tool: Doxygen (http://www.doxygen.com/)
*/


#include <adapter/video/video_env.hpp>
#include <adapter/video/udp_jpeg_video_server.hpp>


//#define ENABLE_TRACE

#ifdef ENABLE_TRACE
    #define TRACE video_trace
#else
    #define TRACE video_nop
#endif


namespace adapter {
namespace video {


/**
 * Start up the sub-system.
 */
void
udp_jpeg_video_server::startup(string service_name)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    TRACE("==> udp_jpeg_video_server::startup(service_name=\"%s\")\n", static_cast<char const *>(service_name.c_str()));
    
    try
    {
        // Fail when already running.
        if (m_socket.is_open()) { throw already_started_exception(); }

        init_session_attrs();

        // Create server socket.
        m_socket.open(udp::v4());
        m_socket.bind(*udp::resolver(m_io_service).resolve(udp::resolver::query(udp::v4(), service_name)));

        // Start receiving (capture anything/everything that comes back at any time).
        m_socket.async_receive_from(boost::asio::buffer(m_rx_buf), m_remote_endpoint,
                                          boost::bind(&udp_jpeg_video_server::on_async_rx_complete, this,
                                                      boost::asio::placeholders::error,
                                                      boost::asio::placeholders::bytes_transferred));
        ++mi_rx_in_pgrs;

        // Start the timer that will terminate the stream when the peer does not send a keep-alive ping.
        reset_peer_disconnected_timer();

        transmit_next_packet();
    }
    catch (...)
    {
        try { shutdown(); } catch(...) {}
        throw;
    }

    TRACE("<-- udp_jpeg_video_server::startup(service_name=\"%s\")\n", static_cast<char const *>(service_name.c_str()));
    
    LEAVE_SYNC_DOMAIN()
}


/**
 * Shutdown the sub-system.
 */
void
udp_jpeg_video_server::shutdown()
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    try
    {
        try { m_peer_disconnected_timer.cancel(); } catch (...) {} // Ignore errors.
        try { m_socket.close(); } catch (...) {} // Ignore errors.

        if (udp::endpoint() == m_remote_endpoint)
        {
            // Make sure the endpoint is not default initialized so that the completion logic is executed.
            // What it is doesn't matter.
            m_remote_endpoint = udp::endpoint(boost::asio::ip::address(), 0xffff);
        }
        notify_client_when_all_pending_operations_complete();
    }
    catch (...)
    {
        throw;
    }

    LEAVE_SYNC_DOMAIN()
}


bool
udp_jpeg_video_server::is_peer_connected() const
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    return udp::endpoint() != m_remote_endpoint;

    LEAVE_SYNC_DOMAIN()
}


/**
 * @brief Appends one frame to the frame transmission queue.
 * @param frame_data The complete jpeg image to send.
 * @param frame_size_in_bytes The number of bytes addressed by 'frame_data'.
 *
 * This method TAKES OWNERSHIP of the memory addressed by 'frame_data' so the client must
 * not delete or reuse it after calling this method.  It will be deleted by this afer transmission.
 * For example:
 * udp_jpeg_video_server my_serer;
 * size_t frame_size = <whatever>;
 * uint8_t frame_data = new uint8_t [frame_size];
 * <fill in frame data here>
 * my_server::send_frame(udp_jpeg_video_server::frame_data_ptr(frame_data), size);
 * At this point 'my_server' has taken ownership of 'frame_data'.  DO NOT DELETE OR REUSE IT!
 *
 * The frame transmission queue capacity is _never_ greater than two (2).
 * The new frame will be appended to the queue when:
 *   o A frame is already in the queue and it is being transmitted.
 *   o The queue is empty.
 * When the queue already containes two frames, the oldest in the queue will be replaced.
 * When the queue contains only one frame and it is not being transmitted, it will be replaced.
 *
 * Frames will be transmitted IMMEDIATELY when this method is called.
 *
 * Pace calls to this method to avoid saturating the network link,
 * e.g. 12 times per second and only when pending_frame_count() returns false.
 *
 * This will stop transmitting frames when the frame transmission queue becomes empty so the
 * caller of this method must repeatedly call this method with same [last] frame data,
 * when there is no new frame data available, to cause the jpeg video client to continue to
 * receive an image.
 */
void
udp_jpeg_video_server::send_frame(frame_data_ptr frame_data, size_t frame_size_in_bytes)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())
    
    TRACE("==> udp_jpeg_video_server::send_frame(frame_data=%p, frame_size_in_bytes=%u)\n", static_cast<void *>(frame_data.get()), static_cast<unsigned int>(frame_size_in_bytes));

    try
    {
        bool const frame_queue_empty = nullptr == mv_frames[0].mp_frame_data;
        frame_desc_t& frame = mv_frames[frame_queue_empty || (0 == mi_tx_in_pgrs) ? 0 : 1];
        frame = 0 == frame_size_in_bytes ? frame_desc_t() : frame_desc_t(move(frame_data), frame_size_in_bytes);

        TRACE("    udp_jpeg_video_server::send_frame(): mv_frames[0] = %p %u\n", static_cast<void *>(mv_frames[0].mp_frame_data.get()), static_cast<unsigned int>(mv_frames[0].m_frame_data_size));
        TRACE("    udp_jpeg_video_server::send_frame(): mv_frames[1] = %p %u\n", static_cast<void *>(mv_frames[1].mp_frame_data.get()), static_cast<unsigned int>(mv_frames[1].m_frame_data_size));
        
        transmit_next_packet();
    }
    catch (...)
    {
        throw;
    }

    TRACE("<-- udp_jpeg_video_server::send_frame(frame_data=%p, frame_size_in_bytes=%u)\n", static_cast<void *>(frame_data.get()), static_cast<unsigned int>(frame_size_in_bytes));
    
    LEAVE_SYNC_DOMAIN()
}


size_t
udp_jpeg_video_server::pending_frame_count() const
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    return nullptr == mv_frames[0].mp_frame_data ? 0 :
           nullptr == mv_frames[1].mp_frame_data ? (0 != mi_tx_in_pgrs ? 0 : 1) :
           (0 != mi_tx_in_pgrs ? 1 : 2);

    LEAVE_SYNC_DOMAIN()
}


void
udp_jpeg_video_server::clear_frames()
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    mv_frames[1] = frame_desc_t();
    if (nullptr != mv_frames[0].mp_frame_data && 0 == mi_tx_in_pgrs)
    {
        mv_frames[0] = frame_desc_t();
        mi_stream_frame_index = mi_stream_frame_index % 0xff + 1; // Zero is a sentinel value.
    }
    if (0 == mi_tx_in_pgrs && !frame_transmission_enabled()) { mi_stream_frame_part_index = 0; }

    LEAVE_SYNC_DOMAIN()
}


void
udp_jpeg_video_server::init_session_attrs()
{
    mi_peer_disconnected_timer_in_pgrs = 0;
    mi_tx_in_pgrs = 0;
    mi_stream_frame_index = 1;
    mi_stream_frame_part_index = 0;
    size_t const new_tx_buf_cap = 1500; // Note: UDP MTU = 576 bytes; IP MTU is 1500 bytes.
    if (new_tx_buf_cap != mi_tx_buf_cap)
    {
        mi_tx_buf_cap = new_tx_buf_cap;
        try { mp_tx_buf.reset(new uint8_t [mi_tx_buf_cap]); }
        catch (...) { mi_tx_buf_cap = 0; throw; }
    }
    mi_rx_in_pgrs = 0;
}


void
udp_jpeg_video_server::transmit_next_packet()
{
    TRACE("==> udp_jpeg_video_server::transmit_next_packet()\n");
    
    // Do nothing when:
    bool const frame_queue_empty = nullptr == mv_frames[0].mp_frame_data;
    if (0 != mi_tx_in_pgrs            ||  // a transmission is already in progress, or
        !handle_peer_disconnect()     ||  // no remote connection (peer)
        !frame_transmission_enabled() ||  // transmission is disabled, or
        frame_queue_empty              )  // no frames are avilable for transmission
    {
        if (frame_queue_empty &&
            !frame_transmission_enabled())
        {
            mi_stream_frame_part_index = 0;
        }
        notify_client_when_all_pending_operations_complete();
        TRACE("<-- udp_jpeg_video_server::transmit_next_packet(): nothing to do\n");
        return;
    }

    frame_desc_t& frame = mv_frames[0];

    // Construct the next packet to send.
    /*
        packet format (rev 1; 10 bytes):
             [0] prot_rev      : uint8_t
             [1] frame_index   : uint8_t
             [2] part_index    : uint16_t
             [4] max_part_size : uint16_t  (multiply by 'part_index' to find part index in frame data)
             [6] frame_size    : uint32_t
            [10] data          : <up to mi_tx_buf_size-8 bytes>
    */
    size_t const pkt_hdr_size = 10;
    size_t const max_pyld_size = mi_tx_buf_cap - pkt_hdr_size;
    memset(mp_tx_buf.get(), 0, mi_tx_buf_cap); // Superfluous; useful for debugging.

    mp_tx_buf[0] = 1; // 'proto_rev' field.

    mp_tx_buf[1] = mi_stream_frame_index & 0xff; // 'frame_index' field.

    uint16_t uint16_val = 0;
    uint32_t uint32_val = 0;

    uint16_val = htons(static_cast<uint16_t>(mi_stream_frame_part_index));
    memcpy(mp_tx_buf.get() + 2, &uint16_val, sizeof(uint16_val)); // 'part_index' field.

    uint16_val = htons(static_cast<uint16_t>(max_pyld_size));
    memcpy(mp_tx_buf.get() + 4, &uint16_val, sizeof(uint16_val)); // 'max_part_size' field.

    uint32_val = htonl(static_cast<uint32_t>(frame.m_frame_data_size));
    memcpy(mp_tx_buf.get() + 6, &uint32_val, sizeof(uint32_val)); // 'frame_size' field (in bytes).

    // 'data' field.
    size_t const pyld_amount_sent = mi_stream_frame_part_index * max_pyld_size;
    size_t const tx_buf_pyld_size = (min)(frame.m_frame_data_size - pyld_amount_sent, max_pyld_size);
    memcpy(mp_tx_buf.get() + pkt_hdr_size,
           frame.mp_frame_data.get() + pyld_amount_sent,
           tx_buf_pyld_size);

    // Start the asynchrnous send operation.
    size_t const pkt_size = pkt_hdr_size + tx_buf_pyld_size;
    m_socket.async_send_to(boost::asio::buffer(mp_tx_buf.get(), pkt_size), m_remote_endpoint,
                           boost::bind(&udp_jpeg_video_server::on_async_tx_complete, this,
                                       boost::asio::placeholders::error,
                                       boost::asio::placeholders::bytes_transferred));
    ++mi_tx_in_pgrs;

    if (0 == mi_stream_frame_part_index) { TRACE("\n"); TRACE("    ==============================================================================\n"); }
    TRACE("    udp_jpeg_video_server::transmit_next_packet(): sent f-idx(%u) p-idx(%u) pyldsize(%u) jpgsize(%lu)\n",
          static_cast<unsigned int>(mi_stream_frame_index), static_cast<unsigned int>(mi_stream_frame_part_index),
          static_cast<unsigned int>(tx_buf_pyld_size), static_cast<unsigned long>(frame.m_frame_data_size));
    if (0 == mi_stream_frame_part_index) { TRACE("    ==============================================================================\n"); }

    // Advance to the next frame part (if any).
    size_t const pyld_amount_remaining = frame.m_frame_data_size - pyld_amount_sent - tx_buf_pyld_size;
    if (0 < pyld_amount_remaining)
    {
        ++mi_stream_frame_part_index;
    }
    else
    {
        mi_stream_frame_part_index = 0;

        mi_stream_frame_index = (mi_stream_frame_index + 1) % 0xff; // Zero is a sentinel value.
        mv_frames[0] = move(mv_frames[1]);
        mv_frames[1] = frame_desc_t();
        
        TRACE("    udp_jpeg_video_server::transmit_next_packet(): end of frame reached\n");
    }
    
    TRACE("<-- udp_jpeg_video_server::transmit_next_packet()\n");
}


void
udp_jpeg_video_server::on_async_tx_complete(boost::system::error_code const& error_code,
                                            size_t bytes_transferred)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    TRACE("==> udp_jpeg_video_server::on_async_tx_complete(error_code=%d, bytes_transferred=%u)\n",
          static_cast<int>(error_code.value()), static_cast<unsigned int>(bytes_transferred));
    
    try
    {
        --mi_tx_in_pgrs;
        transmit_next_packet();
    }
    catch (...)
    {
        // Do nothing.
    }

    TRACE("<-- udp_jpeg_video_server::on_async_tx_complete(error_code=%d, bytes_transferred=%u)\n",
          static_cast<int>(error_code.value()), static_cast<unsigned int>(bytes_transferred));

    LEAVE_SYNC_DOMAIN()
}


void
udp_jpeg_video_server::on_async_rx_complete(boost::system::error_code const& error_code, size_t bytes_transferred)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    TRACE("==> udp_jpeg_video_server::on_async_rx_complete(error_code=%d, bytes_transferred=%u)\n",
          static_cast<int>(error_code.value()), static_cast<unsigned int>(bytes_transferred));

    try
    {
        --mi_rx_in_pgrs;

        // Handle transfer cancellation.
        bool subsystem_running = m_socket.is_open();
        if (!subsystem_running)
        {
            notify_client_when_all_pending_operations_complete();
            return;
        }

        if (boost::system::errc::success == error_code.value() &&
            0 == mi_rx_in_pgrs)
        {
            reset_peer_disconnected_timer();

            // The message content is currently being ignored since it is unimportant.
            // The receive is used only to obtain remote endpoint information.
            if (0 < bytes_transferred)
            {
                bool const enable_video = 0 != m_rx_buf[0];
                if (!enable_video)
                {
                    // Disconnect from peer.
                    // It would be better to track enable/disable set by peer and weave it into the video
                    // enable/disable state, but this is good enough for now because the only thing this
                    // does is send video frames.  There is no other communication that requires an endpoint.
                    m_remote_endpoint = udp::endpoint(); // Disconnect from peer.
                    handle_peer_disconnect();
                }
            }

            // Start receiving again.
            m_socket.async_receive_from(boost::asio::buffer(m_rx_buf), m_remote_endpoint,
                                        boost::bind(&udp_jpeg_video_server::on_async_rx_complete, this,
                                                    boost::asio::placeholders::error,
                                                    boost::asio::placeholders::bytes_transferred));
            ++mi_rx_in_pgrs;

            transmit_next_packet();
        }
    }
    catch (...)
    {
        // Do nothing.
    }

    TRACE("<-- udp_jpeg_video_server::on_async_rx_complete(error_code=%d, bytes_transferred=%u)\n",
          static_cast<int>(error_code.value()), static_cast<unsigned int>(bytes_transferred));

    LEAVE_SYNC_DOMAIN()
}


void
udp_jpeg_video_server::reset_peer_disconnected_timer()
{
    bool subsystem_running = m_socket.is_open();
    if (subsystem_running)
    {
        m_peer_disconnected_timer.expires_from_now(boost::posix_time::seconds(2)); // Make this longer than ping frequency!
        m_peer_disconnected_timer.async_wait(boost::bind(&udp_jpeg_video_server::on_peer_disconnected_timer, this, _1));
        ++mi_peer_disconnected_timer_in_pgrs;
    }
}


void
udp_jpeg_video_server::on_peer_disconnected_timer(boost::system::error_code const& error_code)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    TRACE("<-- udp_jpeg_video_server::on_peer_disconnected_timer(error_code=%d)\n",
          static_cast<int>(error_code.value()));

    --mi_peer_disconnected_timer_in_pgrs;
    TRACE("<-- udp_jpeg_video_server::on_peer_disconnected_timer(): --mi_peer_disconnected_timer_in_pgrs=%u\n",
          static_cast<unsigned int>(mi_peer_disconnected_timer_in_pgrs));

    bool subsystem_running = m_socket.is_open();
    bool const timer_canceled = make_error_code(boost::asio::error::operation_aborted) == error_code;
    bool const timeout = m_peer_disconnected_timer.expires_at() <= deadline_timer::traits_type::now();
    if (subsystem_running &&
        !timer_canceled &&
        timeout)
    {
        m_remote_endpoint = udp::endpoint(); // Disconnect from peer.
        handle_peer_disconnect();
    }

    notify_client_when_all_pending_operations_complete();

    TRACE("<-- udp_jpeg_video_server::on_peer_disconnected_timer(error_code=%d)\n",
          static_cast<int>(error_code.value()));

    LEAVE_SYNC_DOMAIN()
}


/**
 * @brief Detects when the peer is disconnected and stops the video stream.
 * @return true = peer connected; false = peer not connected.
 */
bool
udp_jpeg_video_server::handle_peer_disconnect()
{
    bool const peer_connected = is_peer_connected();

    if (!peer_connected)
    {
        clear_frames();
    }

    return peer_connected;
}


void
udp_jpeg_video_server::notify_client_when_all_pending_operations_complete()
{
    if (0 == mi_peer_disconnected_timer_in_pgrs &&
        0 == mi_rx_in_pgrs &&
        0 == mi_tx_in_pgrs &&
        udp::endpoint() != m_remote_endpoint)
    {
        TRACE("udp_jpeg_video_server::notify_client_when_all_pending_operations_complete()\n");

        try { m_remote_endpoint = udp::endpoint(); } catch (...) {} // Ignore errors.
        for (size_t idx = 0; arycap(mv_frames) > idx; ++idx)
        {
            try { mv_frames[idx] = frame_desc_t(); } catch (...) {} // Ignore errors.
        }
        mi_stream_frame_part_index = 0;
        mi_tx_buf_cap = 0;
        try { mp_tx_buf.reset(); } catch (...) {} // Ignore errors.

        // Notify subscribers that the operation is complete.
        // WARNING: Do not use io_service::dispatch() or the callback may be invoked while
        //          within the synchronization domain, i.e. this is locked!
        m_io_service.post(boost::bind<void>([](udp_jpeg_video_server *thiz){
            thiz->m_operation_complete_publisher(*thiz);
        }, this));
    }
}


udp_jpeg_video_server::udp_jpeg_video_server(io_service& io_service, lock_type *sync_domain_lock)
    : sync_domain_provider(sync_domain_lock)
    , m_io_service(io_service)
    , m_socket(m_io_service)
    , m_peer_disconnected_timer(m_io_service)
    , mb_frame_tx_enabled(true)
    , mi_tx_buf_cap(0)
    , mp_tx_buf(nullptr)
{
    if (nullptr == sync_domain_lock) { set_sync_domain_lock(m_lock); }
    init_session_attrs();
}


udp_jpeg_video_server::~udp_jpeg_video_server()
{
    // Do nothing.
}


} // namespace video {
} // namespace adapter {


/*
    End of "udp_jpeg_video_server.cpp"
*/
