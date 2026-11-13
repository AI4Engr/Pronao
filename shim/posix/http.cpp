// POSIX: HTTP requests are not implemented yet (the Windows layer is shim/win/http.cpp); the program is told so and nothing is sent.
#include <string>
#include <vector>

#include "../app_native.h"

int app_http_request(int, std::string, std::string, std::string, std::vector<std::string>, std::string, int, int, int& status_code, std::string& resp_headers,
                     std::string& resp_body, int& flags) {
    status_code = 0;
    resp_headers.clear();
    resp_body.clear();
    flags = 0;
    return 5;
}
