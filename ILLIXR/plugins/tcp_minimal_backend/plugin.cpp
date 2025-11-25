#include "plugin.hpp"
#include <spdlog/spdlog.h>
#include <cstring>

using namespace ILLIXR;

tcp_minimal_backend::tcp_minimal_backend(const std::string& name_, phonebook* pb_)
    : plugin(name_, pb_)
    , switchboard_{pb_->lookup_impl<switchboard>()} {
    
    // Read environment variables
    if (switchboard_->get_env_char("ILLIXR_TCP_SERVER_IP")) {
        server_ip_ = switchboard_->get_env_char("ILLIXR_TCP_SERVER_IP");
        spdlog::get("illixr")->info("[tcp_minimal_backend] Using TCP server IP {}", server_ip_);
    }

    if (switchboard_->get_env_char("ILLIXR_TCP_SERVER_PORT")) {
        server_port_ = std::stoi(switchboard_->get_env_char("ILLIXR_TCP_SERVER_PORT"));
        spdlog::get("illixr")->info("[tcp_minimal_backend] Using TCP server port {}", server_port_);
    }

    if (switchboard_->get_env_char("ILLIXR_TCP_CLIENT_IP")) {
        client_ip_ = switchboard_->get_env_char("ILLIXR_TCP_CLIENT_IP");
        spdlog::get("illixr")->info("[tcp_minimal_backend] Using TCP client IP {}", client_ip_);
    }

    if (switchboard_->get_env_char("ILLIXR_TCP_CLIENT_PORT")) {
        client_port_ = std::stoi(switchboard_->get_env_char("ILLIXR_TCP_CLIENT_PORT"));
        spdlog::get("illixr")->info("[tcp_minimal_backend] Using TCP client port {}", client_port_);
    }

    if (switchboard_->get_env_char("ILLIXR_IS_CLIENT")) {
        is_client_ = std::stoi(switchboard_->get_env_char("ILLIXR_IS_CLIENT"));
        spdlog::get("illixr")->info("[tcp_minimal_backend] Is client: {}", is_client_);
    }

    // Relay-specific configuration
    if (switchboard_->get_env_char("ILLIXR_FORWARD_IP")) {
        forward_ip_ = switchboard_->get_env_char("ILLIXR_FORWARD_IP");
        spdlog::get("illixr")->info("[tcp_minimal_backend] Forward IP: {}", forward_ip_);
    }

    if (switchboard_->get_env_char("ILLIXR_FORWARD_PORT")) {
        forward_port_ = std::stoi(switchboard_->get_env_char("ILLIXR_FORWARD_PORT"));
        spdlog::get("illixr")->info("[tcp_minimal_backend] Forward port: {}", forward_port_);
    }

    // Start in appropriate mode
    if (is_client_) {
        spdlog::get("illixr")->info("[tcp_minimal_backend] Starting in CLIENT mode");
        std::thread([this]() { start_client(); }).detach();
    } else {
        spdlog::get("illixr")->info("[tcp_minimal_backend] Starting in RELAY/SERVER mode");
        std::thread([this]() { start_server(); }).detach();
    }

    // Wait until connection is established
    while (!ready_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    spdlog::get("illixr")->info("[tcp_minimal_backend] Initialization complete");
}

void tcp_minimal_backend::start_client() {
    auto* socket = new network::TCPSocket();
    
    if (!client_ip_.empty() && client_port_ > 0) {
        socket->socket_bind(client_ip_, client_port_);
        spdlog::get("illixr")->info("[tcp_minimal_backend] Bound to {}:{}", client_ip_, client_port_);
    }
    
    socket->socket_set_reuseaddr();
    socket->enable_no_delay();
    peer_socket_ = socket;

    spdlog::get("illixr")->info("[tcp_minimal_backend] Connecting to {}:{}", server_ip_, server_port_);
    socket->socket_connect(server_ip_, server_port_);
    spdlog::get("illixr")->info("[tcp_minimal_backend] Connected to relay/server");

    ready_ = true;

    // Run read loop - client receives data from relay/server
    read_loop(socket);
}

void tcp_minimal_backend::start_server() {
    network::TCPSocket server_socket;
    server_socket.socket_set_reuseaddr();
    server_socket.socket_bind(server_ip_, server_port_);
    server_socket.enable_no_delay();
    server_socket.socket_listen();

    spdlog::get("illixr")->info("[tcp_minimal_backend] Listening on {}:{}", server_ip_, server_port_);
    
    auto* client_socket = new network::TCPSocket(server_socket.socket_accept());
    spdlog::get("illixr")->info("[tcp_minimal_backend] Accepted connection from {}", client_socket->peer_address());
    
    peer_socket_ = client_socket;
    ready_ = true;

    // If forward settings are present, this is RELAY mode
    if (!forward_ip_.empty() && forward_port_ > 0) {
        spdlog::get("illixr")->info("[tcp_minimal_backend] RELAY MODE: Connecting to end server {}:{}", 
                                    forward_ip_, forward_port_);
        
        forward_socket_ = new network::TCPSocket();
        forward_socket_->socket_set_reuseaddr();
        forward_socket_->enable_no_delay();
        
        try {
            forward_socket_->socket_connect(forward_ip_, forward_port_);
            spdlog::get("illixr")->info("[tcp_minimal_backend] Connected to end server");
            
            // Start reverse path thread (end server → client)
            std::thread([this]() { read_loop_reverse(); }).detach();
            spdlog::get("illixr")->info("[tcp_minimal_backend] Started reverse path thread");
            
        } catch (const std::exception& e) {
            spdlog::get("illixr")->error("[tcp_minimal_backend] Failed to connect to end server: {}", e.what());
            delete forward_socket_;
            forward_socket_ = nullptr;
        }
    } else {
        spdlog::get("illixr")->info("[tcp_minimal_backend] SERVER MODE: No forwarding");
    }

    // Forward path: client → end server
    read_loop(client_socket);
}

void tcp_minimal_backend::read_loop(network::TCPSocket* socket) {
    std::string buffer;
    buffer.reserve(65536);

    while (running_) {
        std::string chunk = socket->read_data();
        
        if (!chunk.empty()) {
            buffer += chunk;
        }

        // Process complete packets
        while (buffer.size() >= 8) {
            uint32_t total_len = 0, topic_len = 0;
            std::memcpy(&total_len, buffer.data(), 4);
            std::memcpy(&topic_len, buffer.data() + 4, 4);

            // Validation
            if (total_len < 8 || total_len > 100000000 || topic_len > total_len - 8) {
                spdlog::get("illixr")->error("[tcp_minimal_backend] Invalid packet header: total={}, topic={}", 
                                             total_len, topic_len);
                buffer.clear();
                break;
            }

            if (buffer.size() < total_len) {
                // Wait for complete packet
                break;
            }

            // Extract complete packet
            std::string packet = buffer.substr(0, total_len);
            buffer.erase(0, total_len);

            // Decode topic name
            std::string topic_name(packet.data() + 8, topic_len);
            
            spdlog::get("illixr")->debug("[tcp_minimal_backend] Received topic='{}' size={}", 
                                        topic_name, total_len);

            // If we're in relay mode with forwarding enabled
            if (forward_socket_) {
                // Forward to end server (no filtering)
                try {
                    forward_socket_->write_data(packet);
                    spdlog::get("illixr")->info("[tcp_minimal_backend] RELAY FWD: Forwarded topic='{}' size={}", 
                                               topic_name, total_len);
                } catch (const std::exception& e) {
                    spdlog::get("illixr")->error("[tcp_minimal_backend] Forward error: {}", e.what());
                    running_ = false;
                    break;
                }
            } else {
                // Normal client/server mode - process the message locally
                std::vector<char> message(packet.begin() + 8 + topic_len, packet.end());
                topic_receive(topic_name, message);
            }
        }
        
        // Small delay if no data
        if (chunk.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    spdlog::get("illixr")->info("[tcp_minimal_backend] Read loop terminated");
}

void tcp_minimal_backend::read_loop_reverse() {
    std::string buffer;
    buffer.reserve(65536);

    spdlog::get("illixr")->info("[tcp_minimal_backend] Reverse path loop started");

    while (running_) {
        std::string chunk = forward_socket_->read_data();
        
        if (!chunk.empty()) {
            buffer += chunk;
        }

        // Process complete packets
        while (buffer.size() >= 8) {
            uint32_t total_len = 0, topic_len = 0;
            std::memcpy(&total_len, buffer.data(), 4);
            std::memcpy(&topic_len, buffer.data() + 4, 4);

            // Validation
            if (total_len < 8 || total_len > 100000000 || topic_len > total_len - 8) {
                spdlog::get("illixr")->error("[tcp_minimal_backend] REVERSE: Invalid packet header: total={}, topic={}", 
                                             total_len, topic_len);
                buffer.clear();
                break;
            }

            if (buffer.size() < total_len) {
                // Wait for complete packet
                break;
            }

            // Extract complete packet
            std::string packet = buffer.substr(0, total_len);
            buffer.erase(0, total_len);

            // Decode topic name
            std::string topic_name(packet.data() + 8, topic_len);
            
            spdlog::get("illixr")->debug("[tcp_minimal_backend] REVERSE: Received topic='{}' size={}", 
                                        topic_name, total_len);

            // Forward back to client (no filtering)
            try {
                peer_socket_->write_data(packet);
                spdlog::get("illixr")->info("[tcp_minimal_backend] RELAY REV: Forwarded topic='{}' size={}", 
                                           topic_name, total_len);
            } catch (const std::exception& e) {
                spdlog::get("illixr")->error("[tcp_minimal_backend] Reverse forward error: {}", e.what());
                running_ = false;
                break;
            }
        }
        
        // Small delay if no data
        if (chunk.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    spdlog::get("illixr")->info("[tcp_minimal_backend] Reverse path loop terminated");
}

void tcp_minimal_backend::topic_create(std::string topic_name, network::topic_config& config) {
    networked_topics_.push_back(topic_name);
    networked_topics_configs_[topic_name] = config;
    
    std::string serialization = (config.serialization_method == network::topic_config::SerializationMethod::BOOST)
                                ? "BOOST" : "PROTOBUF";
    
    std::string message = "create_topic" + topic_name + delimiter_ + serialization;
    send_to_peer("illixr_control", std::move(message));
    
    spdlog::get("illixr")->info("[tcp_minimal_backend] Created topic '{}' with {} serialization", 
                                topic_name, serialization);
}

bool tcp_minimal_backend::is_topic_networked(std::string topic_name) {
    return std::find(networked_topics_.begin(), networked_topics_.end(), topic_name) != networked_topics_.end();
}

void tcp_minimal_backend::topic_send(std::string topic_name, std::string&& message) {
    if (!is_topic_networked(topic_name)) {
        spdlog::get("illixr")->warn("[tcp_minimal_backend] Topic '{}' not networked, skipping", topic_name);
        return;
    }

    spdlog::get("illixr")->debug("[tcp_minimal_backend] Sending topic='{}' size={}", 
                                 topic_name, message.size());
    send_to_peer(topic_name, std::move(message));
}

void tcp_minimal_backend::topic_receive(const std::string& topic_name, std::vector<char>& message) {
    if (topic_name == "illixr_control") {
        std::string message_str(message.begin(), message.end());
        
        if (message_str.find("create_topic") == 0) {
            size_t d_pos = message_str.find(delimiter_);
            if (d_pos == std::string::npos) {
                spdlog::get("illixr")->error("[tcp_minimal_backend] Invalid control message format");
                return;
            }
            
            std::string l_topic_name = message_str.substr(12, d_pos - 12);
            std::string serialization = message_str.substr(d_pos + 1);
            
            networked_topics_.push_back(l_topic_name);
            network::topic_config config;
            config.serialization_method = (serialization == "BOOST") 
                                         ? network::topic_config::SerializationMethod::BOOST
                                         : network::topic_config::SerializationMethod::PROTOBUF;
            
            networked_topics_configs_[l_topic_name] = config;
            spdlog::get("illixr")->info("[tcp_minimal_backend] Received create_topic for '{}'", l_topic_name);
        }
        return;
    }

    if (!switchboard_->topic_exists(topic_name)) {
        spdlog::get("illixr")->warn("[tcp_minimal_backend] Topic '{}' does not exist locally", topic_name);
        return;
    }

    switchboard_->get_topic(topic_name).deserialize_and_put(message, networked_topics_configs_[topic_name]);
}

void tcp_minimal_backend::send_to_peer(const std::string& topic_name, std::string&& message) {
    if (!peer_socket_) {
        spdlog::get("illixr")->error("[tcp_minimal_backend] peer_socket is NULL, cannot send");
        return;
    }

    uint32_t total_len = 8 + topic_name.size() + message.size();
    uint32_t topic_len = topic_name.size();

    std::string packet;
    packet.reserve(total_len);
    packet.append(reinterpret_cast<const char*>(&total_len), 4);
    packet.append(reinterpret_cast<const char*>(&topic_len), 4);
    packet.append(topic_name);
    packet.append(message);

    try {
        peer_socket_->write_data(packet);
    } catch (const std::exception& e) {
        spdlog::get("illixr")->error("[tcp_minimal_backend] Send error: {}", e.what());
    }
}

void tcp_minimal_backend::stop() {
    spdlog::get("illixr")->info("[tcp_minimal_backend] Stopping...");
    running_ = false;
    
    if (forward_socket_) {
        delete forward_socket_;
        forward_socket_ = nullptr;
    }
    
    if (peer_socket_) {
        delete peer_socket_;
        peer_socket_ = nullptr;
    }
    
    spdlog::get("illixr")->info("[tcp_minimal_backend] Stopped");
}

extern "C" plugin* this_plugin_factory(phonebook* pb) {
    auto plugin_ptr = std::make_shared<tcp_minimal_backend>("tcp_minimal_backend", pb);
    pb->register_impl<network::network_backend>(plugin_ptr);
    return plugin_ptr.get();
}
