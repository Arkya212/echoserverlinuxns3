#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include <iostream>

int main() {
    httplib::Client cli("https://www.google.com");

    auto res = cli.Get("/");
    if (res) {
        std::cout << "Status: " << res->status << std::endl;
    } else {
        std::cout << "Error: " << httplib::to_string(res.error()) << std::endl;
    }
}
