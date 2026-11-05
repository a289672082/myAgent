#include "mymcp/mcp.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
  if(argc!=2)throw std::invalid_argument("Expected local SSE test URL");
  int events=0;
  mymcp::Client::listen_http(argv[1],
    mymcp::json::object{{"toolsListChanged",true}},
    [&](const mymcp::Json& event) {
      if(event.is_object() && event.contains("method") &&
         event.at("method")=="notifications/tools/list_changed") {
        ++events;
        return events<2;
      }
      return true;
    },1);
  if(events!=2)throw std::runtime_error("SSE subscription was not re-established");
  std::cout<<"SSE reconnect and CRLF frame validation passed\n";
  return 0;
}
