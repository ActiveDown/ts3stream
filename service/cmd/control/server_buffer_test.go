package main

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func TestServerBufferTokenAndCapability(t *testing.T) {
	for _, valid := range []bool{false, true} {
		backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if r.URL.Path != "/ts3stream/buffering" {
				t.Errorf("unexpected probe: %s", r.URL.Path)
			}
			if valid {
				_, _ = w.Write([]byte(`{"protocol":"ts3stream-server-av-v1","maxDelayMs":5000}`))
			} else {
				_, _ = w.Write([]byte("OK"))
			}
		}))
		api := newTestAPIWithConfig(t, config{apiKey: "key", apiSecret: "secret", tokenTTL: time.Minute,
			playoutDelayMs: 2000, bufferMode: "server", bufferProbeURL: backend.URL})
		for _, publish := range []string{"0", "1"} {
			status, body := api.do(t, http.MethodGet, "/api/token?room=room&identity=viewer&publish="+publish, "", "")
			if !valid {
				if status != http.StatusServiceUnavailable {
					t.Fatalf("stock backend got status %d", status)
				}
				continue
			}
			if status != http.StatusOK {
				t.Fatalf("status=%d body=%s", status, body)
			}
			var response tokenResponse
			if err := json.Unmarshal(body, &response); err != nil {
				t.Fatal(err)
			}
			payload, err := base64.RawURLEncoding.DecodeString(strings.Split(response.Token, ".")[1])
			if err != nil {
				t.Fatal(err)
			}
			var claims struct {
				RoomConfig struct {
					Min      int    `json:"minPlayoutDelay"`
					Max      int    `json:"maxPlayoutDelay"`
					Sync     bool   `json:"syncStreams"`
					Metadata string `json:"metadata"`
				} `json:"roomConfig"`
			}
			if err := json.Unmarshal(payload, &claims); err != nil {
				t.Fatal(err)
			}
			c := claims.RoomConfig
			if c.Min != 0 || c.Max != 0 || !c.Sync || c.Metadata != "ts3stream-server-av-v1:2000" {
				t.Fatalf("server must not request client delay: %+v", c)
			}
		}
		backend.Close()
	}
}

func TestServerBufferRejectsIncompatibleCapabilities(t *testing.T) {
	for _, body := range []string{`{}`, `{"protocol":"wrong","maxDelayMs":5000}`, `{"protocol":"ts3stream-server-av-v1","maxDelayMs":1000}`, strings.Repeat("x", 5000)} {
		backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { _, _ = w.Write([]byte(body)) }))
		if verifyServerBuffer(context.Background(), backend.URL, 2000) == nil {
			t.Error("incompatible backend accepted")
		}
		backend.Close()
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if verifyServerBuffer(ctx, "http://127.0.0.1:1", 2000) == nil {
		t.Error("canceled request accepted")
	}
}
