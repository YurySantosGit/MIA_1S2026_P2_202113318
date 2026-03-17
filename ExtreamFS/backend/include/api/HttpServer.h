#pragma once

#include <string>

class HttpServer {
public:
    HttpServer(const std::string& host = "0.0.0.0", int port = 8080);
    bool Start();
    void Stop();

private:
    std::string host_;
    int port_;
};