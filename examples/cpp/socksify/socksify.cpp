// socksify.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include "pch.h"
#include <iostream>

int main()
{
	std::wstring app_name_w;
	net::ip_address_v4 local_ip_address;
	uint16_t local_proxy_port;
	uint16_t socks5_server_port;
	std::unordered_map<unsigned short, std::pair<net::ip_address_v4, unsigned short>> mapper;
	std::mutex mapper_lock;

	WSADATA wsa_data;
	const auto version_requested = MAKEWORD(2, 2);

	if (::WSAStartup(version_requested, &wsa_data) != 0)
	{
		std::cout << "WSAStartup failed with error\n";
		return 1;
	}
	
	auto ndis_api = std::make_unique<ndisapi::fastio_packet_filter>(
		nullptr,
		[&app_name_w, &local_proxy_port, &mapper, &mapper_lock](HANDLE adapter_handle, INTERMEDIATE_BUFFER& buffer)
		{
			thread_local ndisapi::local_redirector redirect{ local_proxy_port };

			const auto ether_header = reinterpret_cast<ether_header_ptr>(buffer.m_IBuffer);

			if (ntohs(ether_header->h_proto) == ETH_P_IP)
			{
				const auto ip_header = reinterpret_cast<iphdr_ptr>(ether_header + 1);

				if (ip_header->ip_p == IPPROTO_TCP)
				{
					const auto tcp_header = reinterpret_cast<tcphdr_ptr>(reinterpret_cast<PUCHAR>(ip_header) + sizeof(DWORD)*ip_header->ip_hl);

					auto process = process_lookup<net::ip_address_v4>::get_process_helper().
						lookup_process_for_tcp<false>(ip_session<net::ip_address_v4>{ip_header->ip_src, ip_header->ip_dst, ntohs(tcp_header->th_sport), ntohs(tcp_header->th_dport)});

					if (!process)
					{
						process_lookup<net::ip_address_v4>::get_process_helper().actualize(true, false);
						process = process_lookup<net::ip_address_v4>::get_process_helper().
							lookup_process_for_tcp<true>(ip_session<net::ip_address_v4>{ip_header->ip_src, ip_header->ip_dst, ntohs(tcp_header->th_sport), ntohs(tcp_header->th_dport)});
					}

					if (process->name.find(app_name_w) != std::wstring::npos)
					{
						if(tcp_header->th_flags == TH_SYN)
						{
							std::lock_guard<std::mutex> lock(mapper_lock);
							mapper[ntohs(tcp_header->th_sport)] = std::make_pair(net::ip_address_v4(ip_header->ip_dst), ntohs(tcp_header->th_dport));
						}
						
						if(redirect.process_client_to_server_packet(buffer))
						{
							CNdisApi::RecalculateTCPChecksum(&buffer);
							CNdisApi::RecalculateIPChecksum(&buffer);
							buffer.m_dwDeviceFlags = PACKET_FLAG_ON_RECEIVE;
						}
					}
					else if(ntohs(tcp_header->th_sport) == redirect.get_proxy_port())
					{
						if (redirect.process_server_to_client_packet(buffer))
						{
							CNdisApi::RecalculateTCPChecksum(&buffer);
							CNdisApi::RecalculateIPChecksum(&buffer);
							buffer.m_dwDeviceFlags = PACKET_FLAG_ON_RECEIVE;
						}
					}
				}
			}

			return ndisapi::packet_action::pass;
		}, false);

	if (ndis_api->IsDriverLoaded())
	{
		std::cout << "WinpkFilter is loaded" << std::endl << std::endl;
	}
	else
	{
		std::cout << "WinpkFilter is not loaded" << std::endl << std::endl;
		return 1;
	}

	std::cout << "Available network interfaces:" << std::endl << std::endl;
	size_t index = 0;
	for (auto& e : ndis_api->get_interface_names_list())
	{
		std::cout << ++index << ")\t" << e << std::endl;
	}

	std::cout << std::endl << "Select interface to filter:";
	std::cin >> index;

	if (index > ndis_api->get_interface_names_list().size())
	{
		std::cout << "Wrong parameter was selected. Out of range." << std::endl;
		return 0;
	}

	std::cout << std::endl << "Application name to proxify:";
	std::wcin >> app_name_w;

	std::cout << std::endl << "Local proxy server port:";
	std::cin >> local_proxy_port;

	std::cout << std::endl << "Local SOCKS5 proxy port:";
	std::cin >> socks5_server_port;

	winsys::io_completion_port io_port;

	io_port.start_thread_pool();

	proxy::tcp_proxy_server<proxy::socks5_tcp_proxy_socket<net::ip_address_v4>> proxy(
		local_proxy_port, io_port, [&socks5_server_port, &mapper, &mapper_lock](
		net::ip_address_v4 address, const uint16_t port)-> std::tuple<net::ip_address_v4, uint16_t, std::unique_ptr<proxy::tcp_proxy_server<proxy::
		socks5_tcp_proxy_socket<net::ip_address_v4>>::negotiate_context_t>>
	{

		std::lock_guard<std::mutex> lock(mapper_lock);
		
		const auto it = mapper.find(port);

		if (it != mapper.end())
		{
			std::cout << "Redirect entry was found for the port " << port << " is " << it->second.first << ":" << it->second.second << "\n";

			auto remote_address = it->second.first;
			auto remote_port = it->second.second;

			mapper.erase(it);

			return std::make_tuple(net::ip_address_v4("127.0.0.1"), socks5_server_port,
				std::make_unique<proxy::socks5_tcp_proxy_socket<net::ip_address_v4>::negotiate_context_t>(
					remote_address, remote_port));
		}
		
		return std::make_tuple(net::ip_address_v4{}, 0, nullptr);
	});

	ndis_api->start_filter(index - 1);

	proxy.start();

	std::cout << "Press any key to stop filtering" << std::endl;

	std::ignore = _getch();

	io_port.stop_thread_pool();
	
	::WSACleanup();

	std::cout << "Exiting..." << std::endl;

	return 0;
}

