#include "rest.hh"

namespace Rest
{
thread_local const cppgow::ServerRequest* request;
thread_local cppgow::ServerResponse* reply;
}