#ifdef _WIN32

#include <mutex>
#include <list>
#include <vector>

#include "adapter.h"

#include <iphlpapi.h>
#pragma comment(lib, "IPHLPAPI.lib")

#include <netlistmgr.h>
#include <atlcomcli.h>

#include "guidparse.h"
#include "wintunshim.h"

#include <iostream>

constexpr const GUID adapterGUID = guid_parse::make_guid("{F8D2D65B-7012-4602-805E-FD00529352D9}");

volatile bool interrupted = false;

void setCategory()
{
	CoInitialize(NULL);
	HRESULT hr = S_OK;
	CComPtr<INetworkListManager> pLocalNLM;
	CComPtr<IEnumNetworks> pEnumNetworks;
	hr = CoCreateInstance(CLSID_NetworkListManager, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pLocalNLM));
	if (hr != S_OK) {
		CoUninitialize();
		return;
	}
	hr = pLocalNLM->GetNetworks(NLM_ENUM_NETWORK_ALL, &pEnumNetworks);
	if (hr != S_OK) {
		CoUninitialize();
		return;
	}
	while (1)
	{
		CComPtr<INetwork> pNetwork;
		hr = pEnumNetworks->Next(1, &pNetwork, NULL);
		if (hr == S_OK)
		{
			BSTR name = NULL;
			pNetwork->GetName(&name);
			if (wcscmp(name, L"LAN Party VPN") == 0) {
				pNetwork->SetCategory(NLM_NETWORK_CATEGORY_PRIVATE);
			}
				
		}
		else
			break;
	}
	CoUninitialize();
}

class WintunAdapter {
	HMODULE wintun = 0;
	WINTUN_ADAPTER_HANDLE adapterHandle = 0;
	WINTUN_SESSION_HANDLE sessionHandle = 0;
	ULONG ipContext = 0;
	ULONG ipInstance = 0;
	cidr::CIDR currentAddr = { 0 };
	NET_LUID adapterLUID = { 0 };

	public:
	WintunAdapter() {
		wintun = InitializeWintun();
		GUID guid = {};
		BOOL rebootRequired = false;
		std::cerr << WintunOpenAdapter << std::endl;
		adapterHandle = WintunOpenAdapter(L"LAN Party VPN", L"LAN Party VPN");
		if (adapterHandle) {
			// always close existing adapter to clear IP addresses associated.
			WintunDeleteAdapter(adapterHandle, true, &rebootRequired);
		}
		adapterHandle = WintunCreateAdapter(L"LAN Party VPN", L"LAN Party VPN", &adapterGUID, &rebootRequired);
		if (!adapterHandle) {
			throw std::runtime_error("Cannot Create Adapter");
		}
		if (rebootRequired) {
			throw std::runtime_error("Reboot Required to Create a Network Adapter.");
		}
		sessionHandle = WintunStartSession(adapterHandle, 0x400000);
		if (!sessionHandle) {
			WintunDeleteAdapter(adapterHandle, true, &rebootRequired);
			adapterHandle = 0;
			throw std::runtime_error("Cannot Create Session");
		}
		WintunGetAdapterLUID(adapterHandle, &adapterLUID);
	}

	~WintunAdapter() {
		BOOL rebootRequired = false;
		if (sessionHandle) {
			WintunEndSession(sessionHandle);
			sessionHandle = 0;
		}
		
		if (adapterHandle) {
			WintunDeleteAdapter(adapterHandle, true, &rebootRequired);
			adapterHandle = 0;
		}
	}

	std::vector<uint8_t> read()
	{
		std::vector<uint8_t> data;
		if (!sessionHandle) {
			return data;
		}
		DWORD incomingPacketSize;
		BYTE *incomingPacket = WintunReceivePacket(sessionHandle, &incomingPacketSize);
		if (incomingPacket)
		{
			// copy the packet to a buffer
			data.assign(incomingPacket, incomingPacket + incomingPacketSize);
			WintunReleaseReceivePacket(sessionHandle, incomingPacket);
		}
		return data;
	}

	void write(std::vector<uint8_t> &data)
	{
		if (!sessionHandle) {
			return;
		}
		auto wintunPacket = WintunAllocateSendPacket(sessionHandle, (DWORD)data.size());
		memcpy(wintunPacket, &data[0], data.size());
		WintunSendPacket(sessionHandle, wintunPacket);
	}

	void setIP(cidr::CIDR &addr)
	{
		if (addr == currentAddr) {
			return;
		}
		if (ipContext) {
			DeleteIPAddress(ipContext);
		}
		NET_IFINDEX ifindex;
		auto ret = ConvertInterfaceLuidToIndex(&adapterLUID, &ifindex);
		if (ret == ERROR_INVALID_PARAMETER) {
			return;
		}
		ret = AddIPAddress(addr.addr.S_un.S_addr, addr.mask().S_un.S_addr, ifindex, &ipContext, &ipInstance);
		if (ret != NO_ERROR) {
			throw std::system_error(std::error_code(ret, std::system_category()));
		}
		currentAddr = addr;
		setCategory();
	}

};

lpvpn::adapter::Adapter::Adapter() {
	this->impl = std::make_shared<WintunAdapter>();
}

std::vector<uint8_t> lpvpn::adapter::Adapter::read() {
	return std::static_pointer_cast<WintunAdapter>(impl)->read();
}

void lpvpn::adapter::Adapter::write(std::vector<uint8_t> &data) {
	std::static_pointer_cast<WintunAdapter>(impl)->write(data);
}

void lpvpn::adapter::Adapter::setIP(cidr::CIDR &addr) {
	std::static_pointer_cast<WintunAdapter>(impl)->setIP(addr);
}

#endif
