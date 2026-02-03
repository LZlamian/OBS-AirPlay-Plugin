#include "raop-server.hpp"
#include <obs-module.h>

RAOPServer::RAOPServer()
    : m_running(false)
{
}

RAOPServer::~RAOPServer()
{
    stop();
}

bool RAOPServer::start(uint16_t port)
{
    UNUSED_PARAMETER(port);
    m_running = true;
    blog(LOG_INFO, "RAOP server started");
    return true;
}

void RAOPServer::stop()
{
    if (m_running) {
        m_running = false;
        blog(LOG_INFO, "RAOP server stopped");
    }
}
