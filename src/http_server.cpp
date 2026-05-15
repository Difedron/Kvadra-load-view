#include "http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

constexpr int Backlog = 16;
constexpr int ReadBufferSize = 4096;
constexpr int MaxRequestSize = 16 * 1024;

// Проверка существования обычного файла перед отдачей статики

bool fileExists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

// Проверка папки WebUI на старте приложения

bool directoryExists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// Браузер может добавить query string(такую как /app.js?v=1)
// Для поиска файла на диске нам нужна только часть пути до знака вопроса

std::string stripQueryString(const std::string& path) {
    const std::size_t queryStart = path.find('?');
    if (queryStart == std::string::npos) {
        return path;
    }

    return path.substr(0, queryStart);
}

// send() не обязан отправить все байты за один вызов, поэтому заводим его в цикл, пока весь HTTP-ответ не уйдет клиенту

bool sendAll(int socket, const std::string& data) {
    const char* cursor = data.data();
    std::size_t left = data.size();

    while (left > 0) {
        const ssize_t sent = send(socket, cursor, left, 0);
        if (sent <= 0) {
            return false;
        }

        cursor += sent;
        left -= static_cast<std::size_t>(sent);
    }

    return true;
}

}

HttpServer::HttpServer(int port, std::string webRoot, ResourceMonitor& monitor)
    : port_(port), webRoot_(std::move(webRoot)), monitor_(monitor) {
    if (!directoryExists(webRoot_)) {
        throw std::runtime_error("папка WebUI не найдена: " + webRoot_);
    }
}

void HttpServer::run() {

    // Создаем обычный TCP-сокет. HTTP выбран как простой IPC между WebUI и C++

    const int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        throw std::runtime_error("socket(): " + std::string(std::strerror(errno)));
    }

    // SO_REUSEADDR нужен для перезапуска приложения на том же порту

    int reuse = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Слушаем только localhost: приложение не открывает порт во внешнюю сеть

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port_));

    if (bind(serverSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        close(serverSocket);
        throw std::runtime_error("bind(): " + std::string(std::strerror(errno)));
    }

    if (listen(serverSocket, Backlog) < 0) {
        close(serverSocket);
        throw std::runtime_error("listen(): " + std::string(std::strerror(errno)));
    }

    // Каждый запрос обрабатываем в отдельном потоке

    while (true) {
        sockaddr_in clientAddress {};
        socklen_t clientLength = sizeof(clientAddress);
        const int clientSocket = accept(serverSocket,
                                        reinterpret_cast<sockaddr*>(&clientAddress),
                                        &clientLength);
        if (clientSocket < 0) {
            std::cerr << "accept(): " << std::strerror(errno) << "\n";
            continue;
        }

        std::thread(&HttpServer::handleClient, this, clientSocket).detach();
    }
}

void HttpServer::handleClient(int clientSocket) {
    const std::string request = readRequest(clientSocket);
    if (request.empty()) {
        close(clientSocket);
        return;
    }

    std::istringstream input(request);
    std::string method;
    std::string rawPath;
    std::string version;
    input >> method >> rawPath >> version;

    if (method.empty() || rawPath.empty() || version.empty()) {
        sendBadRequest(clientSocket);
        close(clientSocket);
        return;
    }

    if (method != "GET") {
        sendMethodNotAllowed(clientSocket);
        close(clientSocket);
        return;
    }

    const std::string requestPath = stripQueryString(rawPath);

    // Динамический API: frontend раз в секунду забирает новый JSON

    if (requestPath == "/api/stats") {
        sendResponse(clientSocket,
                     200,
                     "OK",
                     "application/json; charset=utf-8",
                     monitor_.collectJson(),
                     true);
        close(clientSocket);
        return;
    }

    // health-check нужен для ручной проверки через curl

    if (requestPath == "/api/health") {
        sendResponse(clientSocket,
                     200,
                     "OK",
                     "application/json; charset=utf-8",
                     "{\"status\":\"ok\"}\n",
                     true);
        close(clientSocket);
        return;
    }

    // Все остальные GET-запросы считаем запросами к статическим файлам WebUI

    const std::string staticPath = resolveStaticPath(requestPath);
    if (staticPath.empty() || !fileExists(staticPath)) {
        sendNotFound(clientSocket);
        close(clientSocket);
        return;
    }

    sendResponse(clientSocket, 200, "OK", mimeType(staticPath), readFile(staticPath), false);
    close(clientSocket);
}

std::string HttpServer::readRequest(int clientSocket) {
    std::string request;
    char buffer[ReadBufferSize];

    // Читаем только заголовки HTTP, пустая строка \r\n\r\n означает конец заголовка

    while (request.find("\r\n\r\n") == std::string::npos &&
           request.size() < static_cast<std::size_t>(MaxRequestSize)) {
        const ssize_t received = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }

        request.append(buffer, static_cast<std::size_t>(received));
    }

    return request;
}

void HttpServer::sendResponse(int clientSocket,
                              int statusCode,
                              const std::string& statusText,
                              const std::string& contentType,
                              const std::string& body,
                              bool noCache) {

    // Формируем минимальный HTTP/1.1

    std::ostringstream response;
    response << "HTTP/1.1 " << statusCode << ' ' << statusText << "\r\n";
    response << "Content-Type: " << contentType << "\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    response << "Connection: close\r\n";
    if (noCache) {
        response << "Cache-Control: no-store\r\n";
    }
    response << "\r\n";
    response << body;

    sendAll(clientSocket, response.str());
}

void HttpServer::sendNotFound(int clientSocket) {
    sendResponse(clientSocket,
                 404,
                 "Not Found",
                 "text/plain; charset=utf-8",
                 "404: файл или маршрут не найден\n",
                 true);
}

void HttpServer::sendBadRequest(int clientSocket) {
    sendResponse(clientSocket,
                 400,
                 "Bad Request",
                 "text/plain; charset=utf-8",
                 "400: некорректный HTTP-запрос\n",
                 true);
}

void HttpServer::sendMethodNotAllowed(int clientSocket) {
    sendResponse(clientSocket,
                 405,
                 "Method Not Allowed",
                 "text/plain; charset=utf-8",
                 "405: поддерживается только GET\n",
                 true);
}

std::string HttpServer::resolveStaticPath(const std::string& requestPath) const {

    // Защита от path traversal (запросы вида /../../etc/passwd не обслуживаются)

    if (requestPath.find("..") != std::string::npos) {
        return {};
    }

    std::string relativePath = requestPath;

    // Главная страница по умолчанию

    if (relativePath == "/") {
        relativePath = "/index.html";
    }

    if (relativePath.empty() || relativePath[0] != '/') {
        return {};
    }

    return webRoot_ + relativePath;
}

std::string HttpServer::readFile(const std::string& path) const {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

std::string HttpServer::mimeType(const std::string& path) const {

    // MIME type нужен браузеру, чтобы правильно интерпретировать HTML/CSS/JS

    if (path.size() >= 5 && path.substr(path.size() - 5) == ".html") {
        return "text/html; charset=utf-8";
    }
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".css") {
        return "text/css; charset=utf-8";
    }
    if (path.size() >= 3 && path.substr(path.size() - 3) == ".js") {
        return "application/javascript; charset=utf-8";
    }
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".svg") {
        return "image/svg+xml";
    }

    return "application/octet-stream";
}
