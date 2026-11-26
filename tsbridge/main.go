// Встроенный Tailscale для SA Coop: своя сеть игры без отдельного приложения.
//
// Телефон входит в аккаунт Tailscale один раз (ссылка открывается в браузере), дальше
// узел живёт внутри игры под именем «sacoop-<ID>». Хост держит «точку» — UDP-порт в сети
// Tailscale; друг по ID находит хоста среди устройств аккаунта и подключается.
//
// Игра (C++) общается с сетью как раньше — обычным UDP-сокетом, только на 127.0.0.1:
// мост пересылает пакеты между этим портом и сетью Tailscale (userspace, без VPN).
//
// Собирается в статическую библиотеку для arm64 Android: tools/tsbridge/build.sh.
package main

/*
#include <stdlib.h>
#include <string.h>
*/
import "C"

import (
	"context"
	"fmt"
	"net"
	"net/netip"
	"os"
	"path/filepath"
	"runtime"
	"runtime/debug"
	"strings"
	"sync"
	"time"
	"unsafe"

	"tailscale.com/client/local"
	"tailscale.com/ipn"
	"tailscale.com/tsnet"
)

var (
	logMu    sync.Mutex
	mu       sync.Mutex
	srv      *tsnet.Server
	lc       *local.Client
	started  bool
	failed   bool // tsnet не запустился — в меню «ошибка», а не вечное «подключается»
	useKey   bool // вход по ключу из кода приглашения: браузер не нужен, интерактивный вход не просим
	loginErr string // последняя ошибка входа от Tailscale (для меню)
	pendKey  string // ключ пришёл, пока tsnet ещё стартует — применить, когда будет LocalClient
	lastErr  string
	logFile  *os.File
	logBytes int64

	// мост
	tsConn    net.PacketConn // в сети Tailscale
	loop      *net.UDPConn   // 127.0.0.1:<порт моста> — сюда шлёт игра
	gameAddr  *net.UDPAddr   // сокет игры (хост — известен заранее, друг — по первому пакету)
	peer      netip.AddrPort // второй телефон в сети Tailscale
	tsPortNow int
	bridgeGen int

	// статус (обновляется раз в секунду)
	stState, stURL, stIP, stName string
	stPeers                     []peerInfo
	loginAsked                  time.Time
)

type peerInfo struct {
	host   string
	ip     string
	online bool
}

func logf(format string, args ...any) {
	logMu.Lock()
	defer logMu.Unlock()
	if logFile == nil {
		return
	}
	line := time.Now().Format("15:04:05.000 ") + fmt.Sprintf(format, args...)
	if !strings.HasSuffix(line, "\n") {
		line += "\n"
	}
	if logBytes > 4<<20 { // не больше ~4 МБ: начать файл заново
		logFile.Truncate(0)
		logFile.Seek(0, 0)
		logBytes = 0
	}
	n, _ := logFile.WriteString(line)
	logBytes += int64(n)
}

// Переменная окружения → папка (создать). force — даже если уже задана.
func setDirEnv(key, path string, force bool) {
	if !force && os.Getenv(key) != "" {
		return
	}
	if err := os.MkdirAll(path, 0o700); err != nil {
		logf("папка %s (%s): %v", path, key, err)
		return
	}
	os.Setenv(key, path)
	logf("%s=%s", key, path)
}

// Паника в нашем коде не должна ронять игру: записать в журнал и жить дальше.
// (Падения внутри tsnet ловит SetCrashOutput — текст в tailscale_crash.txt.)
func guard(where string, rc *C.int) {
	if r := recover(); r != nil {
		logf("ПАНИКА в %s: %v\n%s", where, r, debug.Stack())
		if rc != nil {
			*rc = -1
		}
	}
}

// Журналы — рядом с подробным логом коопа (игрок может их прислать); прошлый — в *_prev.txt
func openLogs(dir string) {
	if dir == "" {
		return
	}
	os.MkdirAll(dir, 0o777)
	p := filepath.Join(dir, "tailscale_log.txt")
	if st, err := os.Stat(p); err == nil && st.Size() > 0 {
		os.Rename(p, filepath.Join(dir, "tailscale_log_prev.txt"))
	}
	logMu.Lock()
	logFile, _ = os.OpenFile(p, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o666)
	logBytes = 0
	logMu.Unlock()
	cp := filepath.Join(dir, "tailscale_crash.txt")
	if st, err := os.Stat(cp); err == nil && st.Size() > 1<<20 {
		os.Remove(cp)
	}
	if cf, err := os.OpenFile(cp, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o666); err == nil {
		cf.WriteString("\n===== запуск Tailscale " + time.Now().Format("2006-01-02 15:04:05") + " =====\n")
		debug.SetTraceback("all")
		if err := debug.SetCrashOutput(cf, debug.CrashOptions{}); err != nil {
			logf("SetCrashOutput: %v", err)
		}
		cf.Close() // SetCrashOutput держит свою копию
	}
}

func cstr(p *C.char) string {
	if p == nil {
		return ""
	}
	return C.GoString(p)
}

func put(buf *C.char, n C.int, s string) C.int {
	if buf == nil || n <= 0 {
		return 0
	}
	b := []byte(s)
	if len(b) > int(n)-1 {
		b = b[:int(n)-1]
	}
	dst := unsafe.Slice((*byte)(unsafe.Pointer(buf)), int(n))
	copy(dst, b)
	dst[len(b)] = 0
	return C.int(len(b))
}

//export sacoop_ts_start
func sacoop_ts_start(stateDir *C.char, hostname *C.char, logDir *C.char, authKey *C.char) (rc C.int) {
	defer guard("start", &rc)
	mu.Lock()
	defer mu.Unlock()
	if started {
		return 0
	}
	dir := cstr(stateDir)
	name := strings.ToLower(cstr(hostname))
	ld := cstr(logDir)
	if ld == "" {
		ld = dir
	}
	openLogs(ld)
	logf("Go %s %s/%s, Tailscale-библиотека загружена", runtime.Version(), runtime.GOOS, runtime.GOARCH)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		lastErr = "папка: " + err.Error()
		logf("папка %s: %v", dir, err)
		return -1
	}
	os.Setenv("TS_NO_LOGS_NO_SUPPORT", "true") // не отправлять журналы Tailscale в интернет
	// Android: у приложения нет /var/lib, $HOME и записываемого /data/local/tmp — logpolicy.LogsDir
	// паниковал «no safe place found to store log state» (тест игрока, 1.1.2). Даём свои папки.
	setDirEnv("TS_LOGS_DIR", filepath.Join(dir, "logs"), true)
	setDirEnv("TMPDIR", filepath.Join(dir, "tmp"), false)
	setDirEnv("XDG_CACHE_HOME", filepath.Join(dir, "cache"), false)
	setDirEnv("XDG_CONFIG_HOME", filepath.Join(dir, "config"), false)
	srv = &tsnet.Server{
		Dir:      dir,
		Hostname: name,
		Logf:     func(f string, a ...any) { logf(f, a...) },
		UserLogf: func(f string, a ...any) { logf("[user] "+f, a...) },
	}
	if k := strings.TrimSpace(cstr(authKey)); k != "" {
		srv.AuthKey = k // ключ в журнал не пишем
		useKey = true
		logf("вход по ключу из кода приглашения")
	}
	started = true
	stName = name
	logf("старт: имя %s, папка %s", name, dir)
	go func() {
		defer func() {
			if r := recover(); r != nil {
				logf("ПАНИКА в запуск tsnet: %v\n%s", r, debug.Stack())
				mu.Lock()
				failed, lastErr = true, fmt.Sprintf("сбой запуска: %v", r)
				mu.Unlock()
			}
		}()
		logf("tsnet: Start…")
		if err := srv.Start(); err != nil {
			mu.Lock()
			failed, lastErr = true, err.Error()
			mu.Unlock()
			logf("Start: %v", err)
			return
		}
		c, err := srv.LocalClient()
		if err != nil {
			mu.Lock()
			failed, lastErr = true, err.Error()
			mu.Unlock()
			logf("LocalClient: %v", err)
			return
		}
		mu.Lock()
		lc = c
		pk := pendKey
		pendKey = ""
		mu.Unlock()
		logf("tsnet: запущен, жду состояния")
		if pk != "" {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			st, err := c.Status(ctx)
			cancel()
			if err == nil && st.BackendState == "NeedsLogin" {
				go loginWithKey(c, pk)
			}
		}
		statusLoop()
	}()
	return 0
}

func statusLoop() {
	for {
		mu.Lock()
		c := lc
		mu.Unlock()
		if c == nil {
			return
		}
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		st, err := c.Status(ctx)
		cancel()
		if err != nil {
			logf("Status: %v", err)
			time.Sleep(time.Second)
			continue
		}
		var ip string
		for _, a := range st.TailscaleIPs {
			if a.Is4() {
				ip = a.String()
			}
		}
		var peers []peerInfo
		for _, p := range st.Peer {
			pi := peerInfo{host: strings.ToLower(p.HostName), online: p.Online}
			for _, a := range p.TailscaleIPs {
				if a.Is4() {
					pi.ip = a.String()
				}
			}
			if pi.ip != "" {
				peers = append(peers, pi)
			}
		}
		mu.Lock()
		stState, stURL, stIP, stPeers = st.BackendState, st.AuthURL, ip, peers
		le := ""
		for _, h := range st.Health {
			if i := strings.Index(h, "login error was: "); i >= 0 {
				le = h[i+len("login error was: "):]
			}
		}
		if st.BackendState == "Running" {
			le = ""
		}
		if le != loginErr {
			loginErr = le
			if le != "" {
				logf("ошибка входа: %s", le)
			}
		}
		ask := st.BackendState == "NeedsLogin" && st.AuthURL == "" && !useKey && time.Since(loginAsked) > 20*time.Second
		if ask {
			loginAsked = time.Now()
		}
		mu.Unlock()
		if ask {
			logf("нужен вход — прошу ссылку")
			ctx2, cancel2 := context.WithTimeout(context.Background(), 10*time.Second)
			if err := c.StartLoginInteractive(ctx2); err != nil {
				logf("StartLoginInteractive: %v", err)
			}
			cancel2()
		}
		time.Sleep(time.Second)
	}
}

// Статус одной строкой: состояние \t ссылка для входа \t свой адрес \t имя \t ошибка
// Состояния Tailscale: NoState, NeedsLogin, NeedsMachineAuth, Stopped, Starting, Running.
//
//export sacoop_ts_status
func sacoop_ts_status(buf *C.char, n C.int) (rc C.int) {
	defer guard("status", &rc)
	mu.Lock()
	defer mu.Unlock()
	state := stState
	if !started {
		state = "Off"
	} else if failed {
		state = "Error"
	} else if state == "" {
		state = "Starting"
	}
	e := lastErr
	if e == "" && state == "NeedsLogin" {
		e = loginErr
	}
	url := stURL
	if useKey {
		url = "" // по ключу браузер не открываем
	}
	return put(buf, n, strings.Join([]string{state, url, stIP, stName, e}, "\t"))
}

// Найти устройство по имени (sacoop-<ID>). 1 — в сети, 2 — есть, но не в сети, 0 — нет, -1 — сеть не готова.
//
//export sacoop_ts_find_peer
func sacoop_ts_find_peer(hostname *C.char, ipOut *C.char, n C.int) (rc C.int) {
	defer guard("find_peer", &rc)
	mu.Lock()
	defer mu.Unlock()
	if stState != "Running" {
		return -1
	}
	want := strings.ToLower(cstr(hostname))
	for _, p := range stPeers {
		if p.host == want || strings.HasPrefix(p.host, want+"-") {
			put(ipOut, n, p.ip)
			if p.online {
				return 1
			}
			return 2
		}
	}
	return 0
}

// Список других телефонов с модом (для подсказки в меню): «имя:адрес:1/0;…»
//
//export sacoop_ts_peers
func sacoop_ts_peers(buf *C.char, n C.int) (rc C.int) {
	defer guard("peers", &rc)
	mu.Lock()
	defer mu.Unlock()
	var parts []string
	for _, p := range stPeers {
		if strings.HasPrefix(p.host, "sacoop-") {
			on := "0"
			if p.online {
				on = "1"
			}
			parts = append(parts, p.host+":"+p.ip+":"+on)
		}
	}
	return put(buf, n, strings.Join(parts, ";"))
}

// Войти по ключу из кода приглашения, когда Tailscale уже запущен без ключа (ждёт входа).
//
//export sacoop_ts_use_key
func sacoop_ts_use_key(authKey *C.char) (rc C.int) {
	defer guard("use_key", &rc)
	k := strings.TrimSpace(cstr(authKey))
	if k == "" {
		return -1
	}
	mu.Lock()
	c := lc
	useKey = true
	if c == nil {
		pendKey = k // tsnet ещё стартует — применим, когда будет готов
	}
	mu.Unlock()
	if c != nil {
		go loginWithKey(c, k)
	}
	return 0
}

func loginWithKey(c *local.Client, k string) {
	defer guard("вход по ключу", nil)
	logf("вход по ключу из кода приглашения (Tailscale уже запущен)")
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	if err := c.Start(ctx, ipn.Options{AuthKey: k}); err != nil {
		logf("вход по ключу: %v", err)
	}
}

// Мост: игра шлёт на 127.0.0.1:<ответ>, мост — в сеть Tailscale и обратно.
// tsPort — порт «точки» в сети Tailscale; gamePort — порт сокета игры (хост; 0 — узнать по первому пакету).
// Возвращает порт моста на 127.0.0.1; 0 — сеть ещё не готова; -1 — ошибка.
//
//export sacoop_ts_bridge
func sacoop_ts_bridge(tsPort C.int, gamePort C.int) (rc C.int) {
	defer guard("bridge", &rc)
	mu.Lock()
	defer mu.Unlock()
	if srv == nil || stState != "Running" || stIP == "" {
		return 0
	}
	if gamePort > 0 {
		gameAddr = &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: int(gamePort)}
	} else {
		gameAddr = nil
	}
	if loop != nil && tsConn != nil && tsPortNow == int(tsPort) {
		return C.int(loop.LocalAddr().(*net.UDPAddr).Port)
	}
	closeBridgeLocked()
	pc, err := srv.ListenPacket("udp", fmt.Sprintf("%s:%d", stIP, int(tsPort)))
	if err != nil {
		lastErr = "точка: " + err.Error()
		logf("ListenPacket: %v", err)
		return -1
	}
	l, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 0})
	if err != nil {
		pc.Close()
		lastErr = "мост: " + err.Error()
		return -1
	}
	tsConn, loop, tsPortNow = pc, l, int(tsPort)
	bridgeGen++
	gen := bridgeGen
	logf("мост: точка %s:%d <-> 127.0.0.1:%d", stIP, int(tsPort), l.LocalAddr().(*net.UDPAddr).Port)
	go fromTailnet(pc, l, gen)
	go fromGame(pc, l, gen)
	return C.int(l.LocalAddr().(*net.UDPAddr).Port)
}

func fromTailnet(pc net.PacketConn, l *net.UDPConn, gen int) {
	defer guard("мост из сети", nil)
	buf := make([]byte, 2048)
	for {
		n, from, err := pc.ReadFrom(buf)
		if err != nil {
			logf("мост: чтение из сети: %v", err)
			return
		}
		mu.Lock()
		if gen != bridgeGen {
			mu.Unlock()
			return
		}
		if ua, ok := from.(*net.UDPAddr); ok {
			if ap := ua.AddrPort(); ap != peer {
				logf("мост: второй телефон %s", ap)
				peer = netip.AddrPortFrom(ap.Addr().Unmap(), ap.Port())
			}
		}
		ga := gameAddr
		mu.Unlock()
		if ga != nil {
			l.WriteToUDP(buf[:n], ga)
		}
	}
}

func fromGame(pc net.PacketConn, l *net.UDPConn, gen int) {
	defer guard("мост от игры", nil)
	buf := make([]byte, 2048)
	for {
		n, from, err := l.ReadFromUDP(buf)
		if err != nil {
			logf("мост: чтение от игры: %v", err)
			return
		}
		mu.Lock()
		if gen != bridgeGen {
			mu.Unlock()
			return
		}
		if gameAddr == nil || gameAddr.Port != from.Port {
			gameAddr = from
		}
		p := peer
		mu.Unlock()
		if p.IsValid() {
			pc.WriteTo(buf[:n], net.UDPAddrFromAddrPort(p))
		}
	}
}

func closeBridgeLocked() {
	if tsConn != nil {
		tsConn.Close()
		tsConn = nil
	}
	if loop != nil {
		loop.Close()
		loop = nil
	}
	tsPortNow = 0
	bridgeGen++
}

// Друг: куда слать (адрес хоста в сети Tailscale).
//
//export sacoop_ts_set_peer
func sacoop_ts_set_peer(ip *C.char, port C.int) (rc C.int) {
	defer guard("set_peer", &rc)
	a, err := netip.ParseAddr(cstr(ip))
	if err != nil {
		return -1
	}
	mu.Lock()
	peer = netip.AddrPortFrom(a, uint16(port))
	mu.Unlock()
	logf("мост: шлём на %s", peer)
	return 0
}

//export sacoop_ts_close_bridge
func sacoop_ts_close_bridge() {
	defer guard("close_bridge", nil)
	mu.Lock()
	defer mu.Unlock()
	closeBridgeLocked()
	peer = netip.AddrPort{}
}

// Выйти из аккаунта (сменить аккаунт)
//
//export sacoop_ts_logout
func sacoop_ts_logout() (rc C.int) {
	defer guard("logout", &rc)
	mu.Lock()
	c := lc
	mu.Unlock()
	if c == nil {
		return -1
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := c.Logout(ctx); err != nil {
		logf("Logout: %v", err)
		return -1
	}
	mu.Lock()
	loginAsked = time.Time{}
	mu.Unlock()
	return 0
}

func main() {}
