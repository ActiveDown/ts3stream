package main

import (
	"crypto/subtle"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/livekit/protocol/auth"
	"github.com/livekit/protocol/livekit"
)

type tokenResponse struct {
	Token string `json:"token"`
	URL   string `json:"url,omitempty"`
	Room  string `json:"room"`
}

type streamsResponse struct {
	Streams []streamEntry `json:"streams"`
}

type config struct {
	apiKey     string
	apiSecret  string
	livekitURL string
	tokenTTL   time.Duration
	publicURL  string
	clientKey  string
	query      ts3QueryConfig
}

type controlServer struct {
	apiKey     string
	apiSecret  string
	livekitURL string
	tokenTTL   time.Duration
	publicURL  string
	clientKey  string
	query      ts3QueryConfig
	registry   *streamRegistry
}

type registerRequest struct {
	UID       string `json:"uid"`
	Nickname  string `json:"nickname"`
	ServerUID string `json:"serverUid"`
	ChannelID uint64 `json:"channelId"`
}

type registerResponse struct {
	ControlURL string `json:"controlUrl"`
	ClientKey  string `json:"clientKey"`
	LivekitURL string `json:"livekitUrl"`
}

type streamStartRequest struct {
	Room      string `json:"room"`
	Identity  string `json:"identity"`
	Nickname  string `json:"nickname"`
	ChannelID uint64 `json:"channelId"`
	ServerUID string `json:"serverUid"`
}

type streamHeartbeatRequest struct {
	Room     string `json:"room"`
	Identity string `json:"identity"`
	Nickname string `json:"nickname"`
}

type streamStopRequest struct {
	Room     string `json:"room"`
	Identity string `json:"identity"`
}

func envOr(key, fallback string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return fallback
}

func envBool(key string, fallback bool) bool {
	value := strings.TrimSpace(os.Getenv(key))
	if value == "" {
		return fallback
	}
	parsed, err := strconv.ParseBool(value)
	if err != nil {
		log.Printf("ignoring invalid %s value %q", key, value)
		return fallback
	}
	return parsed
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}

func newControlServer(cfg config) *controlServer {
	return &controlServer{
		apiKey:     cfg.apiKey,
		apiSecret:  cfg.apiSecret,
		livekitURL: cfg.livekitURL,
		tokenTTL:   cfg.tokenTTL,
		publicURL:  cfg.publicURL,
		clientKey:  cfg.clientKey,
		query:      cfg.query,
		registry:   newStreamRegistry(streamTTL),
	}
}

func (s *controlServer) handler(clientKey string) http.Handler {
	mux := http.NewServeMux()

	mux.HandleFunc("/api/health", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
	})

	mux.HandleFunc("/api/token", s.handleToken)
	mux.HandleFunc("/api/register", s.handleRegister)
	mux.HandleFunc("/api/streams/start", s.handleStreamStart)
	mux.HandleFunc("/api/streams/heartbeat", s.handleStreamHeartbeat)
	mux.HandleFunc("/api/streams/stop", s.handleStreamStop)
	mux.HandleFunc("/api/streams", s.handleStreams)
	mux.HandleFunc("/api/live", s.handleLive)

	return requireClientKey(clientKey, mux)
}

func requireClientKey(key string, next http.Handler) http.Handler {
	if key == "" {
		return next
	}

	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasPrefix(r.URL.Path, "/api/") && r.URL.Path != "/api/health" && r.URL.Path != "/api/register" {
			supplied := r.Header.Get("X-TS3Stream-Key")
			if subtle.ConstantTimeCompare([]byte(supplied), []byte(key)) != 1 {
				writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "unauthorized"})
				return
			}
		}
		next.ServeHTTP(w, r)
	})
}

func requireMethod(w http.ResponseWriter, r *http.Request, method string) bool {
	if r.Method == method {
		return true
	}
	w.Header().Set("Allow", method)
	writeJSON(w, http.StatusMethodNotAllowed, map[string]string{"error": "method not allowed"})
	return false
}

func decodeJSONBody(w http.ResponseWriter, r *http.Request, dst any) bool {
	r.Body = http.MaxBytesReader(w, r.Body, 64<<10)

	decoder := json.NewDecoder(r.Body)
	if err := decoder.Decode(dst); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid JSON body"})
		return false
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid JSON body"})
		return false
	}
	return true
}

func (s *controlServer) handleToken(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost && r.Method != http.MethodGet {
		writeJSON(w, http.StatusMethodNotAllowed, map[string]string{"error": "method not allowed"})
		return
	}

	query := r.URL.Query()
	room := strings.TrimSpace(query.Get("room"))
	// QUrlQuery sends literal '+' in TS3's base64 UID. Go's form-style
	// query decoder treats it as a space, breaking the viewer's exact UID
	// filter. Preserve raw '+' for identity while still decoding %2B once.
	// Keep standard form decoding for other fields such as display names.
	identityQuery, _ := url.ParseQuery(strings.ReplaceAll(r.URL.RawQuery, "+", "%2B"))
	identity := strings.TrimSpace(identityQuery.Get("identity"))
	name := strings.TrimSpace(query.Get("name"))
	publish := query.Get("publish") == "1" || strings.EqualFold(query.Get("publish"), "true")

	if room == "" || identity == "" {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "room and identity are required"})
		return
	}

	grant := &auth.VideoGrant{
		RoomJoin: true,
		Room:     room,
	}

	if publish {
		grant.SetCanPublish(true)
		grant.SetCanSubscribe(false)
		grant.SetCanPublishSources([]livekit.TrackSource{
			livekit.TrackSource_SCREEN_SHARE,
			livekit.TrackSource_SCREEN_SHARE_AUDIO,
		})
	} else {
		grant.SetCanPublish(false)
		grant.SetCanSubscribe(true)
	}

	accessToken := auth.NewAccessToken(s.apiKey, s.apiSecret).
		AddGrant(grant).
		SetIdentity(identity).
		SetName(name).
		SetValidFor(s.tokenTTL)

	token, err := accessToken.ToJWT()
	if err != nil {
		log.Printf("token error: %v", err)
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "token generation failed"})
		return
	}

	log.Printf("token issued: room=%q identity=%q publish=%v", room, identity, publish)
	writeJSON(w, http.StatusOK, tokenResponse{
		Token: token,
		URL:   s.livekitURL,
		Room:  room,
	})
}

func (s *controlServer) handleRegister(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodPost) {
		return
	}

	var req registerRequest
	if !decodeJSONBody(w, r, &req) {
		return
	}

	uid := strings.TrimSpace(req.UID)
	serverUID := strings.TrimSpace(req.ServerUID)
	if uid == "" || serverUID == "" {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "uid and serverUid are required"})
		return
	}

	if !s.query.configured() {
		writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "TS3 query is not configured"})
		return
	}

	client, err := newTS3QueryClient(s.query, defaultTS3QueryTimeout)
	if err != nil {
		log.Printf("register: TS3 query connect failed: %v", err)
		writeJSON(w, http.StatusBadGateway, map[string]string{"error": "TS3 query is unavailable"})
		return
	}
	defer client.Close()

	online, err := client.ClientOnline(uid)
	if err != nil {
		log.Printf("register: TS3 client lookup failed: %v", err)
		writeJSON(w, http.StatusBadGateway, map[string]string{"error": "TS3 query failed"})
		return
	}
	if !online {
		writeJSON(w, http.StatusForbidden, map[string]string{"error": "client is not online on the TS3 server"})
		return
	}

	// The server UID check is best-effort: older or restricted query logins
	// may not be able to read serverinfo. The client UID check above is the
	// authoritative registration check.
	if actualUID, err := client.ServerUniqueID(); err != nil {
		log.Printf("register: TS3 server UID lookup failed: %v", err)
	} else if actualUID != "" && actualUID != serverUID {
		writeJSON(w, http.StatusForbidden, map[string]string{"error": "server UID mismatch"})
		return
	}

	log.Printf("register: verified client on TS3 server")
	writeJSON(w, http.StatusOK, registerResponse{
		ControlURL: s.publicURL,
		ClientKey:  s.clientKey,
		LivekitURL: s.livekitURL,
	})
}

var welcomeMarkerPattern = regexp.MustCompile(`(?s)\[ts3stream\].*?\[/ts3stream\]`)

// welcomeMessageWithMarker replaces any existing ts3stream marker and appends
// the current public URL on its own line.
func welcomeMessageWithMarker(current, publicURL string) string {
	cleaned := welcomeMarkerPattern.ReplaceAllString(current, "")
	cleaned = strings.TrimRight(cleaned, " \t\r\n")
	marker := "[ts3stream]" + publicURL + "[/ts3stream]"
	if cleaned == "" {
		return marker
	}
	return cleaned + "\n" + marker
}

func configureWelcome(cfg ts3QueryConfig, publicURL string) error {
	client, err := newTS3QueryClient(cfg, defaultTS3QueryTimeout)
	if err != nil {
		return err
	}
	defer client.Close()

	current, err := client.WelcomeMessage()
	if err != nil {
		return fmt.Errorf("read TS3 welcome message: %w", err)
	}
	updated := welcomeMessageWithMarker(current, publicURL)
	if updated == current {
		return nil
	}
	if err := client.SetWelcomeMessage(updated); err != nil {
		return fmt.Errorf("set TS3 welcome message: %w", err)
	}
	return nil
}

func configureWelcomeWithRetry(cfg ts3QueryConfig, publicURL string, attempts int, delay time.Duration) error {
	var err error
	for attempt := 1; attempt <= attempts; attempt++ {
		if err = configureWelcome(cfg, publicURL); err == nil {
			return nil
		}
		if attempt < attempts {
			time.Sleep(delay)
		}
	}
	return err
}

func (s *controlServer) handleStreamStart(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodPost) {
		return
	}

	var req streamStartRequest
	if !decodeJSONBody(w, r, &req) {
		return
	}

	room := strings.TrimSpace(req.Room)
	identity := strings.TrimSpace(req.Identity)
	if room == "" || identity == "" {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "room and identity are required"})
		return
	}

	channelID := req.ChannelID
	serverUID := strings.TrimSpace(req.ServerUID)
	s.registry.start(streamEntry{
		Room:      room,
		Identity:  identity,
		Nickname:  strings.TrimSpace(req.Nickname),
		ChannelID: channelID,
		ServerUID: serverUID,
	})

	log.Printf("stream started: room=%q identity=%q channel=%d", room, identity, channelID)
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

func (s *controlServer) handleStreamHeartbeat(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodPost) {
		return
	}

	var req streamHeartbeatRequest
	if !decodeJSONBody(w, r, &req) {
		return
	}

	room := strings.TrimSpace(req.Room)
	identity := strings.TrimSpace(req.Identity)
	if room == "" || identity == "" {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "room and identity are required"})
		return
	}

	if !s.registry.heartbeat(room, identity, strings.TrimSpace(req.Nickname)) {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "unknown stream"})
		return
	}

	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

func (s *controlServer) handleStreamStop(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodPost) {
		return
	}

	var req streamStopRequest
	if !decodeJSONBody(w, r, &req) {
		return
	}

	room := strings.TrimSpace(req.Room)
	identity := strings.TrimSpace(req.Identity)
	if room == "" || identity == "" {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "room and identity are required"})
		return
	}

	if s.registry.stop(room, identity) {
		log.Printf("stream stopped: room=%q identity=%q", room, identity)
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

func (s *controlServer) handleStreams(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodGet) {
		return
	}

	room := strings.TrimSpace(r.URL.Query().Get("room"))
	writeJSON(w, http.StatusOK, streamsResponse{Streams: s.registry.list(room)})
}

func (s *controlServer) handleLive(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodGet) {
		return
	}

	writeJSON(w, http.StatusOK, streamsResponse{Streams: s.registry.list("")})
}

func main() {
	addr := flag.String("addr", envOr("TS3STREAM_CONTROL_ADDR", "127.0.0.1:8081"), "HTTP listen address")
	apiKey := flag.String("api-key", envOr("LIVEKIT_API_KEY", "devkey"), "LiveKit API key")
	apiSecret := flag.String("api-secret", envOr("LIVEKIT_API_SECRET", "secret"), "LiveKit API secret")
	livekitURL := flag.String("livekit-url", envOr("LIVEKIT_URL", "ws://127.0.0.1:7880"), "LiveKit URL returned to clients")
	tokenTTL := flag.Duration("token-ttl", time.Hour, "token lifetime")
	clientKey := flag.String("client-key", envOr("TS3STREAM_CLIENT_KEY", ""), "shared key required in X-TS3Stream-Key for /api endpoints (empty disables auth)")
	controlPublicURL := flag.String("public-url", envOr("TS3STREAM_PUBLIC_URL", ""), "public base URL of the control API")
	ts3QueryAddr := flag.String("ts3-query-addr", envOr("TS3_QUERY_ADDR", ""), "TeamSpeak 3 ServerQuery address, e.g. 127.0.0.1:10011 (empty disables query features)")
	ts3QueryUser := flag.String("ts3-query-user", envOr("TS3_QUERY_USER", defaultTS3QueryUser), "TeamSpeak 3 ServerQuery login name")
	ts3QueryPassword := flag.String("ts3-query-password", envOr("TS3_QUERY_PASSWORD", ""), "TeamSpeak 3 ServerQuery password")
	ts3QuerySID := flag.String("ts3-query-sid", envOr("TS3_QUERY_SID", defaultTS3QuerySID), "TeamSpeak 3 virtual server ID for ServerQuery")
	welcomeAuto := flag.Bool("welcome-auto", envBool("TS3STREAM_WELCOME_AUTO", false), "update the TS3 server welcome message with the public control URL on startup")
	flag.Parse()

	query := ts3QueryConfig{
		addr:     strings.TrimSpace(*ts3QueryAddr),
		user:     strings.TrimSpace(*ts3QueryUser),
		password: *ts3QueryPassword,
		sid:      strings.TrimSpace(*ts3QuerySID),
	}

	control := newControlServer(config{
		apiKey:     *apiKey,
		apiSecret:  *apiSecret,
		livekitURL: *livekitURL,
		tokenTTL:   *tokenTTL,
		publicURL:  strings.TrimRight(strings.TrimSpace(*controlPublicURL), "/"),
		clientKey:  *clientKey,
		query:      query,
	})

	server := &http.Server{
		Addr:              *addr,
		Handler:           control.handler(*clientKey),
		ReadHeaderTimeout: 5 * time.Second,
	}

	listener, err := net.Listen("tcp", *addr)
	if err != nil {
		log.Fatal(err)
	}

	if *welcomeAuto {
		switch {
		case !query.configured():
			log.Printf("welcome auto-configuration skipped: TS3 query address/password are not configured")
		case control.publicURL == "":
			log.Printf("welcome auto-configuration skipped: --public-url/TS3STREAM_PUBLIC_URL is not configured")
		default:
			go func() {
				if err := configureWelcomeWithRetry(query, control.publicURL, 3, 2*time.Second); err != nil {
					log.Printf("welcome auto-configuration failed: %v", err)
					return
				}
				log.Printf("welcome auto-configuration updated the TS3 server welcome message")
			}()
		}
	}

	if *clientKey != "" {
		log.Printf("client key authentication enabled")
	} else {
		log.Printf("client key authentication disabled")
	}
	log.Printf("ts3stream control listening on %s, LiveKit URL %s", *addr, *livekitURL)
	if err := server.Serve(listener); err != nil && err != http.ErrServerClosed {
		log.Fatal(err)
	}
}
