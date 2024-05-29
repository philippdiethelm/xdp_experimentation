#include <memory>
#include <format>
#include <iostream>

#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

class WinsockHelper {
  public:
    ~WinsockHelper() { WSACleanup(); };

    WinsockHelper()
    {
        WSADATA wsaData = {0};

        auto result = WSAStartup(MAKEWORD(2, 2), &wsaData);

        if (result != 0)
            throw std::format("WSAStartup failed with error {}", result);
    }

  public:
    static void JoinGroup(SOCKET sd, in_addr multicast_address, in_addr interface_address)
    {
        struct ip_mreq imr = {
            0,
        };

        imr.imr_interface = interface_address;
        imr.imr_multiaddr = multicast_address;

        if (setsockopt(sd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&imr, sizeof(imr)) != 0)
            throw std::format("join_group failed with error {}", WSAGetLastError());
    }

    static void LeaveGroup(SOCKET sd, in_addr multicast_address, in_addr interface_address)
    {
        struct ip_mreq imr = {
            0,
        };

        imr.imr_interface = interface_address;
        imr.imr_multiaddr = multicast_address;

        setsockopt(sd, IPPROTO_IP, IP_DROP_MEMBERSHIP, (char*)&imr, sizeof(imr));
    }

    static std::string to_string(in_addr in4)
    {
        char str[128];
        if (inet_ntop(AF_INET, &in4, str, sizeof(str)) != nullptr) {
            return std::string(str, strlen(str));
        }
        return "(unknown)";
    }

    static std::string to_string(sockaddr_gen sa)
    {
        void* pAddr = nullptr;

        switch (sa.Address.sa_family) {
            case AF_INET:
                pAddr = &sa.AddressIn.sin_addr;
                break;

            case AF_INET6:
                pAddr = &sa.AddressIn6.sin6_addr;
                break;

            default:
                return "(unknown)";
        }

        char str[256];
        if (inet_ntop(sa.Address.sa_family, pAddr, str, sizeof(str)) != nullptr) {
            return std::string(str, strlen(str));
        }
        return "(unknown)";
    }

    static std::string to_string(sockaddr sa) { return to_string((sockaddr_gen)sa); }
};

static WinsockHelper helper;

void JoinGroupOnAllInterfaces(const char* group_address = "224.0.0.200")
{
    auto temp_socket = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, 0);

    in_addr ia_group = {
        0,
    };
    inet_pton(AF_INET, group_address, &ia_group);

    DWORD interface_list_entries = 16;
    auto interface_list = std::make_unique<INTERFACE_INFO[]>(interface_list_entries);
    DWORD interface_list_size = interface_list_entries * sizeof(INTERFACE_INFO);
    ULONG bytes_returned = 0;

    std::cout << "JoinGroupOnAllInterfaces() START" << std::endl;

    if (auto status = WSAIoctl(
            temp_socket, SIO_GET_INTERFACE_LIST, 0, 0, &interface_list[0], interface_list_size, &bytes_returned, 0, 0);
        status != NO_ERROR) {
        closesocket(temp_socket);
        std::cout << "Failed to get interface list! WSAIoctl error " << WSAGetLastError() << std::endl;
    }

    // 3. Use SetSockOpt() with SO_REUSEADDR against the listening socket.
    //    This is required to be able to Bind() and Connect() to the same socket.
    const int enable = 1;

    if (auto result = setsockopt(temp_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&enable, sizeof(enable));
        result < 0)
        throw "setsockopt(SO_REUSEADDR) failed!";

    int joined_successfully = 0;
    for (int i = 0; i < bytes_returned / sizeof(INTERFACE_INFO); ++i) {
        try {
            WinsockHelper::JoinGroup(temp_socket, ia_group, interface_list[i].iiAddress.AddressIn.sin_addr);
            joined_successfully++;
            std::cout << "Successfully joined group on interface "
                      << WinsockHelper::to_string(interface_list[i].iiAddress) << std::endl;
        } catch (...) {
            std::cout << "Could not join group on interface " << WinsockHelper::to_string(interface_list[i].iiAddress)
                      << std::endl;
        }
    }

    std::cout << "Successfully joined group on " << joined_successfully << " interfaces " << std::endl;
}
