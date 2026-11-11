package main

import (
	"sort"
	"sync"
	"time"
)

// streamTTL is how long a stream stays live after its last start/heartbeat.
const streamTTL = 15 * time.Second

type streamEntry struct {
	Room      string `json:"room"`
	Identity  string `json:"identity"`
	Nickname  string `json:"nickname"`
	ChannelID uint64 `json:"channelId"`
	ServerUID string `json:"serverUid"`
	LastSeen  int64  `json:"lastSeen"` // Unix timestamp in milliseconds.
}

type streamKey struct {
	room     string
	identity string
}

// streamRegistry is an in-memory, concurrency-safe registry of live streams.
type streamRegistry struct {
	mu      sync.Mutex
	ttl     time.Duration
	now     func() time.Time
	streams map[streamKey]streamEntry
}

func newStreamRegistry(ttl time.Duration) *streamRegistry {
	if ttl <= 0 {
		ttl = streamTTL
	}
	return &streamRegistry{
		ttl:     ttl,
		now:     time.Now,
		streams: make(map[streamKey]streamEntry),
	}
}

// start stores a new stream entry or refreshes an existing one.
func (r *streamRegistry) start(entry streamEntry) {
	r.mu.Lock()
	defer r.mu.Unlock()

	entry.LastSeen = r.now().UnixMilli()
	r.streams[streamKey{room: entry.Room, identity: entry.Identity}] = entry
}

// heartbeat refreshes an existing stream. Expired entries are treated as
// unknown so a stale start cannot be resurrected by a late heartbeat.
func (r *streamRegistry) heartbeat(room, identity, nickname string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()

	key := streamKey{room: room, identity: identity}
	entry, ok := r.streams[key]
	if !ok {
		return false
	}

	now := r.now()
	if r.expired(entry, now) {
		delete(r.streams, key)
		return false
	}

	entry.LastSeen = now.UnixMilli()
	if nickname != "" {
		entry.Nickname = nickname
	}
	r.streams[key] = entry
	return true
}

// stop removes a stream if present. It reports whether an entry was removed.
func (r *streamRegistry) stop(room, identity string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()

	key := streamKey{room: room, identity: identity}
	if _, ok := r.streams[key]; !ok {
		return false
	}
	delete(r.streams, key)
	return true
}

// list returns non-expired streams, optionally filtered by room. Expired
// entries are pruned as a side effect, including entries for other rooms.
func (r *streamRegistry) list(room string) []streamEntry {
	r.mu.Lock()
	defer r.mu.Unlock()

	now := r.now()
	result := make([]streamEntry, 0, len(r.streams))
	for key, entry := range r.streams {
		if r.expired(entry, now) {
			delete(r.streams, key)
			continue
		}
		if room != "" && entry.Room != room {
			continue
		}
		result = append(result, entry)
	}

	sort.Slice(result, func(i, j int) bool {
		if result[i].Room != result[j].Room {
			return result[i].Room < result[j].Room
		}
		return result[i].Identity < result[j].Identity
	})
	return result
}

func (r *streamRegistry) expired(entry streamEntry, now time.Time) bool {
	return now.Sub(time.UnixMilli(entry.LastSeen)) > r.ttl
}
