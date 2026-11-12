package main

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

const testClientKey = "test-client-key"

type testAPI struct {
	server *controlServer
	http   *httptest.Server
}

func newTestAPI(t *testing.T) *testAPI {
	t.Helper()

	return newTestAPIWithConfig(t, config{
		apiKey:     "test-api-key",
		apiSecret:  "test-api-secret",
		livekitURL: "ws://livekit.test",
		tokenTTL:   time.Hour,
		publicURL:  "https://stream.test",
		clientKey:  testClientKey,
	})
}

func newTestAPIWithConfig(t *testing.T, cfg config) *testAPI {
	t.Helper()

	server := newControlServer(cfg)
	httpServer := httptest.NewServer(server.handler(cfg.clientKey))
	t.Cleanup(httpServer.Close)

	return &testAPI{server: server, http: httpServer}
}

func (a *testAPI) do(t *testing.T, method, path, body, clientKey string) (int, []byte) {
	t.Helper()

	var reader io.Reader
	if body != "" {
		reader = strings.NewReader(body)
	}

	req, err := http.NewRequest(method, a.http.URL+path, reader)
	if err != nil {
		t.Fatalf("new request: %v", err)
	}
	if body != "" {
		req.Header.Set("Content-Type", "application/json")
	}
	if clientKey != "" {
		req.Header.Set("X-TS3Stream-Key", clientKey)
	}

	resp, err := a.http.Client().Do(req)
	if err != nil {
		t.Fatalf("do request: %v", err)
	}
	defer resp.Body.Close()

	data, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("read response: %v", err)
	}
	return resp.StatusCode, data
}

func decodeStatus(t *testing.T, body []byte) string {
	t.Helper()

	var response map[string]string
	if err := json.Unmarshal(body, &response); err != nil {
		t.Fatalf("decode response %q: %v", body, err)
	}
	return response["status"]
}

func TestClientKeyAuth(t *testing.T) {
	api := newTestAPI(t)

	if status, _ := api.do(t, http.MethodGet, "/api/health", "", ""); status != http.StatusOK {
		t.Fatalf("health without key = %d, want %d", status, http.StatusOK)
	}

	if status, _ := api.do(t, http.MethodGet, "/api/live", "", ""); status != http.StatusUnauthorized {
		t.Fatalf("live without key = %d, want %d", status, http.StatusUnauthorized)
	}
	if status, _ := api.do(t, http.MethodGet, "/api/live", "", "wrong-key"); status != http.StatusUnauthorized {
		t.Fatalf("live with wrong key = %d, want %d", status, http.StatusUnauthorized)
	}
	if status, _ := api.do(t, http.MethodGet, "/api/live", "", testClientKey); status != http.StatusOK {
		t.Fatalf("live with key = %d, want %d", status, http.StatusOK)
	}

	if status, _ := api.do(t, http.MethodGet, "/api/token?room=room-a&identity=user-1", "", ""); status != http.StatusUnauthorized {
		t.Fatalf("token without key = %d, want %d", status, http.StatusUnauthorized)
	}
}

func TestStreamEndpoints(t *testing.T) {
	api := newTestAPI(t)

	// Malformed and invalid bodies.
	if status, _ := api.do(t, http.MethodPost, "/api/streams/start", `{"room":`, testClientKey); status != http.StatusBadRequest {
		t.Fatalf("malformed start body = %d, want %d", status, http.StatusBadRequest)
	}
	if status, _ := api.do(t, http.MethodPost, "/api/streams/start", `{"identity":"user-1"}`, testClientKey); status != http.StatusBadRequest {
		t.Fatalf("start without room = %d, want %d", status, http.StatusBadRequest)
	}
	if status, _ := api.do(t, http.MethodGet, "/api/streams/start", "", testClientKey); status != http.StatusMethodNotAllowed {
		t.Fatalf("GET start = %d, want %d", status, http.StatusMethodNotAllowed)
	}

	startBody := `{"room":"room-a","identity":"user-1","nickname":"Alice","channelId":42,"serverUid":"server-1"}`
	status, body := api.do(t, http.MethodPost, "/api/streams/start", startBody, testClientKey)
	if status != http.StatusOK {
		t.Fatalf("start = %d (%s), want %d", status, strings.TrimSpace(string(body)), http.StatusOK)
	}
	if got := decodeStatus(t, body); got != "ok" {
		t.Fatalf("start status = %q, want %q", got, "ok")
	}

	if status, _ := api.do(t, http.MethodPost, "/api/streams/start", `{"room":"room-b","identity":"user-2"}`, testClientKey); status != http.StatusOK {
		t.Fatalf("second start = %d, want %d", status, http.StatusOK)
	}

	// Unknown heartbeat must be detectable.
	status, body = api.do(t, http.MethodPost, "/api/streams/heartbeat", `{"room":"room-a","identity":"ghost"}`, testClientKey)
	if status != http.StatusNotFound {
		t.Fatalf("unknown heartbeat = %d, want %d", status, http.StatusNotFound)
	}
	var apiError map[string]string
	if err := json.Unmarshal(body, &apiError); err != nil {
		t.Fatalf("decode heartbeat error: %v", err)
	}
	if apiError["error"] != "unknown stream" {
		t.Fatalf("unknown heartbeat error = %q, want %q", apiError["error"], "unknown stream")
	}

	// Heartbeat refreshes an existing stream and can update the nickname.
	status, body = api.do(t, http.MethodPost, "/api/streams/heartbeat", `{"room":"room-a","identity":"user-1","nickname":"Alice B"}`, testClientKey)
	if status != http.StatusOK {
		t.Fatalf("heartbeat = %d (%s), want %d", status, strings.TrimSpace(string(body)), http.StatusOK)
	}

	status, body = api.do(t, http.MethodGet, "/api/streams?room=room-a", "", testClientKey)
	if status != http.StatusOK {
		t.Fatalf("streams by room = %d, want %d", status, http.StatusOK)
	}
	var streams streamsResponse
	if err := json.Unmarshal(body, &streams); err != nil {
		t.Fatalf("decode streams: %v", err)
	}
	if len(streams.Streams) != 1 {
		t.Fatalf("streams by room returned %d entries, want 1", len(streams.Streams))
	}
	entry := streams.Streams[0]
	if entry.Room != "room-a" || entry.Identity != "user-1" || entry.Nickname != "Alice B" {
		t.Fatalf("unexpected stream entry: %+v", entry)
	}
	if entry.ChannelID != 42 || entry.ServerUID != "server-1" {
		t.Fatalf("unexpected TS3 metadata: %+v", entry)
	}
	if entry.LastSeen <= 0 {
		t.Fatalf("lastSeen = %d, want a positive Unix millisecond timestamp", entry.LastSeen)
	}

	status, body = api.do(t, http.MethodGet, "/api/streams?room=room-c", "", testClientKey)
	if status != http.StatusOK {
		t.Fatalf("streams for missing room = %d, want %d", status, http.StatusOK)
	}
	if err := json.Unmarshal(body, &streams); err != nil {
		t.Fatalf("decode streams: %v", err)
	}
	if len(streams.Streams) != 0 {
		t.Fatalf("streams for missing room returned %d entries, want 0", len(streams.Streams))
	}

	// /api/live exposes all rooms.
	status, body = api.do(t, http.MethodGet, "/api/live", "", testClientKey)
	if status != http.StatusOK {
		t.Fatalf("live = %d, want %d", status, http.StatusOK)
	}
	if err := json.Unmarshal(body, &streams); err != nil {
		t.Fatalf("decode live: %v", err)
	}
	if len(streams.Streams) != 2 {
		t.Fatalf("live returned %d entries, want 2", len(streams.Streams))
	}

	if status, _ := api.do(t, http.MethodPost, "/api/streams/stop", `{"room":"room-a","identity":"user-1"}`, testClientKey); status != http.StatusOK {
		t.Fatalf("stop = %d, want %d", status, http.StatusOK)
	}
	if status, _ := api.do(t, http.MethodPost, "/api/streams/stop", `{"room":"room-a","identity":"user-1"}`, testClientKey); status != http.StatusOK {
		t.Fatalf("repeated stop = %d, want %d", status, http.StatusOK)
	}

	status, body = api.do(t, http.MethodGet, "/api/streams", "", testClientKey)
	if status != http.StatusOK {
		t.Fatalf("streams = %d, want %d", status, http.StatusOK)
	}
	if err := json.Unmarshal(body, &streams); err != nil {
		t.Fatalf("decode streams: %v", err)
	}
	if len(streams.Streams) != 1 || streams.Streams[0].Identity != "user-2" {
		t.Fatalf("streams after stop = %+v, want only user-2", streams.Streams)
	}

	if status, _ := api.do(t, http.MethodGet, "/api/streams", "", testClientKey); status != http.StatusOK {
		t.Fatalf("GET streams = %d, want %d", status, http.StatusOK)
	}
	if status, _ := api.do(t, http.MethodPost, "/api/streams", "", testClientKey); status != http.StatusMethodNotAllowed {
		t.Fatalf("POST streams = %d, want %d", status, http.StatusMethodNotAllowed)
	}
}

func newRegisterTestAPI(t *testing.T, query ts3QueryConfig) *testAPI {
	t.Helper()

	return newTestAPIWithConfig(t, config{
		apiKey:     "test-api-key",
		apiSecret:  "test-api-secret",
		livekitURL: "ws://livekit.test",
		tokenTTL:   time.Hour,
		publicURL:  "https://stream.test",
		clientKey:  testClientKey,
		query:      query,
	})
}

func decodeAPIError(t *testing.T, body []byte) string {
	t.Helper()

	var response map[string]string
	if err := json.Unmarshal(body, &response); err != nil {
		t.Fatalf("decode error response %q: %v", body, err)
	}
	return response["error"]
}

func TestRegisterEndpointQueryDisabled(t *testing.T) {
	api := newTestAPI(t)

	status, body := api.do(t, http.MethodPost, "/api/register", `{"uid":"client-uid","serverUid":"server-uid","channelId":1}`, "")
	if status != http.StatusServiceUnavailable {
		t.Fatalf("register with query disabled = %d (%s), want %d", status, strings.TrimSpace(string(body)), http.StatusServiceUnavailable)
	}
	if got := decodeAPIError(t, body); got != "TS3 query is not configured" {
		t.Fatalf("register error = %q, want %q", got, "TS3 query is not configured")
	}
}

func TestRegisterEndpointValidation(t *testing.T) {
	api := newTestAPI(t)

	bodies := []string{
		`{"serverUid":"server-uid"}`,
		`{"uid":"client-uid"}`,
		`{"uid":"  ","serverUid":"server-uid"}`,
		`{"uid":"client-uid","serverUid":" "}`,
	}
	for _, body := range bodies {
		status, response := api.do(t, http.MethodPost, "/api/register", body, "")
		if status != http.StatusBadRequest {
			t.Fatalf("register %s = %d (%s), want %d", body, status, strings.TrimSpace(string(response)), http.StatusBadRequest)
		}
		if got := decodeAPIError(t, response); got != "uid and serverUid are required" {
			t.Fatalf("register %s error = %q", body, got)
		}
	}

	if status, _ := api.do(t, http.MethodGet, "/api/register", "", ""); status != http.StatusMethodNotAllowed {
		t.Fatalf("GET register = %d, want %d", status, http.StatusMethodNotAllowed)
	}
}

func TestRegisterEndpointVerifiesOnlineClient(t *testing.T) {
	clientUID := "clientUID+/="
	serverUID := "serverUID+/="
	fake := newFakeTS3Server(t, clientUID, serverUID, "")
	api := newRegisterTestAPI(t, ts3QueryConfig{
		addr:     fake.addr(),
		user:     "serveradmin",
		password: "secret",
		sid:      "1",
	})

	payload, err := json.Marshal(registerRequest{
		UID:       clientUID,
		Nickname:  "Alice",
		ServerUID: serverUID,
		ChannelID: 42,
	})
	if err != nil {
		t.Fatalf("marshal register payload: %v", err)
	}

	// /api/register must not require X-TS3Stream-Key: UID presence is the check.
	status, body := api.do(t, http.MethodPost, "/api/register", string(payload), "")
	if status != http.StatusOK {
		t.Fatalf("register = %d (%s), want %d", status, strings.TrimSpace(string(body)), http.StatusOK)
	}

	var response registerResponse
	if err := json.Unmarshal(body, &response); err != nil {
		t.Fatalf("decode register response: %v", err)
	}
	if response.ControlURL != "https://stream.test" {
		t.Fatalf("controlUrl = %q, want %q", response.ControlURL, "https://stream.test")
	}
	if response.ClientKey != testClientKey {
		t.Fatalf("clientKey = %q, want %q", response.ClientKey, testClientKey)
	}
	if response.LivekitURL != "ws://livekit.test" {
		t.Fatalf("livekitUrl = %q, want %q", response.LivekitURL, "ws://livekit.test")
	}
}

func TestRegisterEndpointRejectsOfflineClient(t *testing.T) {
	fake := newFakeTS3Server(t, "somebody-else", "server-uid", "")
	api := newRegisterTestAPI(t, ts3QueryConfig{
		addr:     fake.addr(),
		password: "secret",
	})

	status, body := api.do(t, http.MethodPost, "/api/register", `{"uid":"client-uid","serverUid":"server-uid"}`, "")
	if status != http.StatusForbidden {
		t.Fatalf("register offline client = %d (%s), want %d", status, strings.TrimSpace(string(body)), http.StatusForbidden)
	}
	if got := decodeAPIError(t, body); got != "client is not online on the TS3 server" {
		t.Fatalf("register error = %q, want %q", got, "client is not online on the TS3 server")
	}
}

func TestRegisterEndpointRejectsServerMismatch(t *testing.T) {
	fake := newFakeTS3Server(t, "client-uid", "other-server-uid", "")
	api := newRegisterTestAPI(t, ts3QueryConfig{
		addr:     fake.addr(),
		password: "secret",
	})

	status, body := api.do(t, http.MethodPost, "/api/register", `{"uid":"client-uid","serverUid":"expected-server-uid"}`, "")
	if status != http.StatusForbidden {
		t.Fatalf("register with server mismatch = %d (%s), want %d", status, strings.TrimSpace(string(body)), http.StatusForbidden)
	}
	if got := decodeAPIError(t, body); got != "server UID mismatch" {
		t.Fatalf("register error = %q, want %q", got, "server UID mismatch")
	}
}

func TestWelcomeMessageWithMarker(t *testing.T) {
	const publicURL = "https://stream.test"
	marker := "[ts3stream]" + publicURL + "[/ts3stream]"

	tests := []struct {
		name    string
		current string
		want    string
	}{
		{name: "empty message", current: "", want: marker},
		{name: "append marker", current: "Welcome!", want: "Welcome!\n" + marker},
		{name: "replace old marker", current: "Welcome!\n[ts3stream]https://old.test[/ts3stream]", want: "Welcome!\n" + marker},
		{name: "idempotent", current: "Welcome!\n" + marker, want: "Welcome!\n" + marker},
		{name: "normalize trailing whitespace", current: "Welcome!\n\n", want: "Welcome!\n" + marker},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := welcomeMessageWithMarker(tt.current, publicURL); got != tt.want {
				t.Fatalf("welcomeMessageWithMarker(%q) = %q, want %q", tt.current, got, tt.want)
			}
			if got := welcomeMessageWithMarker(tt.want, publicURL); got != tt.want {
				t.Fatalf("welcomeMessageWithMarker is not idempotent: got %q, want %q", got, tt.want)
			}
		})
	}
}

func TestConfigureWelcomeWithFakeServer(t *testing.T) {
	fake := newFakeTS3Server(t, "", "server-uid", "Welcome!")
	query := ts3QueryConfig{
		addr:     fake.addr(),
		user:     "serveradmin",
		password: "secret",
		sid:      "1",
	}

	if err := configureWelcome(query, "https://stream.test"); err != nil {
		t.Fatalf("configureWelcome: %v", err)
	}
	want := "Welcome!\n[ts3stream]https://stream.test[/ts3stream]"
	if got := fake.welcomeMessage(); got != want {
		t.Fatalf("welcome message = %q, want %q", got, want)
	}

	if err := configureWelcome(query, "https://stream.test"); err != nil {
		t.Fatalf("second configureWelcome: %v", err)
	}
	if got := fake.welcomeMessage(); got != want {
		t.Fatalf("welcome message changed on second run: %q, want %q", got, want)
	}
}

func TestTokenEndpointUnchanged(t *testing.T) {
	api := newTestAPI(t)

	status, body := api.do(t, http.MethodGet, "/api/token?room=room-a&identity=user-1&name=Alice&publish=1", "", testClientKey)
	if status != http.StatusOK {
		t.Fatalf("token = %d (%s), want %d", status, strings.TrimSpace(string(body)), http.StatusOK)
	}

	var response tokenResponse
	if err := json.Unmarshal(body, &response); err != nil {
		t.Fatalf("decode token response: %v", err)
	}
	if response.Token == "" || response.URL != "ws://livekit.test" || response.Room != "room-a" {
		t.Fatalf("unexpected token response: %+v", response)
	}

	if status, _ := api.do(t, http.MethodGet, "/api/token?room=room-a", "", testClientKey); status != http.StatusBadRequest {
		t.Fatalf("token without identity = %d, want %d", status, http.StatusBadRequest)
	}
}
