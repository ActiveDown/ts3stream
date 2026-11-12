package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

// Probe the actual SFU, not a claimed version number in an environment variable.
// Stock LiveKit answers unknown paths with OK, which must NOT enable this mode.
func verifyServerBuffer(ctx context.Context, baseURL string, delayMs int) error {
	ctx, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, strings.TrimRight(baseURL, "/")+"/ts3stream/buffering", nil)
	if err != nil {
		return err
	}
	client := &http.Client{CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	response, err := client.Do(req)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	var capability struct {
		Protocol   string `json:"protocol"`
		MaxDelayMs int    `json:"maxDelayMs"`
	}
	if response.StatusCode != http.StatusOK {
		return fmt.Errorf("capability status %d", response.StatusCode)
	}
	if err = json.NewDecoder(io.LimitReader(response.Body, 4096)).Decode(&capability); err != nil {
		return err
	}
	if capability.Protocol != "ts3stream-server-av-v1" || capability.MaxDelayMs < delayMs {
		return fmt.Errorf("incompatible server buffer protocol")
	}
	return nil
}
