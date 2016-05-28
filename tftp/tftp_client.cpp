/*!
    \file "tftp_client.cpp"

    Formatting: 4 spaces/tab, 120 columns.
    Doc-tool: Doxygen (http://www.doxygen.com/)
*/


#include <adapter/tftp/tftp_env.hpp>
#include <adapter/tftp/tftp_client.hpp>


//#define ENABLE_TRACE
//#define ENABLE_TRACE_ASYNC_OP_CNTR

#ifdef ENABLE_TRACE
    #define TRACE tftp_trace
#else
    #define TRACE tftp_nop
#endif

#ifdef ENABLE_TRACE_ASYNC_OP_CNTR
    #define TRACE_ASYNC_OP_CNTR tftp_trace
#else
    #define TRACE_ASYNC_OP_CNTR tftp_nop
#endif


namespace adapter {
namespace tftp {


#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wunused-const-variable" // Prevent "unused variable..." warning.

uint constexpr TFTP_OPCODE_RRQ   = 1; // Read request == get == download file
uint constexpr TFTP_OPCODE_WRQ   = 2; // Write request == put == upload file
uint constexpr TFTP_OPCODE_DATA  = 3; // Data block of file being transferred.
uint constexpr TFTP_OPCODE_ACK   = 4; // Acknowledgement.
uint constexpr TFTP_OPCODE_ERROR = 5; // Error.
uint constexpr TFTP_OPCODE_OACK  = 6; // Options acknowledgement.

uint constexpr TFTP_ERROR_UNDEFINED = 0; // Not defined, see error message (if any).
uint constexpr TFTP_ERROR_FILE_NOT_FOUND = 1; // File not found.
uint constexpr TFTP_ERROR_ACCESS_VIOLATION = 2; // Access violation.
uint constexpr TFTP_ERROR_DISK_FULL = 3; // Disk full or allocation exceeded.
uint constexpr TFTP_ERROR_INVALID_OPCODE = 4; // Illegal TFTP operation.
uint constexpr TFTP_ERROR_UNKNOWN_XFER_ID = 5; // Unknown transfer ID.
uint constexpr TFTP_ERROR_FILE_EXISTS = 6; // File already exists.
uint constexpr TFTP_ERROR_NO_SUCH_USER = 7; // No such user.
uint constexpr TFTP_ERROR_OPTION = 8; // Unknown or invalid TFTP option.

#pragma GCC diagnostic pop

static char const l_tftp_option_name_block_size[] = "blksize";
static char const l_tftp_option_name_transfer_size[] = "tsize";
static char const l_tftp_option_name_timeout[] = "timeout";


void
tftp_client::untyped_get(string host_name, string service_name,
                         string remote_pathname, string local_pathname,
                         startup_callback_ptr_type callback_ptr)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    printf("tftp_client::get(host_name=%s, service_name=%s, remote_pathname=\"%s\", local_pathname=\"%s\")\n",
          host_name.c_str(), service_name.c_str(), remote_pathname.c_str(), local_pathname.c_str());

    // Ensure that an in-progress operation is not interrupted or destroyed.
    if (in_progress()) { throw operation_in_progress_exception(); }

    try
    {
        startup_callback_ = callback_ptr;
        mi_async_io_timer_in_pgrs = 0;
        mi_async_rx_in_pgrs = 0;
        mi_async_tx_in_pgrs = 0;
        mi_expected_block_number = 1;
        m_transfer_status = make_error_code(boost::system::errc::operation_in_progress);

        // Create client socket.
        m_socket.open(udp::v4());
        udp::resolver::iterator endpoint_iter =
            udp::resolver(m_io_service).resolve(udp::resolver::query(udp::v4(), host_name, service_name));
        if (udp::resolver::iterator() == endpoint_iter) { throw unresolved_endpoint_exception(); }
        m_remote_endpoint = *endpoint_iter; // Sentinel; indicates when transfer is in progress.

        // Initialize receiver.
        m_receiver.set_socket(m_socket);

        // Open local file.
        m_local_file.close(); // Failsafe.
        m_local_file.clear(); // Superfluous for c++11, not for c++98.
        m_local_file.open(local_pathname, ios_base::out | ios_base::binary | ios_base::trunc);
        if (m_local_file.fail()) { throw local_file_exception(); }

        m_remote_pathname = remote_pathname;

        // Start first operation.
        m_io_service.dispatch(boost::bind(&tftp_client::tx_read_rqst, this));
    }
    catch (...)
    {
        startup_callback_ = nullptr;
        m_remote_endpoint = udp::endpoint(); // Sentinel; indicates when transfer is in progress.

        try { m_socket.close(); } catch (...) {}

        try { m_local_file.close(); } catch (...) {}
        try { m_local_file.clear(); } catch (...) {}

        m_transfer_status = make_error_code(boost::system::errc::not_connected);

        throw;
    }

    LEAVE_SYNC_DOMAIN()
}


#if 0
void
tftp_client::put(string host_name, string service_name,
                 string local_pathname, string remote_pathname)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

//!\todo >>> Under Construction <<<

    LEAVE_SYNC_DOMAIN()
}
#endif // #if 0


void
tftp_client::transfer_complete(boost::system::error_code status)
{
    printf("tftp_client::transfer_complete(status=%d=\"%s\")\n",
          static_cast<int>(status.value()), strerror(status.value()));

    // Record transfer result.
    m_transfer_status = status;

    // Clean up.
    try { m_io_timer.cancel(); } catch (...) {}
    try { m_socket.close(); } catch (...) {}
    try { m_local_file.close(); } catch (...) {}

    notify_client_when_all_pending_operations_complete();
}


void
tftp_client::notify_client_when_all_pending_operations_complete()
{
    if (0 == mi_async_io_timer_in_pgrs &&
        0 == mi_async_rx_in_pgrs &&
        0 == mi_async_tx_in_pgrs)
    {
        printf("tftp_client::notify_client_when_all_pending_operations_complete()\n");

        m_remote_endpoint = udp::endpoint(); // Sentinel; indicates when transfer is in progress.

        // Notify subscribers that the operation is complete.
        // WARNING: Do not use io_service::dispatch() or the callback may be invoked while
        //          within the synchronization domain, i.e. this is locked!
        m_io_service.post(boost::bind<void>([](tftp_client *thiz){
            thiz->m_operation_complete_publisher(*thiz);
        }, this));

        // Invoke operation complete functor.
        // WARNING: Do not use io_service::dispatch() or the callback may be invoked while
        //          within the synchronization domain, i.e. this is locked!
        if (nullptr != startup_callback_)
        {
            m_io_service.post(boost::bind<void>([](startup_callback_ptr_type callback,
                                                   boost::system::error_code error_code){
                (*callback)(error_code);
            }, startup_callback_, m_transfer_status));
            startup_callback_.reset();
        }
    }
}


void
tftp_client::tx_read_rqst()
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    printf("tftp_client::tx_read_rqst()\n");

    try
    {
        mb_sent_options = false;

        // Serialize standard RRQ header.
        reset_tx_buf();
        serialize_opcode(TFTP_OPCODE_RRQ);
        serialize_string(m_remote_pathname);
        serialize_string("octet");

        // Serialize block size option, if necessary.
        if (512 != m_blocksize_in_bytes)
        {
            serialize_string(l_tftp_option_name_block_size);
            serialize_integer_as_string(m_blocksize_in_bytes);
            mb_sent_options = true;
        }

        // Serialize transfer size option, if necessary.
        if (0 != m_transfersize_in_bytes)
        {
            serialize_string(l_tftp_option_name_transfer_size);
            serialize_integer_as_string(m_transfersize_in_bytes);
            mb_sent_options = true;
        }

        // Serialize timeout option, if necessary.
        if (0 != m_timeout_in_seconds)
        {
            serialize_string(l_tftp_option_name_timeout);
            serialize_integer_as_string(m_timeout_in_seconds);
            mb_sent_options = true;
        }

        // Transmit packet.
        transmit(&tftp_client::on_read_rqst_rply);
    }
    catch (...)
    {
        transfer_complete(m_transfer_status);
    }

    LEAVE_SYNC_DOMAIN()
}


void
tftp_client::on_read_rqst_rply(boost::system::error_code const& error_code)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    printf("tftp_client::on_read_rqst_rply(error_code = %d=\"%s\")\n",
          static_cast<int>(error_code.value()), strerror(error_code.value()));

    try
    {
        bool const bFailed = boost::system::error_code() != error_code;
        if (bFailed)
        {
            tx_error(TFTP_ERROR_UNDEFINED, "internal error");
            return;
        }

        // Parse the response.
        unsigned int const opcode = deserialize_opcode();
        switch (opcode)
        {
            case TFTP_OPCODE_OACK:
            {
                mb_sent_options = false;
                on_read_rqst_rply_oack(error_code);
                break;
            }

            case TFTP_OPCODE_DATA:
            {
                if (mb_sent_options) { set_tftp_options_to_defaults(); mb_sent_options = false; }
                on_read_rqst_rply_data(error_code);
                break;
            }

            default:
            {
                tx_error(TFTP_ERROR_INVALID_OPCODE, "invalid opcode");
                return;
            }
        }
    }
    catch (...)
    {
        tx_error(TFTP_ERROR_UNDEFINED, "internal error");
    }

    LEAVE_SYNC_DOMAIN()
}


void
tftp_client::on_read_rqst_rply_oack(boost::system::error_code const& error_code)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    printf("tftp_client::on_read_rqst_rply_oack(error_code = %d=\"%s\")\n",
          static_cast<int>(error_code.value()), strerror(error_code.value()));

    try
    {
        bool const bFailed = boost::system::error_code() != error_code;
        if (bFailed)
        {
            tx_error(TFTP_ERROR_UNDEFINED, "internal error");
            return;
        }

        // Parse the response.
        typedef string optname_t;
        typedef string optval_t;
        typedef pair<optname_t, optval_t> opt_t;
        typedef vector<opt_t> opts_t;
        opts_t options;

        while (0 < rx_buf_size_remaining())
        {
            string optname(deserialize_string());
            if (l_tftp_option_name_block_size == optname)
            {
                unsigned int block_size = deserialize_integer_as_string<uint16_t>();
                if (m_blocksize_in_bytes < block_size ||
                    0 == block_size)
                {
                    tx_error(TFTP_ERROR_OPTION, "invalid option value");
                    return;
                }
                else
                {
                    m_blocksize_in_bytes = block_size;
                }
            }
            else if (l_tftp_option_name_transfer_size == optname)
            {
                unsigned int transfer_size = deserialize_integer_as_string<uint16_t>();
                if (m_transfersize_in_bytes < transfer_size ||
                    0 == transfer_size)
                {
                    tx_error(TFTP_ERROR_OPTION, "invalid option value");
                    return;
                }
                else
                {
                    m_transfersize_in_bytes = transfer_size;
                }
            }
            else if (l_tftp_option_name_timeout == optname)
            {
                unsigned int timeout = deserialize_integer_as_string<uint16_t>();
                if (m_timeout_in_seconds < timeout ||
                    0 == timeout)
                {
                    tx_error(TFTP_ERROR_OPTION, "invalid option value");
                    return;
                }
                else
                {
                    m_timeout_in_seconds = timeout;
                }
            }
            else
            {
                // Send error reply.
                tx_error(TFTP_ERROR_OPTION, "unrecognized option");
                return;
            }
        }

        // Send ACK.
        reset_tx_buf();
        serialize_opcode(TFTP_OPCODE_ACK);
        serialize_block_number(0);
        transmit(&tftp_client::on_read_rqst_rply, true);
    }
    catch (...)
    {
        tx_error(TFTP_ERROR_UNDEFINED, "internal error");
    }

    LEAVE_SYNC_DOMAIN()
}


void
tftp_client::on_read_rqst_rply_data(boost::system::error_code const& error_code)
{
    printf("tftp_client::on_read_rqst_rply_data(error_code = %d=\"%s\")\n",
          static_cast<int>(error_code.value()), strerror(error_code.value()));

    try
    {
        bool more_packets_coming = false;

        unsigned int const blknum = deserialize_block_number();
        // Ignore unexpected blocks, deduplicates blocks already received.
        // An inequality test cannot be done on an unsigned integer (block number) due to rollover.
        // Therefore, this cannot be blindly done to detect errors caused by skipped blocks.
        // Retry will eventually correct the problem or terminate the transfer.
        if (blknum == mi_expected_block_number)
        {
            if (0 < rx_buf_size_remaining())
            {
                // Save received data.
                m_local_file.write(reinterpret_cast<char const *>(rx_buf() + mi_rx_buf_iter), rx_buf_size_remaining());
                if (!m_local_file.good()) { throw local_file_exception(); }
            }

            size_t const max_pyld_size = m_blocksize_in_bytes + sizeof(uint16_t) + sizeof(uint16_t);
            more_packets_coming = max_pyld_size == rx_buf_size();

            if (more_packets_coming)
            {
                ++mi_expected_block_number;
            }
        }

        // Send ACK.
        reset_tx_buf();
        serialize_opcode(TFTP_OPCODE_ACK);
        serialize_uint16(mi_expected_block_number - 1);
        io_completion_callback_t callback = more_packets_coming ? &tftp_client::on_read_rqst_rply
                                                                : &tftp_client::on_final_ack_sent;
        transmit(callback, more_packets_coming);
    }
    catch (...)
    {
        tx_error(TFTP_ERROR_UNDEFINED, "internal error");
    }
}


void
tftp_client::on_final_ack_sent(boost::system::error_code const& /*error_code*/)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    printf("tftp_client::on_final_ack_sent()\n");

    try
    {
        /*
            It's ideal to wait around for a while after terminating the transfer
            so another ACK can be sent in the event that the peer resends.
            This implementation does not currently do this.
        */
        if (make_error_code(boost::system::errc::operation_in_progress) == m_transfer_status)
        {
            m_transfer_status = make_error_code(boost::system::errc::success);
        }
        transfer_complete(m_transfer_status);
    }
    catch (...)
    {
        // Do nothing.  (This should _never_ hapen!)
    }

    LEAVE_SYNC_DOMAIN()
}


void
tftp_client::on_error_sent(boost::system::error_code const& /*error_code*/)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    printf("tftp_client::on_error_sent()\n");

    try
    {
        transfer_complete(m_transfer_status);
    }
    catch (...)
    {
        // Do nothing.  (This should _never_ hapen!)
    }

    LEAVE_SYNC_DOMAIN()
}


void
tftp_client::tx_ack(unsigned int block_number, bool more_packets_coming)
{
    printf("tftp_client::tx_ack(block_number=%u, more_packets_coming=%s)\n",
           block_number, (more_packets_coming ? "true" : "false"));

    reset_tx_buf();
    serialize_opcode(TFTP_OPCODE_ACK);
    serialize_uint16(block_number);
    io_completion_callback_t callback = more_packets_coming ? &tftp_client::on_read_rqst_rply
                                                            : &tftp_client::on_final_ack_sent;
    transmit(callback, more_packets_coming);
}


void
tftp_client::tx_error(unsigned int error_code, string message)
{
    printf("tftp_client::tx_ack(error_code=%d=\"%s\", message=\"%s\")\n",
           static_cast<int>(error_code), strerror(error_code), message.c_str());

    reset_tx_buf();
    serialize_opcode(TFTP_OPCODE_ERROR);
    serialize_uint16(error_code);
    serialize_string(message);
    transmit(&tftp_client::on_error_sent, false);
}


void
tftp_client::set_tftp_options_to_defaults()
{
    m_blocksize_in_bytes = (std::min)((std::min)(static_cast<size_t>(512), arycap(mv_tx_buf)), arycap(mv_rx_buf));
    m_transfersize_in_bytes = 0;
    m_timeout_in_seconds = 0;
}


void
tftp_client::reset_tx_buf()
{
    tx_buf_size() = 0;
    fill(tx_buf(), tx_buf() + tx_buf_capacity(), 0); // Superfluous; useful for debugging.
    mi_tx_attempt_counter = 0;
    mi_io_attempt_counter = 0;
}


void
tftp_client::serialize_uint16(uint value)
{
    uint16_t const network_value = htons(value);
    if (m_blocksize_in_bytes <= tx_buf_size() ||
        m_blocksize_in_bytes - tx_buf_size() < sizeof(network_value))
    {
        throw serialization_overflow_exception();
    }
    memcpy(tx_buf() + tx_buf_size(), &network_value, sizeof(network_value));
    tx_buf_size() += sizeof(network_value);
}


uint
tftp_client::deserialize_uint16()
{
    uint16_t value = 0;
    if (rx_buf_size() <= mi_rx_buf_iter ||
        rx_buf_size() - mi_rx_buf_iter < sizeof(value))
    {
        throw serialization_overflow_exception();
    }
    memcpy(&value, rx_buf() + mi_rx_buf_iter, sizeof(value));
    value = ntohs(value);
    mi_rx_buf_iter += sizeof(value);
    return value;
}


void
tftp_client::serialize_string(string const& value)
{
    if (m_blocksize_in_bytes <= tx_buf_size() ||
        m_blocksize_in_bytes - tx_buf_size() < value.length() + 1)
    {
        throw serialization_overflow_exception();
    }
    strncat(reinterpret_cast<char *>(tx_buf() + tx_buf_size()),
            value.c_str(),
            m_blocksize_in_bytes - 1 - tx_buf_size());
    tx_buf_size() += value.length();
    tx_buf()[tx_buf_size()++] = '\0';
}


string
tftp_client::deserialize_string()
{
    size_t start_idx = mi_rx_buf_iter;
    for (; rx_buf_size() > mi_rx_buf_iter; ++mi_rx_buf_iter)
    {
        if ('\0' == rx_buf()[mi_rx_buf_iter]) { break; }
    }
    if (rx_buf_size() == mi_rx_buf_iter)
    {
        mi_rx_buf_iter = start_idx;
        throw serialization_overflow_exception();
    }
    return string(rx_buf()[start_idx], rx_buf()[mi_rx_buf_iter]);
}


void
tftp_client::transmit(io_completion_callback_t io_completion_callback, bool expect_reply)
{
    mi_io_attempt_counter = 0;
    attempt_request(io_completion_callback, expect_reply);
}


void
tftp_client::attempt_request(io_completion_callback_t io_completion_callback, bool expect_reply)
{
    printf("\n");
    printf("tftp_client::attempt_request(expect_reply = %s)\n", (expect_reply ? "true" : "false"));

    mi_tx_attempt_counter = 0;

    // Start transmitting I/O buffer content.
    if (1 == mi_expected_block_number)
    {
        m_socket.async_send_to(boost::asio::buffer(tx_buf(), tx_buf_size()), m_remote_endpoint,
                               boost::bind(&tftp_client::on_tx_complete, this,
                                           boost::asio::placeholders::error,
                                           boost::asio::placeholders::bytes_transferred,
                                           io_completion_callback,
                                           expect_reply));
    }
    else
    {
        m_socket.async_send(boost::asio::buffer(tx_buf(), tx_buf_size()),
                            boost::bind(&tftp_client::on_tx_complete, this,
                                        boost::asio::placeholders::error,
                                        boost::asio::placeholders::bytes_transferred,
                                        io_completion_callback,
                                        expect_reply));
    }
    ++mi_async_tx_in_pgrs;
    printf("tftp_client::attempt_request(): ++mi_async_tx_in_pgrs = %lu (%s[%lu])\n", static_cast<unsigned long>(mi_async_tx_in_pgrs), __FILE__, static_cast<unsigned long>(__LINE__));

    // Cancel I/O timer (failsafe).
    m_io_timer.cancel();
}


void
tftp_client::on_tx_complete(boost::system::error_code const& error_code, size_t bytes_transferred,
                            io_completion_callback_t io_completion_callback, bool expect_reply)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    printf("tftp_client::on_tx_complete(error_code = %d=\"%s\", bytes_transferred = %u, expect_reply = %s)\n",
          static_cast<int>(error_code.value()), strerror(error_code.value()),
          static_cast<int>(bytes_transferred), (expect_reply ? "true" : "false"));

    --mi_async_tx_in_pgrs;
    printf("tftp_client::on_tx_complete(): --mi_async_tx_in_pgrs = %lu (%s[%lu])\n",
                        static_cast<unsigned long>(mi_async_tx_in_pgrs), __FILE__, static_cast<unsigned long>(__LINE__));

    // Handle transfer cancellation.
    bool operation_cancelled = !m_socket.is_open();
    if (operation_cancelled)
    {
        notify_client_when_all_pending_operations_complete();
        return;
    }

    bool transmit_failed = boost::system::error_code() != error_code ||
                           bytes_transferred != tx_buf_size();
    if (transmit_failed)
    {
        if (mi_io_attempt_max_count <= ++mi_tx_attempt_counter)
        {
            // Fail the operation.
            rx_buf_size() = 0;
            mi_rx_buf_iter = 0;
            (this->*io_completion_callback)(error_code);
        }
        else
        {
            // Retry the transmission after a [short] delay.
            m_io_timer.expires_from_now(boost::posix_time::seconds(timeout_in_seconds()));
            m_io_timer.async_wait(boost::bind<void>([](boost::system::error_code const& error_code, tftp_client *thiz,
                                                       io_completion_callback_t io_completion_callback,
                                                       bool expect_reply)
            { // Lambda body.
                --thiz->mi_async_io_timer_in_pgrs;

                bool const bTimerCanceled = make_error_code(boost::asio::error::operation_aborted) == error_code;
/*
Canceling a timer _always_ causes it to send operation_aborted to all pending async operations!
Therefore, operation_aborted is an unreliable way to know if something went wrong or the transfer has been aborted.
It is necessary to check an extended state variable to know if the transfer has been aborted.
                if (bTimerCanceled)
                {
                    (thiz->*io_completion_callback)(make_error_code(boost::system::errc::operation_canceled), 0);
                }
*/
                if (!bTimerCanceled &&
                    thiz->m_io_timer.expires_at() <= deadline_timer::traits_type::now())
                {
                    unsigned int const tx_retry_counter = thiz->mi_tx_attempt_counter;
                    thiz->attempt_request(io_completion_callback, expect_reply);
                    thiz->mi_tx_attempt_counter = tx_retry_counter;
                }
            }, _1, this, io_completion_callback, expect_reply));
            ++mi_async_io_timer_in_pgrs;
            printf("tftp_client::on_tx_complete(): ++mi_async_io_timer_in_pgrs = %lu (%s[%lu])\n", static_cast<unsigned long>(mi_async_io_timer_in_pgrs), __FILE__, static_cast<unsigned long>(__LINE__));
        }
    }
    else
    {
        if (expect_reply)
        {
            printf("tftp_client::on_tx_complete(): starting receive.\n");

            fill(rx_buf(), rx_buf() + rx_buf_capacity(), 0); // Superfluous; useful for debugging.
            rx_buf_size() = 0;
            mi_rx_buf_iter = 0;

            // Start receive operation.
            size_t const max_read_size = rx_buf_capacity();
            if (1 == mi_expected_block_number)
            {
                m_receiver.async_receive_from(rx_buf(), max_read_size,
                                              boost::posix_time::seconds(timeout_in_seconds()),
                                              m_remote_endpoint,
                                              boost::protect(boost::bind(&tftp_client::on_rx_complete, this,
                                                                         boost::asio::placeholders::error,
                                                                         boost::asio::placeholders::bytes_transferred,
                                                                         io_completion_callback, expect_reply)));
            }
            else
            {
                m_receiver.async_receive(rx_buf(), max_read_size, boost::posix_time::seconds(timeout_in_seconds()),
                                         boost::protect(boost::bind(&tftp_client::on_rx_complete, this,
                                                                    boost::asio::placeholders::error,
                                                                    boost::asio::placeholders::bytes_transferred,
                                                                    io_completion_callback, expect_reply)));
            }
            ++mi_async_rx_in_pgrs;
            printf("tftp_client::on_tx_complete(): ++mi_async_rx_in_pgrs = %lu (%s[%lu])\n",
                                static_cast<unsigned long>(mi_async_rx_in_pgrs), __FILE__,
                                static_cast<unsigned long>(__LINE__));
        }
        else
        {
            (this->*io_completion_callback)(error_code);
        }
    }

    LEAVE_SYNC_DOMAIN()
}


void
tftp_client::on_rx_complete(boost::system::error_code const& error_code, size_t bytes_transferred,
                            io_completion_callback_t io_completion_callback, bool expect_reply)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    printf("tftp_client::on_rx_complete(error_code = %d=\"%s\", bytes_transferred = %u, expect_reply = %s)\n",
          static_cast<int>(error_code.value()), strerror(error_code.value()),
          static_cast<int>(bytes_transferred), (expect_reply ? "true" : "false"));

    --mi_async_rx_in_pgrs;
    printf("tftp_client::on_rx_complete(): --mi_async_rx_in_pgrs = %lu (%s[%lu])\n",
                        static_cast<unsigned long>(mi_async_rx_in_pgrs), __FILE__,
                        static_cast<unsigned long>(__LINE__));

    // Handle transfer cancellation.
    bool operation_cancelled = !m_socket.is_open();
    if (operation_cancelled)
    {
        notify_client_when_all_pending_operations_complete();
        return;
    }

    bool const receive_failed = boost::system::error_code() != error_code;
    if (receive_failed)
    {
        /*
            Canceling I/O _always_ sends operation_cencelled to all pending async operations!
            The receive timeout handler canceles all pending I/O (to kill the read in progress) when timeout occurrs.
            Therefore, operation_cancelled is an unreliable way to detect when the transfer has been aborted.
        */
        if (boost::system::errc::operation_canceled != error_code.value())
        {
            bool retry = mi_io_attempt_max_count > mi_io_attempt_counter;
            if (retry &&

//!\TODO: >>> Evaluate and handle all error codes. <<<

                boost::system::errc::connection_aborted != error_code.value())
            {
                // Retry the operation.
                printf("tftp_client::on_rx_complete(): retrying operation.\n");
                ++mi_io_attempt_counter;
                attempt_request(io_completion_callback, expect_reply);
            }
            else
            {
                (this->*io_completion_callback)(error_code);
            }
        }
    }
    else
    {
        // Connect the socket to the remote endpoint when the first response is received,
        // which will include the peer's connection information (ip and port).
        if (1 == mi_expected_block_number) { m_socket.connect(m_remote_endpoint); }

        rx_buf_size() = bytes_transferred;
        mi_rx_buf_iter = 0;

        try
        {
            unsigned int const opcode = deserialize_opcode();
            if (TFTP_OPCODE_ERROR == opcode)
            {
                unsigned int const tftp_error_code = deserialize_uint16();
                printf("tftp_client::on_rx_complete(): received error opcode = %u.\n", tftp_error_code);
                switch (tftp_error_code)
                {

//!\todo >>> Implement specific error codes in the future, if necessary. <<<

                    case 0: // Not defined, see error message (if any).
                    case 1: // File not found.
                    case 2: // Access violation.
                    case 3: // Disk full or allocation exceeded.
                    case 4: // Illegal TFTP operation.
                    case 5: // Unknown transfer ID.
                    case 6: // File already exists.
                    case 7: // No such user.
                    default:
                    {
                        (this->*io_completion_callback)(make_error_code(boost::system::errc::protocol_error));
                        break;
                    }
                }
            }
            else
            {
                mi_rx_buf_iter -= sizeof(uint16_t); // Rewind to undo prior opcode deserialization.
                (this->*io_completion_callback)(error_code);
            }
        }
        catch (...)
        {
            (this->*io_completion_callback)(make_error_code(boost::system::errc::protocol_error));
        }
    }

    LEAVE_SYNC_DOMAIN()
}


bool
tftp_client::in_progress() const
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    return udp::endpoint() != m_remote_endpoint;

    LEAVE_SYNC_DOMAIN()
}


boost::system::error_code
tftp_client::transfer_status() const
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    return m_transfer_status;

    LEAVE_SYNC_DOMAIN()
}


uint_least16_t
tftp_client::set_blocksize_in_bytes(uint_least16_t value)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    return m_blocksize_in_bytes = value;

    LEAVE_SYNC_DOMAIN()
}


uint_least16_t
tftp_client::blocksize_in_bytes() const
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    return m_blocksize_in_bytes;

    LEAVE_SYNC_DOMAIN()
}


uint_least32_t
tftp_client::set_transfersize_in_bytes(uint_least32_t value)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    return m_transfersize_in_bytes = value;

    LEAVE_SYNC_DOMAIN()
}


uint_least32_t
tftp_client::transfersize_in_bytes() const
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    return m_transfersize_in_bytes;

    LEAVE_SYNC_DOMAIN()
}


uint_least32_t
tftp_client::set_timeout_in_seconds(uint_least32_t value)
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    return m_timeout_in_seconds = value;

    LEAVE_SYNC_DOMAIN()
}


uint_least32_t
tftp_client::timeout_in_seconds() const
{
    ENTER_SYNC_DOMAIN(sync_domain_lock())

    return 0 == m_timeout_in_seconds ? 1 : m_timeout_in_seconds;

    LEAVE_SYNC_DOMAIN()
}


tftp_client::tftp_client(io_service& io_service, lock_type *sync_domain_lock)
    : sync_domain_provider(sync_domain_lock)
    , m_io_service(io_service)
    , m_io_timer(m_io_service)
    , m_socket(m_io_service)
    , m_receiver(m_io_service, sync_domain_lock)
    , mi_async_io_timer_in_pgrs(0)
    , mi_async_rx_in_pgrs(0)
    , mi_async_tx_in_pgrs(0)
    , mi_io_attempt_max_count(5)
    , mi_expected_block_number(0)
    , m_transfer_status(make_error_code(boost::system::errc::not_connected))
{
    set_tftp_options_to_defaults();
}


tftp_client::~tftp_client()
{
    // Do nothing.
}


} // namespace tftp {
} // namespace adapter {


/*
    End of "tftp_client.cpp"
*/
