package main

import (
	"crypto/subtle"
	"encoding/json"
	"flag"
	"io"
	"log"
	"net/http"
	"os"
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
}

type controlServer struct {
	apiKey     string
	apiSecret  string
	livekitURL string
	tokenTTL   time.Duration
	registry   *streamRegistry
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
		registry:   newStreamRegistry(streamTTL),
	}
}

func (s *controlServer) handler(clientKey string) http.Handler {
	mux := http.NewServeMux()

	mux.HandleFunc("/api/health", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
	})

	mux.HandleFunc("/api/token", s.handleToken)
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
		if strings.HasPrefix(r.URL.Path, "/api/") && r.URL.Path != "/api/health" {
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
	identity := strings.TrimSpace(query.Get("identity"))
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
	publicURL := flag.String("livekit-url", envOr("LIVEKIT_URL", "ws://127.0.0.1:7880"), "LiveKit URL returned to clients")
	tokenTTL := flag.Duration("token-ttl", time.Hour, "token lifetime")
	clientKey := flag.String("client-key", envOr("TS3STREAM_CLIENT_KEY", ""), "shared key required in X-TS3Stream-Key for /api endpoints (empty disables auth)")
	flag.Parse()

	control := newControlServer(config{
		apiKey:     *apiKey,
		apiSecret:  *apiSecret,
		livekitURL: *publicURL,
		tokenTTL:   *tokenTTL,
	})

	server := &http.Server{
		Addr:              *addr,
		Handler:           control.handler(*clientKey),
		ReadHeaderTimeout: 5 * time.Second,
	}

	if *clientKey != "" {
		log.Printf("client key authentication enabled")
	} else {
		log.Printf("client key authentication disabled")
	}
	log.Printf("ts3stream control listening on %s, LiveKit URL %s", *addr, *publicURL)
	if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatal(err)
	}
}
