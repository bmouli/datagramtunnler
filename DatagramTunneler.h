#pragma once
#include <cstdint>
#include <assert.h>
#include <string>
#include "log.h"

class DatagramTunneler {
	struct Config 
	{
		bool            is_client_;
        std::string     udp_iface_ip_;
        std::string     tcp_iface_ip_;
        uint16_t        tcp_srv_port_;
        std::string     udp_dst_ip_;    
        uint16_t        udp_dst_port_;

        std::string     tcp_srv_ip_;

        bool            use_clt_grp_;
        
        Config(); 

        bool isComplete() const;
    };

    DatagramTunneler(Config cfg);   
    ~DatagramTunneler();            
    void run();

    void setupClient(const Config& cfg);
    void runClient();
    
    void setupServer(const Config& cfg);
    void runServer();

    
    Config          cfg_;
    int             udp_socket_;    
    int             tcp_socket_;

};