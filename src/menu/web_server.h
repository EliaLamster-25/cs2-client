#pragma once

/// @file web_server.h
/// @brief Local HTTP Server for the web-based settings menu.

class WebServer {
public:
    static WebServer& get() { static WebServer s; return s; }

    /// Starts the web server thread on localhost:8080.
    void start();

    /// Stops the web server thread.
    void stop();

private:
    WebServer() = default;
    ~WebServer() { stop(); }
};
