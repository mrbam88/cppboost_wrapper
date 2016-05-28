/*!
    \file "udp_jpeg_video_client.cpp"

    Formatting: 4 spaces/tab, 120 columns.
    Doc-tool: Doxygen (http://www.doxygen.com/)
*/


#include <adapter/video/video_env.hpp>
#include <adapter/video/udp_jpeg_video_client.hpp>


//#define ENABLE_TRACE
//#define ENABLE_TRACE_ASYNC_OP_CNTR

#ifdef ENABLE_TRACE
    #define TRACE video_trace
#else
    #define TRACE video_nop
#endif

#ifdef ENABLE_TRACE_ASYNC_OP_CNTR
    #define TRACE_ASYNC_OP_CNTR video_trace
#else
    #define TRACE_ASYNC_OP_CNTR video_nop
#endif


namespace adapter {
namespace video {


/**
 * Start up the sub-system.
 */
void
udp_jpeg_video_client::startup(string host_name, string service_name)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    try
    {
        // Fail when already running.
        if (is_running()) { throw already_started_exception(); }

        init_session_attrs();
        
        // printf("udp_jpeg_video_client::startup --> socket opened \n");
        
        // Create client socket.
        m_socket.open(udp::v4());
        m_remote_endpoint = *udp::resolver(m_io_service).resolve(udp::resolver::query(udp::v4(), host_name, service_name));

        // Start periodic ping transmission timer.
        m_ping_timer.expires_at(deadline_timer::traits_type::now() + boost::posix_time::seconds(1));
        m_ping_timer.async_wait(boost::bind(&udp_jpeg_video_client::on_transmit_ping, this, _1));
        ++mi_ping_timer_in_pgrs;
        TRACE_ASYNC_OP_CNTR("udp_jpeg_video_client::startup(): ++mi_ping_timer_in_pgrs = %lu (%s[%lu])\n", static_cast<unsigned long>(mi_ping_timer_in_pgrs), __FILE__, static_cast<unsigned long>(__LINE__));
        
        receive_next_packet();
    }
    catch (...)
    {
        try { shutdown(); } catch(...) {}
        throw;
    }

    LEAVE_SYNC_DOMAIN()
}


/**
 * Shutdown the sub-system.
 */
void
udp_jpeg_video_client::shutdown()
{
   // printf("upd_jpeg_video_client:: shutdown --> Enter \n");
    ENTER_SYNC_DOMAIN(sync_domain_lock())
    
    try { m_ping_timer.cancel(); } catch (...) {} // Ignore errors.
    try { m_socket.close(); } catch (...) {} // Ignore errors.
   // printf("upd_jpeg_video_client:: shutdown --> Socket closed  \n");
    if (udp::endpoint() == m_remote_endpoint)
    {
        // Make sure the endpoint is not default initialized so that the completion logic is executed.
        // What it is doesn't matter.
        m_remote_endpoint = udp::endpoint(boost::asio::ip::address(), 0xffff);
    }
  //  printf("upd_jpeg_video_client:: shutdown --> notify complete  \n");
    notify_client_when_all_pending_operations_complete();

    LEAVE_SYNC_DOMAIN()
    
    
  //  printf("upd_jpeg_video_client:: shutdown --> Finished \n");
}


bool
udp_jpeg_video_client::is_running() const
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    return m_socket.is_open() || udp::endpoint() != m_remote_endpoint;

    LEAVE_SYNC_DOMAIN()
}


/**
 * @brief Retrieve pending video frame (if any).
 * @return Video frame data or video_frame() if none.
 *
 * This method RELINQUISHES OWNERSHIP of the returned data,
 * which is managed by a smart pointer within 'video_frame'.
 *
 * The frame receive queue capacity is never greater than one (1).
 * A newly received frame will replace an already pending frame, if any.
 */
udp_jpeg_video_client::video_frame
udp_jpeg_video_client::get_frame()
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    try
    {
        video_frame result(move(m_video_frame));
        m_video_frame = video_frame();
        return move(result);
    }
    catch (...)
    {
        throw;
    }

    LEAVE_SYNC_DOMAIN()
}


bool
udp_jpeg_video_client::is_frame_pending() const
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    return video_frame() != m_video_frame;

    LEAVE_SYNC_DOMAIN()
}


void
udp_jpeg_video_client::clear_frames()
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    m_video_frame = video_frame();

    LEAVE_SYNC_DOMAIN()
}


bool
udp_jpeg_video_client::enable_frame_reception(bool value)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    if (mb_frame_rx_enabled != value)
    {
        mb_frame_rx_enabled = value;

        if (!mb_frame_rx_enabled)
        {
            for (size_t idx = 0 ; arycap(mv_receiving_frames) > idx; ++idx) { mv_receiving_frames[idx] = video_frame(); }
            for (size_t idx = 0 ; arycap(mv_part_flags) > idx; ++idx) { mv_part_flags[idx] = frame_part_flags(); }
            mi_stream_frame_index = 0; // Zero is a sentinel value (it's never received).

//!\todo >>> Cancel read operation. <<<

        }
    }

    return mb_frame_rx_enabled;

    LEAVE_SYNC_DOMAIN()
}


void
udp_jpeg_video_client::init_session_attrs()
{
    mi_ping_timer_in_pgrs = 0;
    mi_rx_in_pgrs = 0;
    mi_tx_in_pgrs = 0;
    mi_stream_frame_index = 0; // Zero is a sentinel value (it's never received).
    size_t const new_rx_buf_cap = 1500; // Note: UDP MTU = 576 bytes; IP MTU is 1500 bytes.
    if (new_rx_buf_cap != mi_rx_buf_cap)
    {
        mi_rx_buf_cap = new_rx_buf_cap;
        try { mp_rx_buf.reset(new uint8_t [mi_rx_buf_cap]); }
        catch (...) { mi_rx_buf_cap = 0; throw; }
    }
    mi_rx_buf_size = 0;
    mi_tx_buf_size = 0;
}


/**
 * @brief Allocates or reallocates frame data and part flags to support 'frame_size'.
 * @param frame The frame to [re]alloc, if necessary.
 * @param part_flags The part flags to [re]alloc, if necessary.
 * @param frame_size The new capacity (in bytes).
 * @param max_part_size The maximum size of a part (in bytes).
 *
 * Frame data and part flags are only increased in size.  The memory is never reduced.
 * This function will *not* correctly reduce the frame size!  Do not use it for that!
 * Calling this function to reduce the frame size will result in image artifacts and
 * possibly broken frame part flags!
 */
void
udp_jpeg_video_client::alloc_frame_and_part_flags(video_frame& frame, size_t frame_size, size_t max_part_size)
{
    // Do nothing when no [re]alloc is needed.
    bool const frame_alloc_needed = nullptr == frame.first.get() || frame_size > frame.second;
    if (!frame_alloc_needed) { return; }

    // Allocate the frame.
    video_frame new_frame(frame_data_ptr(new uint8_t [frame_size]), frame_size);
    if (nullptr != frame.first)
    {
        // Copy existing frame data to new frame and zero out new space.
        memcpy(new_frame.first.get(), frame.first.get(), (std::min)(new_frame.second, frame.second));
        memset(new_frame.first.get() + frame.second, 0, new_frame.second - frame.second);
    }
    else
    {
        // Zero out new space.
        memset(new_frame.first.get(), 0, new_frame.second);
    }

    // Allocate the part flags.
    WMASSERT(2 <= arycap(mv_part_flags));
    frame_part_flags& part_flags = mv_part_flags[&mv_receiving_frames[0] == &frame ? 0 : 1];
    size_t const total_part_count = frame_size / max_part_size + (0 == frame_size % max_part_size ? 0 : 1);
    size_t const new_part_flags_cap = (total_part_count >> 3) + (0 == (total_part_count & 0x7) ? 0 : 1);
    frame_part_flags new_part_flags(frame_part_flags_data_ptr(new uint8_t [new_part_flags_cap]), new_part_flags_cap);
    if (nullptr != part_flags.first)
    {
        // Copy existing flags (ignoring the ones set on the end).
        memcpy(new_part_flags.first.get(), part_flags.first.get(),
               (std::min)(new_part_flags.second, part_flags.second));

        // Clear unused part flags from existing part flags, if any.
        size_t flag_count = part_flags.second << 3;
        for (size_t flag_idx = part_flags.second; flag_count > flag_idx; ++flag_idx)
        {
            new_part_flags.first[flag_idx >> 3] &= ~(1 << (flag_idx & 0x7));
        }
    }
    else
    {
        // Set all flags to zero.
        memset(new_part_flags.first.get(), 0, new_part_flags.second);
    }

    // Set unused part flags to '1' for frame completion test logic.
    size_t new_flag_count = new_part_flags.second << 3;
    for (size_t flag_idx = total_part_count; new_flag_count > flag_idx; ++flag_idx)
    {
        new_part_flags.first[flag_idx >> 3] |= 1 << (flag_idx & 0x7);
    }

    // Make new frame and part flags current.
    frame = move(new_frame);
    part_flags = move(new_part_flags);
}


/**
 * @brief Synchronies frame receive buffers with new frame information.
 * @param frame_index Stream frame index of newly received frame part.
 * @param frame_size Size (in bytes) of newly received frame part.
 * @param max_part_size The maximum size of a part (in bytes).
 * @return True = keep frame, it's good; false = frame is bad (old), reject it.
 *
 * This method will discard data in frame buffers that appear to be stale (old),
 * which can/will happen with packet loss.  This method also [re]allocates new frame
 * buffers as necessary.
 */
bool
udp_jpeg_video_client::sync_frame_buffers_with_received_frame(unsigned int frame_index, unsigned int frame_size,
                                                              size_t max_part_size)
{
    unsigned int frame_delta = (frame_index - mi_stream_frame_index) & 0xff;
    unsigned int old_frames_delta = 256-25;
    bool frame_is_old = frame_delta >= old_frames_delta;
    if (frame_is_old) { return false; }

    bool const frame_index_is_valid = frame_index == mi_stream_frame_index ||
                                      (frame_index == mi_stream_frame_index + 1 &&
                                       nullptr != mv_receiving_frames[0].first);
    if (frame_index_is_valid)
    {
        size_t relative_idx = frame_index == mi_stream_frame_index ? 0 : 1;
        WMASSERT(arycap(mv_receiving_frames) > relative_idx);
        WMASSERT(arycap(mv_part_flags) > relative_idx);
        video_frame& frame = mv_receiving_frames[relative_idx];
        alloc_frame_and_part_flags(frame, frame_size, max_part_size);
    }
    else
    {
        // Discard existing frames (assume they're stale).
        // Since frame indexes always advance, it's safe to assume that the frames that
        // were being received are now stale and were missed (probably due to packet loss).
        WMASSERT(1 <= arycap(mv_receiving_frames));
        WMASSERT(1 <= arycap(mv_part_flags));
        mv_receiving_frames[0] = video_frame();
        mv_part_flags[0] = frame_part_flags();

        WMASSERT(2 <= arycap(mv_receiving_frames));
        WMASSERT(2 <= arycap(mv_part_flags));
        mv_receiving_frames[1] = video_frame();
        mv_part_flags[1] = frame_part_flags();

        mi_stream_frame_index = frame_index;

        alloc_frame_and_part_flags(mv_receiving_frames[0], frame_size, max_part_size);
    }
    
    return true;
}


/**
 * @brief Handle frame completion (all parts received).
 * @param frame Frame to test for completion.
 * @param part_flags Frame parts flags associated with 'frame'.
 */
void
udp_jpeg_video_client::handle_frame_completion(video_frame& frame)
{
    WMASSERT(2 <= arycap(mv_part_flags));
    frame_part_flags& part_flags = mv_part_flags[&mv_receiving_frames[0] == &frame ? 0 : 1];

    // Do nothing when all frame part flags are *not* set.
    size_t idx = 0;
    for (idx = 0; part_flags.second > idx && 0xff == part_flags.first[idx]; ++idx) {}
    bool frame_incomplete = part_flags.second != idx;
    if (frame_incomplete) { return; }

    // Move frame from 'mv_receiving_frames' to 'm_video_frame'.
    m_video_frame = move(frame);
    frame = video_frame();
    part_flags = frame_part_flags();

    // Advance to the next frame (that is expected).
    if (&mv_receiving_frames[0] == &frame)
    {
        WMASSERT(2 <= arycap(mv_receiving_frames));
        mv_receiving_frames[0] = move(mv_receiving_frames[1]);
        mv_receiving_frames[1] = video_frame();

        ++mi_stream_frame_index;
    }
    else
    {
        // Discard previous frame (it's old and stale now).
        WMASSERT(1 <= arycap(mv_receiving_frames));
        mv_receiving_frames[0] = video_frame();
        mv_part_flags[0] = frame_part_flags();

        mi_stream_frame_index += 2;
    }
    
    // Notify subscribers.
    m_io_service.post(boost::bind<void>([](udp_jpeg_video_client& host){ host.m_change_publisher(host); }, ref(*this)));
}


void
udp_jpeg_video_client::receive_next_packet()
{
    // Do nothing when:
    if (0 < mi_rx_in_pgrs    || // a receive is already in progress, or
        !mb_frame_rx_enabled  ) // receiving is disabled
    {
        return;
    }

    // Process received data.
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
    bool const pkt_length_error = pkt_hdr_size > mi_rx_buf_size;
    if (!pkt_length_error)
    {
        unsigned int const proto_rev = mp_rx_buf[0]; // 'proto_rev' field.
        if (1 == proto_rev)
        {
            unsigned int const frame_index = mp_rx_buf[1]; // 'frame_index' field.

            uint16_t uint16_val = 0;
            uint32_t uint32_val = 0;

            uint16_val = 0;
            memcpy(&uint16_val, mp_rx_buf.get() + 2, sizeof(uint16_val));
            unsigned int const part_index = ntohs(uint16_val); // 'part_index' field.

            uint16_val = 0;
            memcpy(&uint16_val, mp_rx_buf.get() + 4, sizeof(uint16_val));
            unsigned int const max_part_size = ntohs(uint16_val); // 'max_part_size' field.

            uint32_val = 0;
            memcpy(&uint32_val, mp_rx_buf.get() + 6, sizeof(uint32_val));
            unsigned int const frame_size = ntohl(uint32_val); // 'frame_size' field (in bytes).

            unsigned int divisor = 0 != part_index + 1 ? part_index + 1 : 1;
            bool const part_index_error = numeric_limits<unsigned int>::max() / divisor < max_part_size ||
                                          part_index * max_part_size >= frame_size;
            if (!part_index_error)
            {
                uint8_t const *part_data = mp_rx_buf.get() + pkt_hdr_size;
                size_t const part_data_size = mi_rx_buf_size - pkt_hdr_size;

                if (sync_frame_buffers_with_received_frame(frame_index, frame_size, max_part_size))
                {
                    size_t relative_idx = frame_index == mi_stream_frame_index ? 0 : 1;
                    WMASSERT(arycap(mv_receiving_frames) > relative_idx);
                    video_frame& frame = mv_receiving_frames[relative_idx];
                    WMASSERT(arycap(mv_part_flags) > relative_idx);
                    frame_part_flags& part_flags = mv_part_flags[relative_idx];

                    // Copy part data into frame buffer.
                    memcpy(frame.first.get() + part_index * max_part_size, part_data, part_data_size);

                    // Set flag indicating that part was received.
                    WMASSERT(part_flags.second > part_index >> 3);
                    part_flags.first[part_index >> 3] |= 1 << (part_index & 0x7);

                    handle_frame_completion(frame);
                }
            }
        }
    }

    // Continue receiving, when enabled.
    m_socket.async_receive_from(boost::asio::buffer(mp_rx_buf.get(), mi_rx_buf_cap), m_remote_endpoint,
                                      boost::bind(&udp_jpeg_video_client::on_async_rx_complete, this,
                                                  boost::asio::placeholders::error,
                                                  boost::asio::placeholders::bytes_transferred));
    ++mi_rx_in_pgrs;
}


void
udp_jpeg_video_client::on_async_rx_complete(boost::system::error_code const& error_code, size_t bytes_transferred)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    TRACE("udp_jpeg_video_client::on_async_rx_complete(error_code = %d, bytes_transferred = %u)\n", static_cast<int>(error_code.value()), static_cast<int>(bytes_transferred));
    
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

        if (0 < bytes_transferred)
        {
            mi_rx_buf_size = bytes_transferred;
            receive_next_packet();
        }
    }
    catch (...)
    {
        // Do nothing.
    }

    LEAVE_SYNC_DOMAIN()
}


void
udp_jpeg_video_client::on_transmit_ping(boost::system::error_code const& error_code)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    --mi_ping_timer_in_pgrs;
    TRACE_ASYNC_OP_CNTR("udp_jpeg_video_client::on_transmit_ping(): ++mi_ping_timer_in_pgrs = %lu (%s[%lu])\n", static_cast<unsigned long>(mi_ping_timer_in_pgrs), __FILE__, static_cast<unsigned long>(__LINE__));

    bool subsystem_running = m_socket.is_open();
    bool const bTimerCanceled = boost::asio::error::operation_aborted == error_code.value();
    bool const bTimeout = deadline_timer::traits_type::now() >= m_ping_timer.expires_at();
    if (subsystem_running &&
        !bTimerCanceled &&
        bTimeout)
    {
        // Do nothing when a transmission is in progress.  A future timer tick will keep things going.
        if (0 == mi_tx_in_pgrs)
        {
            // Create transmission data in transmission buffer.
            mv_tx_buf[0] = frame_reception_enabled() ? 1 : 0;
            mi_tx_buf_size = 1;
            
            // Start async transmission.
            m_socket.async_send_to(boost::asio::buffer(mv_tx_buf, mi_tx_buf_size), m_remote_endpoint,
                                   boost::bind(&udp_jpeg_video_client::on_async_tx_complete, this,
                                               boost::asio::placeholders::error,
                                               boost::asio::placeholders::bytes_transferred));
            ++mi_tx_in_pgrs;
        }

        // Restart the timer.
        m_ping_timer.expires_at(deadline_timer::traits_type::now() + boost::posix_time::seconds(1));
        m_ping_timer.async_wait(boost::bind(&udp_jpeg_video_client::on_transmit_ping, this, _1));
        ++mi_ping_timer_in_pgrs;
        TRACE_ASYNC_OP_CNTR("udp_jpeg_video_client::on_transmit_ping(): ++mi_ping_timer_in_pgrs = %lu (%s[%lu])\n", static_cast<unsigned long>(mi_ping_timer_in_pgrs), __FILE__, static_cast<unsigned long>(__LINE__));
    }
    
    notify_client_when_all_pending_operations_complete();

    LEAVE_SYNC_DOMAIN()
}


void
udp_jpeg_video_client::on_async_tx_complete(boost::system::error_code const& error_code, size_t bytes_transferred)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    TRACE("udp_jpeg_video_client::on_async_tx_complete(error_code = %d, bytes_transferred = %u)\n", static_cast<int>(error_code.value()), static_cast<int>(bytes_transferred));

    --mi_tx_in_pgrs;
    TRACE_ASYNC_OP_CNTR("udp_jpeg_video_client::on_async_tx_complete(): --mi_tx_in_pgrs = %lu (%s[%lu])\n", static_cast<unsigned long>(mi_tx_in_pgrs), __FILE__, static_cast<unsigned long>(__LINE__));

    // Handle transfer cancellation.
    bool subsystem_running = m_socket.is_open();
    if (!subsystem_running)
    {
        notify_client_when_all_pending_operations_complete();
        return;
    }

    LEAVE_SYNC_DOMAIN()
}


void
udp_jpeg_video_client::notify_client_when_all_pending_operations_complete()
{
    if (0 == mi_ping_timer_in_pgrs &&
        0 == mi_rx_in_pgrs &&
        0 == mi_tx_in_pgrs &&
        udp::endpoint() != m_remote_endpoint)
    {
        TRACE("udp_jpeg_video_client::notify_client_when_all_pending_operations_complete()\n");

        try { m_remote_endpoint = udp::endpoint(); } catch (...) {} // Ignore errors; sentinel (indicates when transfer is in progress).
        m_video_frame = video_frame();
        for (size_t idx = 0 ; arycap(mv_receiving_frames) > idx; ++idx) { mv_receiving_frames[idx] = video_frame(); }
        mi_rx_buf_cap = 0;
        try { mp_rx_buf.reset(); } catch (...) {} // Ignore errors.
        mi_rx_buf_size = 0;

        // Notify subscribers that the operation is complete.
        // WARNING: Do not use io_service::dispatch() or the callback may be invoked while
        //          within the synchronization domain, i.e. this is locked!
        m_io_service.post(boost::bind<void>([](udp_jpeg_video_client *thiz){
            thiz->m_operation_complete_publisher(*thiz);
        }, this));
    }
}


udp_jpeg_video_client::udp_jpeg_video_client(io_service& io_service, lock_type *sync_domain_lock)
    : sync_domain_provider(sync_domain_lock)
    , m_io_service(io_service)
    , m_socket(m_io_service)
    , m_ping_timer(m_io_service)
    , mb_frame_rx_enabled(true)
    , mi_rx_buf_cap(0)
    , mp_rx_buf(nullptr)
{
    if (nullptr == sync_domain_lock) { set_sync_domain_lock(m_lock); }
    init_session_attrs();
}


udp_jpeg_video_client::~udp_jpeg_video_client()
{
    // Do nothing.
}


} // namespace video {
} // namespace adapter {


/*
    End of "udp_jpeg_video_client.cpp"
*/
