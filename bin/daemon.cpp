#include "daemon.hpp"

#include "daemon_options.hpp"

#include <bonefish/serialization/wamp_serializers.hpp>
#include <bonefish/serialization/json_serializer.hpp>
#include <bonefish/serialization/msgpack_serializer.hpp>
#include <bonefish/router/wamp_router.hpp>
#include <bonefish/router/wamp_routers.hpp>
#include <bonefish/rawsocket/rawsocket_server.hpp>
#include <bonefish/rawsocket/tcp_listener.hpp>
#include <bonefish/trace/trace.hpp>
#include <bonefish/websocket/websocket_server.hpp>

#include <boost/asio/ip/address.hpp>
#include <boost/bind.hpp>
#include <signal.h>
#include <string.h>

namespace bonefish
{

daemon::daemon(const daemon_options& options)
    : m_io_service()
    , m_work()
    , m_termination_signals(m_io_service, SIGTERM, SIGINT, SIGQUIT)
    , m_routers(new wamp_routers)
    , m_serializers(new wamp_serializers)
    , m_rawsocket_server()
    , m_websocket_server()
    , m_websocket_port(0)
{
    std::vector<std::string> problems = options.problems();
    if (!problems.empty()) {
        std::cerr << "There were errors starting the bonefish daemon, please fix and restart:\n";
        for (const std::string& problem : problems) {
            std::cerr << "* " << problem << "\n";
        }
        std::cerr << std::endl;
        exit(1);
    }

    if (getenv("BONEFISH_TRACE")) {
        bonefish::trace::set_enabled(true);
    }

    auto router = std::make_shared<wamp_router>(m_io_service, options.realm());
    m_routers->add_router(router);

    if (options.is_json_serialization_enabled()) {
        m_serializers->add_serializer(std::make_shared<json_serializer>());
    }
    if (options.is_msgpack_serialization_enabled()) {
        m_serializers->add_serializer(std::make_shared<msgpack_serializer>());
    }

    if (options.is_websocket_enabled()) {
        m_websocket_server = std::make_shared<websocket_server>(
                m_io_service, m_routers, m_serializers);
        m_websocket_port = options.websocket_port();
    }

    if (options.is_rawsocket_enabled()) {
        m_rawsocket_server = std::make_shared<rawsocket_server>(m_routers, m_serializers);
        auto listener = std::make_shared<tcp_listener>(
                m_io_service, boost::asio::ip::address(), options.rawsocket_port());
        m_rawsocket_server->attach_listener(std::static_pointer_cast<rawsocket_listener>(listener));
    }
}

daemon::~daemon()
{
    m_termination_signals.cancel();
}

void daemon::run()
{
    m_work.reset(new boost::asio::io_service::work(m_io_service));
    m_termination_signals.async_wait(
            boost::bind(&daemon::termination_signal_handler, this, _1, _2));

    if (m_rawsocket_server) {
        m_rawsocket_server->start();
    }
    if (m_websocket_server) {
        m_websocket_server->start(boost::asio::ip::address(), m_websocket_port);
    }

    m_io_service.run();
}

void daemon::shutdown()
{
    if (m_work.get()) {
        m_io_service.dispatch(boost::bind(&daemon::shutdown_handler, this));
    }
}

boost::asio::io_service& daemon::io_service()
{
    return m_io_service;
}

void daemon::shutdown_handler()
{
    if (m_work.get()) {
        m_termination_signals.cancel();

        if (m_websocket_server) {
            m_websocket_server->shutdown();
        }
        if (m_rawsocket_server) {
            m_rawsocket_server->shutdown();
        }
        m_work.reset();
    }
}

void daemon::termination_signal_handler(
        const boost::system::error_code& error_code, int signal_number)
{
    if (error_code == boost::asio::error::operation_aborted) {
        return;
    }

    if (signal_number == SIGINT || signal_number == SIGTERM || signal_number == SIGQUIT) {
        shutdown();
    } else {
        // We should never encounter a case where we are invoked for a
        // signal for which we are not intentionally checking for. We
        // keep the assert for debugging purposes and re-register the
        // handler for production purposes so that we are still able to
        // terminate upon receipt of one of the registered signals.
        assert(0);
        m_termination_signals.async_wait(
                boost::bind(&daemon::termination_signal_handler, this, _1, _2));
    }
}

} // namespace bonefish
