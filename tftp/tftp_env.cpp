/*!
    \file "tftp_env.cpp"

    Formatting: 4 spaces/tab, 120 columns.
    Doc-tool: Doxygen (http://www.doxygen.com/)
*/


#include <adapter/tftp/tftp_env.hpp>


namespace adapter {
namespace tftp {


#ifdef DEBUG


void tftp_trace(char const *format, ...)
{
    va_list args;
    va_start(args, format);
    adapter_vtrace(format, args);
}


#endif // #ifdef DEBUG


} // namespace tftp {
} // namespace adapter {


/*
    End of "tftp_env.cpp"
*/
