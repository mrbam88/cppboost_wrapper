/*!
    \file "video_env.cpp"

    Formatting: 4 spaces/tab, 120 columns.
    Doc-tool: Doxygen (http://www.doxygen.com/)
*/


#include <adapter/video/video_env.hpp>


namespace adapter {
namespace video {


#ifdef DEBUG


void video_trace(char const *format, ...)
{
    va_list args;
    va_start(args, format);
    adapter_vtrace(format, args);
}


#endif // #ifdef DEBUG


} // namespace video {
} // namespace adapter {


/*
    End of "video_env.cpp"
*/
