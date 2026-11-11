package main

import (
	"fmt"
	"sync"
	"testing"
	"time"
)

func TestStreamRegistryLifecycle(t *testing.T) {
	now := time.Date(2026, 11, 12, 12, 0, 0, 0, time.UTC)
	registry := newStreamRegistry(streamTTL)
	registry.now = func() time.Time { return now }

	registry.start(streamEntry{
		Room:      "room-a",
		Identity:  "user-1",
		Nickname:  "Alice",
		ChannelID: 42,
		ServerUID: "server-1",
	})

	streams := registry.list("")
	if len(streams) != 1 {
		t.Fatalf("list() returned %d streams, want 1", len(streams))
	}
	entry := streams[0]
	if entry.Room != "room-a" || entry.Identity != "user-1" || entry.Nickname != "Alice" {
		t.Fatalf("unexpected entry: %+v", entry)
	}
	if entry.ChannelID != 42 || entry.ServerUID != "server-1" {
		t.Fatalf("unexpected TS3 metadata: %+v", entry)
	}
	if entry.LastSeen != now.UnixMilli() {
		t.Fatalf("lastSeen = %d, want %d", entry.LastSeen, now.UnixMilli())
	}

	now = now.Add(5 * time.Second)
	if !registry.heartbeat("room-a", "user-1", "") {
		t.Fatal("heartbeat() = false for an existing stream")
	}
	streams = registry.list("room-a")
	if len(streams) != 1 {
		t.Fatalf("list(room-a) returned %d streams, want 1", len(streams))
	}
	if streams[0].Nickname != "Alice" {
		t.Fatalf("empty heartbeat nickname changed nickname to %q", streams[0].Nickname)
	}
	if streams[0].LastSeen != now.UnixMilli() {
		t.Fatalf("heartbeat did not refresh lastSeen: got %d, want %d", streams[0].LastSeen, now.UnixMilli())
	}

	if registry.heartbeat("room-a", "missing", "") {
		t.Fatal("heartbeat() = true for an unknown stream")
	}

	now = now.Add(time.Second)
	if !registry.heartbeat("room-a", "user-1", "Alice 2") {
		t.Fatal("heartbeat() = false for an existing stream")
	}
	streams = registry.list("room-a")
	if len(streams) != 1 || streams[0].Nickname != "Alice 2" {
		t.Fatalf("nickname was not updated: %+v", streams)
	}
	if got := registry.list("room-b"); len(got) != 0 {
		t.Fatalf("list(room-b) returned %d streams, want 0", len(got))
	}

	if !registry.stop("room-a", "user-1") {
		t.Fatal("stop() = false for an existing stream")
	}
	if registry.stop("room-a", "user-1") {
		t.Fatal("stop() = true for a missing stream")
	}
	if got := registry.list(""); len(got) != 0 {
		t.Fatalf("list() after stop returned %d streams, want 0", len(got))
	}
}

func TestStreamRegistryTTLPruning(t *testing.T) {
	now := time.Date(2026, 11, 12, 12, 0, 0, 0, time.UTC)
	registry := newStreamRegistry(streamTTL)
	registry.now = func() time.Time { return now }

	registry.start(streamEntry{Room: "room-a", Identity: "user-1"})
	registry.start(streamEntry{Room: "room-b", Identity: "user-2"})

	// Exactly at the TTL boundary entries are still considered live.
	now = now.Add(streamTTL)
	if got := registry.list("room-a"); len(got) != 1 {
		t.Fatalf("list(room-a) at TTL boundary returned %d streams, want 1", len(got))
	}

	now = now.Add(time.Millisecond)
	if got := registry.list("room-a"); len(got) != 0 {
		t.Fatalf("list(room-a) after TTL returned %d streams, want 0", len(got))
	}

	registry.mu.Lock()
	remaining := len(registry.streams)
	registry.mu.Unlock()
	if remaining != 0 {
		t.Fatalf("%d expired entries were not pruned", remaining)
	}
}

func TestStreamRegistryHeartbeatAfterExpiry(t *testing.T) {
	now := time.Date(2026, 11, 12, 12, 0, 0, 0, time.UTC)
	registry := newStreamRegistry(streamTTL)
	registry.now = func() time.Time { return now }

	registry.start(streamEntry{Room: "room-a", Identity: "user-1"})
	now = now.Add(streamTTL + time.Millisecond)

	if registry.heartbeat("room-a", "user-1", "") {
		t.Fatal("heartbeat() resurrected an expired stream")
	}

	registry.mu.Lock()
	remaining := len(registry.streams)
	registry.mu.Unlock()
	if remaining != 0 {
		t.Fatalf("expired entry was not removed, %d remain", remaining)
	}
}

func TestStreamRegistryConcurrentAccess(t *testing.T) {
	registry := newStreamRegistry(streamTTL)

	var wg sync.WaitGroup
	for i := 0; i < 50; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()

			identity := fmt.Sprintf("user-%d", i%10)
			registry.start(streamEntry{Room: "room-a", Identity: identity})
			registry.heartbeat("room-a", identity, "")
			registry.list("")
			registry.stop("room-a", identity)
		}(i)
	}
	wg.Wait()
}
