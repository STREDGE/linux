package main

// Android 11+ не даёт приложениям перечислять сетевые интерфейсы (net.Interfaces падает).
// Приложение Tailscale отдаёт список из Java; у нас Java нет — делаем как feature/androidbin
// из tailscale.com: один «синтетический» интерфейс с адресами, которые ядро выбирает для
// исходящего UDP (без отправки пакетов). Этого хватает, чтобы находить прямой путь.

import (
	"errors"
	"net"
	"net/netip"
	"time"

	"tailscale.com/net/netmon"
)

func init() {
	netmon.RegisterInterfaceGetter(func() ([]netmon.Interface, error) {
		if ifs, err := net.Interfaces(); err == nil && len(ifs) > 0 {
			ret := make([]netmon.Interface, len(ifs))
			for i := range ifs {
				ret[i] = netmon.Interface{Interface: &ifs[i]}
			}
			return ret, nil
		}
		return synthetic()
	})
}

func synthetic() ([]netmon.Interface, error) {
	var addrs []net.Addr
	if ip, ok := outboundIP("udp4", "8.8.8.8:53"); ok {
		addrs = append(addrs, &net.IPNet{IP: ip.AsSlice(), Mask: net.CIDRMask(32, 32)})
	}
	if ip, ok := outboundIP("udp6", "[2001:4860:4860::8888]:53"); ok {
		addrs = append(addrs, &net.IPNet{IP: ip.AsSlice(), Mask: net.CIDRMask(128, 128)})
	}
	if len(addrs) == 0 {
		return nil, errors.New("нет сети")
	}
	return []netmon.Interface{{
		Interface: &net.Interface{Index: 1, MTU: 1500, Name: "android", Flags: net.FlagUp | net.FlagRunning},
		AltAddrs:  addrs,
	}}, nil
}

func outboundIP(network, addr string) (netip.Addr, bool) {
	d := net.Dialer{Timeout: 2 * time.Second}
	c, err := d.Dial(network, addr)
	if err != nil {
		return netip.Addr{}, false
	}
	defer c.Close()
	ua, ok := c.LocalAddr().(*net.UDPAddr)
	if !ok {
		return netip.Addr{}, false
	}
	ip, ok := netip.AddrFromSlice(ua.IP)
	if !ok {
		return netip.Addr{}, false
	}
	ip = ip.Unmap()
	if ip.IsLoopback() || ip.IsUnspecified() {
		return netip.Addr{}, false
	}
	return ip, true
}
