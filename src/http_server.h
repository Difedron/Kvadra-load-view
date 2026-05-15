#pragma once

#include "resource_monitor.h"

#include <string>

class HttpServer {
public:
    HttpServer(int port, std::string webRoot, ResourceMonitor& monitor);

    // Блокирующий запуск сервера. Завершение: Ctrl+C в терминале.

    void run();

private:
    int port_;
    std::string webRoot_;
    ResourceMonitor& monitor_;

    void handleClient(int clientSocket);
    void sendResponse(int clientSocket,
                      int statusCode,
                      const std::string& statusText,
                      const std::string& contentType,
                      const std::string& body,
                      bool noCache);
    void sendNotFound(int clientSocket);
    void sendBadRequest(int clientSocket);
    void sendMethodNotAllowed(int clientSocket);

    std::string readRequest(int clientSocket);
    std::string resolveStaticPath(const std::string& requestPath) const;
    std::string readFile(const std::string& path) const;
    std::string mimeType(const std::string& path) const;
};
