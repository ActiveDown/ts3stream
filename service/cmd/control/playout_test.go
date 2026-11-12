package main

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"testing"
	"time"
)

func TestTokenPlayoutRoomContract(t *testing.T) {
	for _, delay := range []int{0, 2000, 5000} {
		for _, publish := range []string{"0", "1"} {
			t.Run(fmt.Sprintf("delay-%d-publish-%s", delay, publish), func(t *testing.T) {
				api := newTestAPIWithConfig(t, config{
					apiKey: "test-api-key", apiSecret: "test-api-secret", tokenTTL: time.Minute,
					playoutDelayMs: delay,
				})
				status, body := api.do(t, http.MethodGet,
					"/api/token?room=test-room&identity=test-person&publish="+publish, "", "")
				if status != http.StatusOK {
					t.Fatalf("status=%d", status)
				}
				var response tokenResponse
				if err := json.Unmarshal(body, &response); err != nil {
					t.Fatal(err)
				}
				parts := strings.Split(response.Token, ".")
				if len(parts) != 3 {
					t.Fatal("invalid JWT")
				}
				payload, err := base64.RawURLEncoding.DecodeString(parts[1])
				if err != nil {
					t.Fatal(err)
				}
				var claims struct {
					RoomConfig *struct {
						Min      int    `json:"minPlayoutDelay"`
						Max      int    `json:"maxPlayoutDelay"`
						Sync     bool   `json:"syncStreams"`
						Metadata string `json:"metadata"`
					} `json:"roomConfig"`
				}
				if err := json.Unmarshal(payload, &claims); err != nil {
					t.Fatal(err)
				}
				if delay == 0 {
					if claims.RoomConfig != nil {
						t.Fatal("disabled mode changed room defaults")
					}
					return
				}
				c := claims.RoomConfig
				if c == nil || c.Min != delay || c.Max != delay || !c.Sync ||
					c.Metadata != fmt.Sprintf("ts3stream-av-delay-v1:%d", delay) {
					t.Fatalf("video settings and audio metadata disagree: %+v", c)
				}
			})
		}
	}
}
