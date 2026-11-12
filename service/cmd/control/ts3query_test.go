package main

import (
	"bufio"
	"fmt"
	"net"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestEscapeUnescape(t *testing.T) {
	tests := []struct {
		name    string
		raw     string
		escaped string
	}{
		{name: "plain", raw: "hello", escaped: "hello"},
		{name: "space", raw: "hello world", escaped: `hello\sworld`},
		{name: "backslash and slash", raw: `C:\path/with stuff`, escaped: `C:\\path\/with\sstuff`},
		{name: "pipe", raw: "a|b", escaped: `a\pb`},
		{name: "newline and carriage return", raw: "line1\r\nline2", escaped: `line1\r\nline2`},
		{name: "tab", raw: "tab\there", escaped: `tab\there`},
		{name: "bell backspace formfeed vertical tab", raw: "a\ab\bc\fd\ve", escaped: `a\ab\bc\fd\ve`},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := Escape(tt.raw); got != tt.escaped {
				t.Fatalf("Escape(%q) = %q, want %q", tt.raw, got, tt.escaped)
			}
			if got := Unescape(tt.escaped); got != tt.raw {
				t.Fatalf("Unescape(%q) = %q, want %q", tt.escaped, got, tt.raw)
			}
		})
	}

	if got := Unescape(`unknown\x`); got != `unknown\x` {
		t.Fatalf("Unescape preserved unknown escape incorrectly: %q", got)
	}
}

func TestParseTS3Line(t *testing.T) {
	values := parseTS3Line(`clid=7 cluid=abc\/def= nickname=Hello\sWorld`)
	if values["clid"] != "7" {
		t.Fatalf("clid = %q, want %q", values["clid"], "7")
	}
	if values["cluid"] != "abc/def=" {
		t.Fatalf("cluid = %q, want %q", values["cluid"], "abc/def=")
	}
	if values["nickname"] != "Hello World" {
		t.Fatalf("nickname = %q, want %q", values["nickname"], "Hello World")
	}

	empty := parseTS3Line("virtualserver_welcomemessage=")
	if value, ok := empty["virtualserver_welcomemessage"]; !ok || value != "" {
		t.Fatalf("empty value parsed as %q, present=%v", value, ok)
	}
}

func TestParseTS3Error(t *testing.T) {
	id, message := parseTS3Error(`error id=1281 msg=database\sempty\sresult\sset`)
	if id != 1281 {
		t.Fatalf("error id = %d, want 1281", id)
	}
	if message != "database empty result set" {
		t.Fatalf("error message = %q, want %q", message, "database empty result set")
	}

	id, message = parseTS3Error("error id=0 msg=ok")
	if id != 0 || message != "ok" {
		t.Fatalf("ok error parsed as id=%d msg=%q", id, message)
	}
}

// fakeTS3Server implements just enough of the ServerQuery protocol for tests.
type fakeTS3Server struct {
	listener  net.Listener
	onlineUID string
	serverUID string

	mu      sync.Mutex
	welcome string
}

func newFakeTS3Server(t *testing.T, onlineUID, serverUID, welcome string) *fakeTS3Server {
	t.Helper()

	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen fake TS3 query server: %v", err)
	}

	server := &fakeTS3Server{
		listener:  listener,
		onlineUID: onlineUID,
		serverUID: serverUID,
		welcome:   welcome,
	}
	go server.serve()
	t.Cleanup(func() {
		_ = listener.Close()
	})
	return server
}

func (s *fakeTS3Server) addr() string {
	return s.listener.Addr().String()
}

func (s *fakeTS3Server) welcomeMessage() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.welcome
}

func (s *fakeTS3Server) serve() {
	for {
		conn, err := s.listener.Accept()
		if err != nil {
			return
		}
		go s.handle(conn)
	}
}

func (s *fakeTS3Server) handle(conn net.Conn) {
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(10 * time.Second))

	reader := bufio.NewReader(conn)
	_, _ = fmt.Fprint(conn, "TS3\n\r")

	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			return
		}
		line = strings.TrimSpace(line)
		command, arguments, _ := strings.Cut(line, " ")

		switch command {
		case "login", "use":
			fakeTS3Reply(conn, "error id=0 msg=ok")
		case "clientgetids":
			params := parseTS3Line(arguments)
			if params["cluid"] != "" && params["cluid"] == s.onlineUID {
				fakeTS3Reply(conn, "clid=7 cluid="+Escape(params["cluid"])+" nickname=Fake")
				fakeTS3Reply(conn, "error id=0 msg=ok")
				continue
			}
			fakeTS3Reply(conn, `error id=1281 msg=database\sempty\sresult\sset`)
		case "serverinfo":
			s.mu.Lock()
			welcome := s.welcome
			s.mu.Unlock()
			fakeTS3Reply(conn, "virtualserver_unique_identifier="+Escape(s.serverUID)+" virtualserver_welcomemessage="+Escape(welcome))
			fakeTS3Reply(conn, "error id=0 msg=ok")
		case "serveredit":
			params := parseTS3Line(arguments)
			s.mu.Lock()
			s.welcome = params["virtualserver_welcomemessage"]
			s.mu.Unlock()
			fakeTS3Reply(conn, "error id=0 msg=ok")
		case "quit":
			return
		default:
			fakeTS3Reply(conn, `error id=256 msg=command\snot\sfound`)
		}
	}
}

func fakeTS3Reply(conn net.Conn, line string) {
	_, _ = fmt.Fprint(conn, line+"\n\r")
}
