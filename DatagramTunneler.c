#include "DatagramTunneler.h"
#include <sys/socket.h>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <cstdint>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifndef MSG_NOSIGNAL

#define MSG_NOSIGNAL 0 
#endif

static const int        NO_FLAGS = 0;
static const int        MAX_DGRAM_LEN = 1472;       
static const int        HEARTBEAT_PERIOD_SEC = 5;   
enum TunnelPktType : uint8_t {
    HEARTBEAT = 0,
    DATAGRAM = 1
};

#pragma pack(push,1)
struct TunnelPacket { 
    TunnelPktType   type_;                     
    uint32_t        udp_dst_ip_;              
    uint16_t        udp_dst_port_;            
    uint16_t        datalen_; 
    char            databuf_[MAX_DGRAM_LEN]; 
    
    size_t size() const {
        if (type_ == TunnelPktType::HEARTBEAT) {
            return 1;
        } else {
            return static_cast<size_t>(datalen_ + 9);
        }
    }
};
static_assert(sizeof(TunnelPacket) == 1481, "The TunnelPacket struct should be 1481 bytes long!");
#pragma pack(pop)

static bool sendTCPData(int tcp_socket, const void* data, size_t datalen, int flags) {
    size_t len_to_send = datalen;
    const char* p = reinterpret_cast<const char*>(data);
    do {
        ssize_t len_sent = 0;
        if((len_sent = send(tcp_socket, p, len_to_send, flags)) < 0) { 
            return false;
        }
        if (len_sent == 0) {
            DEATH("Unable to send data via TCP. The server might not be able to keep up!"); 
        }
        assert(len_to_send >= static_cast<size_t>(len_sent));
        len_to_send -= static_cast<size_t>(len_sent);
        p += len_sent;
    } while (len_to_send != 0);
    return true;
}

DatagramTunneler::Config::Config() : is_client_(false), udp_iface_ip_(), tcp_iface_ip_(), tcp_srv_port_(0),
                                     udp_dst_ip_(), udp_dst_port_(0), tcp_srv_ip_(), use_clt_grp_(false) {}

bool DatagramTunneler::Config::isComplete() const {
    if (udp_iface_ip_.empty() ||
        //tcp_iface_ip_.empty() ||
        tcp_srv_port_ == 0) {
        return false;
    }
    if (is_client_) {
        if (tcp_srv_ip_.empty() || udp_dst_ip_.empty() || udp_dst_port_ == 0) {
            return false;
        }
    } else if (!use_clt_grp_ && (udp_dst_ip_.empty() || udp_dst_port_ == 0)) {
        return false;
    }
    return true;
}

DatagramTunneler::DatagramTunneler(Config cfg) : cfg_(cfg) {
    if (cfg.is_client_) {
        setupClient(cfg);
    } else { 
        setupServer(cfg);
    }
}

DatagramTunneler::~DatagramTunneler() {
}

void DatagramTunneler::run() {
    if (cfg_.is_client_) {
        runClient();
    } else {
        runServer();
    }
}

void DatagramTunneler::setupClient(const Config& cfg) {
    
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, NO_FLAGS);
    if (udp_socket_ < 0) {
        DEATH("Could not create UDP socket! Error %d", errno);
    }

    
    struct timeval tv {HEARTBEAT_PERIOD_SEC, 0};
    if (setsockopt(udp_socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        DEATH("Could not set receive timeout on UDP socket! Error %d", errno);
    }

    
    sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(cfg.udp_dst_port_);
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    if(bind(udp_socket_, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
        DEATH("Could not bind UDP socket to port %u!", cfg.udp_dst_port_);
    }

    // Creating TCP socket
    tcp_socket_ = socket(AF_INET , SOCK_STREAM , NO_FLAGS);
    if (tcp_socket_ < 0) {
        DEATH("Could not create TCP socket!");
    }
	#ifdef __APPLE__
    int set = 1;
    if (setsockopt(tcp_socket_, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set)) < 0) {
        DEATH("Could not prevent TCP socket to send SIGPIPE on disconnect! Error %d", errno);
    }
#endif
}

void DatagramTunneler::runClient() {
    
    sockaddr_in server_addr;
    
    server_addr.sin_addr.s_addr = inet_addr(cfg_.tcp_srv_ip_.c_str());
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(cfg_.tcp_srv_port_);
    if (connect(tcp_socket_, reinterpret_cast<struct sockaddr *>(&server_addr), sizeof(server_addr)) < 0) {
        DEATH("Unable to connect to server %s:%u. Error %d!", cfg_.tcp_srv_ip_.c_str(), cfg_.tcp_srv_port_, errno);
    }
    INFO("[DatagramTunneler][CLIENT-MODE] connected to TCP remote %s:%u and listening to multicast %s:%u", 
    cfg_.tcp_srv_ip_.c_str(), cfg_.tcp_srv_port_, cfg_.udp_dst_ip_.c_str(), cfg_.udp_dst_port_);

    
    ip_mreq udp_group;
    udp_group.imr_multiaddr.s_addr = inet_addr(cfg_.udp_dst_ip_.c_str());
    udp_group.imr_interface.s_addr = inet_addr(cfg_.udp_iface_ip_.c_str());
    if(setsockopt(udp_socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &udp_group, sizeof(udp_group)) < 0) {
        DEATH("Cannot join multicast group %s:%u. Error %d", cfg_.udp_dst_ip_.c_str(), cfg_.udp_dst_port_, errno);
    }
    INFO("Joined multicast group %s:%u.", cfg_.udp_dst_ip_.c_str(), cfg_.udp_dst_port_);

    
    TunnelPacket tunnel_pkt;
    inet_pton(AF_INET, cfg_.udp_dst_ip_.c_str(), &tunnel_pkt.udp_dst_ip_);
    tunnel_pkt.udp_dst_port_ = cfg_.udp_dst_port_;
    ssize_t len_read = 0;
    
    while (true) {
        
        if((len_read = recv(udp_socket_, tunnel_pkt.databuf_, MAX_DGRAM_LEN, MSG_TRUNC)) < 0) {
            
            if (errno != EAGAIN) {
                DEATH("Unable to read data from UDP socket %d", errno);
            }
            tunnel_pkt.type_ = TunnelPktType::HEARTBEAT;
            INFO("Sending a heartbeat to server.");
        } else {
            assert(len_read <= MAX_DGRAM_LEN);
            if (len_read > MAX_DGRAM_LEN) { 
                WARN("Discarding jumbo datagram of %zd bytes!", len_read);
                continue;
            }
            tunnel_pkt.type_ = TunnelPktType::DATAGRAM;
            INFO("Tunneling a %zd byte datagram to server.", len_read);
            tunnel_pkt.datalen_ = static_cast<uint16_t>(len_read);
        }

        
        if(!sendTCPData(tcp_socket_, &tunnel_pkt, tunnel_pkt.size(), NO_FLAGS)) {
            DEATH("Unable to send data to server! Error %d", errno);
        }
    }
}

void DatagramTunneler::setupServer(const Config &cfg) {
    
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, NO_FLAGS);
    if (udp_socket_ < 0) {
        DEATH("Could not create UDP socket! Error %d", errno);
    }
    
    
    in_addr iface;
    iface.s_addr = inet_addr(cfg_.udp_iface_ip_.c_str());
    if(setsockopt(udp_socket_, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface)) < 0) {
        DEATH("Could not set UDP publisher interface to %s! Error %d", cfg_.udp_iface_ip_.c_str(), errno);
    }
    
    
    tcp_socket_ = socket(AF_INET , SOCK_STREAM , NO_FLAGS);
    if (tcp_socket_ < 0) {
        DEATH("Could not create TCP socket! Error %d", errno);
    }
    
    
    sockaddr_in tcp_iface;
    tcp_iface.sin_family = AF_INET;
    tcp_iface.sin_port = htons (cfg_.tcp_srv_port_);
    tcp_iface.sin_addr.s_addr = htonl(INADDR_ANY); 
    if(bind(tcp_socket_, reinterpret_cast<sockaddr*>(&tcp_iface), sizeof(tcp_iface)) < 0) {
        DEATH("Could not bind TCP socket to port %u! Error %d", cfg.tcp_srv_port_, errno);
    }
}

void DatagramTunneler::runServer() {
    INFO("[DatagramTunneler][SERVER MODE] listening for client connection on port %u...", cfg_.tcp_srv_port_);
    
    if (listen(tcp_socket_, 1) < 0) {
        DEATH("Unable to listen on TCP port %u, error %d!", cfg_.tcp_srv_port_, errno);
    }
    sockaddr remote;
    socklen_t sosize  = sizeof(remote);
    int new_fd = accept(tcp_socket_, &remote, &sosize);
    if (new_fd < 0) {
        DEATH("Unable to accept incoming TCP connection, accept() error %d!", errno);
    }
    INFO("Accepted incoming connection from a remote host. Waiting for forwarded datagrams...");

    
    struct timeval tv {HEARTBEAT_PERIOD_SEC + 1, 0};
    if (setsockopt(new_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        DEATH("Could not set receive timeout on TCP socket! Error %d", errno);
    }

    sockaddr_in pub_group;
    if (!cfg_.use_clt_grp_) { 
        memset(&pub_group, 0, sizeof(pub_group));
        pub_group.sin_family = AF_INET; 
        pub_group.sin_port = htons(cfg_.udp_dst_port_);
        pub_group.sin_addr.s_addr = inet_addr(cfg_.udp_dst_ip_.c_str());
    }

    TunnelPacket tunnel_pkt;
    char *p = reinterpret_cast<char*>(&tunnel_pkt); 
    ssize_t len_read = 0;
    size_t len_to_read = 1;
    while(true) {
        assert(len_to_read != 0);
        
        if ((len_read = recv(new_fd, p, len_to_read, NO_FLAGS)) < 0) {
            if (errno == EAGAIN) {
                INFO("Client has been silent for too long. Terminating");
                exit(0);
            } else {
                DEATH("Unable to read data from TCP socket, recv() error %d!", errno);
            }
        }
        if (len_read == 0) {
            INFO("Client terminated!"); 
            exit(0);
        }
        if (tunnel_pkt.type_ == TunnelPktType::HEARTBEAT) {
            INFO("Received heartbeat from client.");
            continue;
        }
        p += len_read;
        assert(p > reinterpret_cast<char*>(&tunnel_pkt));
        const size_t data_len = static_cast<size_t>(p - reinterpret_cast<char*>(&tunnel_pkt));
        if (data_len < 9) {
            len_to_read = 9 - data_len;
            continue; 
        }
        if (data_len < tunnel_pkt.size()) {
            len_to_read = tunnel_pkt.size() - data_len;
            continue; 
        }
        assert(data_len == tunnel_pkt.size()); 
        p = reinterpret_cast<char*>(&tunnel_pkt); 
        len_to_read = 1;

        if (cfg_.use_clt_grp_) { 
            memset(&pub_group, 0, sizeof(pub_group));
            pub_group.sin_family = AF_INET; 
            pub_group.sin_port = htons(tunnel_pkt.udp_dst_port_);
            pub_group.sin_addr.s_addr = tunnel_pkt.udp_dst_ip_;
        }
    
        
        if(sendto(udp_socket_, tunnel_pkt.databuf_, tunnel_pkt.datalen_, MSG_NOSIGNAL, reinterpret_cast<struct sockaddr*>(&pub_group), sizeof(pub_group)) < 0) {
            DEATH("Unable to publish multicast data, sendto() error %d!", errno);
        }

        
        char clt_grp_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &tunnel_pkt.udp_dst_ip_, clt_grp_ip, INET_ADDRSTRLEN);

        
        char pub_grp_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &pub_group.sin_addr, pub_grp_ip, INET_ADDRSTRLEN);
        INFO("Published to %s:%u a %u byte datagram tunneled by client. Client side group was %s:%u",
        pub_grp_ip, ntohs(pub_group.sin_port), tunnel_pkt.datalen_, clt_grp_ip, tunnel_pkt.udp_dst_port_);
    }
}
