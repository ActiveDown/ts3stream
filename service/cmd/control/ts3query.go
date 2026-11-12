package main

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"net"
	"strconv"
	"strings"
	"time"
)

const (
	defaultTS3QueryTimeout = 5 * time.Second
	defaultTS3QueryUser    = "serveradmin"
	defaultTS3QuerySID     = "1"
)

// ts3QueryConfig holds the TeamSpeak 3 ServerQuery connection settings.
type ts3QueryConfig struct {
	addr     string
	user     string
	password string
	sid      string
}

// configured reports whether the query connection has enough settings to be
// used. An empty address or password disables query-backed features.
func (c ts3QueryConfig) configured() bool {
	return strings.TrimSpace(c.addr) != "" && c.password != ""
}

// ts3QueryError is a non-zero ServerQuery error response.
type ts3QueryError struct {
	ID      int
	Message string
}

func (e *ts3QueryError) Error() string {
	return fmt.Sprintf("TS3 query error %d: %s", e.ID, e.Message)
}

// ts3QueryClient is a minimal TeamSpeak 3 ServerQuery client.
type ts3QueryClient struct {
	conn    net.Conn
	reader  *bufio.Reader
	timeout time.Duration
}

// newTS3QueryClient connects, reads the TS3 banner, and logs in with the
// configured credentials before selecting the virtual server.
func newTS3QueryClient(cfg ts3QueryConfig, timeout time.Duration) (*ts3QueryClient, error) {
	addr := strings.TrimSpace(cfg.addr)
	if addr == "" {
		return nil, errors.New("TS3 query address is not configured")
	}
	if timeout <= 0 {
		timeout = defaultTS3QueryTimeout
	}

	conn, err := net.DialTimeout("tcp", addr, timeout)
	if err != nil {
		return nil, fmt.Errorf("connect to TS3 query: %w", err)
	}

	client := &ts3QueryClient{
		conn:    conn,
		reader:  bufio.NewReader(conn),
		timeout: timeout,
	}
	if err := client.authenticate(cfg); err != nil {
		_ = conn.Close()
		return nil, err
	}
	return client, nil
}

func (c *ts3QueryClient) authenticate(cfg ts3QueryConfig) error {
	if err := c.setDeadline(); err != nil {
		return err
	}

	banner, err := c.readLine()
	if err != nil {
		return fmt.Errorf("read TS3 query banner: %w", err)
	}
	if !strings.HasPrefix(banner, "TS3") {
		return fmt.Errorf("unexpected TS3 query banner %q", banner)
	}

	user := strings.TrimSpace(cfg.user)
	if user == "" {
		user = defaultTS3QueryUser
	}
	if _, err := c.exec("login " + Escape(user) + " " + Escape(cfg.password)); err != nil {
		return fmt.Errorf("TS3 query login failed: %w", err)
	}

	sid := strings.TrimSpace(cfg.sid)
	if sid == "" {
		sid = defaultTS3QuerySID
	}
	if _, err := c.exec("use sid=" + Escape(sid)); err != nil {
		return fmt.Errorf("TS3 query use sid failed: %w", err)
	}
	return nil
}

// Close closes the ServerQuery connection.
func (c *ts3QueryClient) Close() error {
	if c == nil || c.conn == nil {
		return nil
	}
	return c.conn.Close()
}

func (c *ts3QueryClient) setDeadline() error {
	if c.timeout <= 0 {
		return nil
	}
	return c.conn.SetDeadline(time.Now().Add(c.timeout))
}

// readLine reads one ServerQuery line. Raw CR bytes are framing and are
// discarded, so both \n and \r\n or \n\r line endings are accepted.
func (c *ts3QueryClient) readLine() (string, error) {
	var line strings.Builder
	for {
		b, err := c.reader.ReadByte()
		if err != nil {
			if errors.Is(err, io.EOF) && line.Len() > 0 {
				return line.String(), nil
			}
			return "", err
		}
		switch b {
		case '\n':
			return line.String(), nil
		case '\r':
			continue
		default:
			line.WriteByte(b)
		}
	}
}

// exec sends a single command and returns its data lines. The terminating
// "error id=... msg=..." line becomes a *ts3QueryError when id is non-zero.
func (c *ts3QueryClient) exec(command string) ([]string, error) {
	if err := c.setDeadline(); err != nil {
		return nil, err
	}
	if _, err := io.WriteString(c.conn, command+"\n"); err != nil {
		return nil, err
	}

	var lines []string
	for {
		line, err := c.readLine()
		if err != nil {
			return nil, err
		}
		if line == "" {
			continue
		}
		if strings.HasPrefix(line, "error ") || line == "error" {
			code, message := parseTS3Error(line)
			if code != 0 {
				return lines, &ts3QueryError{ID: code, Message: message}
			}
			return lines, nil
		}
		lines = append(lines, line)
	}
}

// ClientOnline reports whether a client with the given unique identifier is
// currently connected to the selected virtual server.
func (c *ts3QueryClient) ClientOnline(uid string) (bool, error) {
	uid = strings.TrimSpace(uid)
	if uid == "" {
		return false, errors.New("client UID is empty")
	}

	lines, err := c.exec("clientgetids cluid=" + Escape(uid))
	if err != nil {
		var queryErr *ts3QueryError
		if errors.As(err, &queryErr) && queryErr.ID == 1281 {
			return false, nil
		}
		return false, err
	}

	for _, line := range lines {
		for _, record := range strings.Split(line, "|") {
			if _, ok := parseTS3Line(record)["clid"]; ok {
				return true, nil
			}
		}
	}
	return false, nil
}

// ServerUniqueID returns the selected virtual server's unique identifier.
func (c *ts3QueryClient) ServerUniqueID() (string, error) {
	return c.serverInfoValue("virtualserver_unique_identifier")
}

// WelcomeMessage returns the selected virtual server's welcome message.
func (c *ts3QueryClient) WelcomeMessage() (string, error) {
	return c.serverInfoValue("virtualserver_welcomemessage")
}

func (c *ts3QueryClient) serverInfoValue(key string) (string, error) {
	lines, err := c.exec("serverinfo")
	if err != nil {
		return "", err
	}
	for _, line := range lines {
		for _, record := range strings.Split(line, "|") {
			if value, ok := parseTS3Line(record)[key]; ok {
				return value, nil
			}
		}
	}
	return "", fmt.Errorf("%s is missing from TS3 serverinfo response", key)
}

// SetWelcomeMessage replaces the selected virtual server's welcome message.
func (c *ts3QueryClient) SetWelcomeMessage(message string) error {
	_, err := c.exec("serveredit virtualserver_welcomemessage=" + Escape(message))
	return err
}

// Escape encodes a value for use as a ServerQuery parameter.
func Escape(value string) string {
	return ts3EscapeReplacer.Replace(value)
}

// Unescape decodes a ServerQuery parameter value.
func Unescape(value string) string {
	if !strings.Contains(value, `\`) {
		return value
	}

	var result strings.Builder
	result.Grow(len(value))
	for i := 0; i < len(value); i++ {
		if value[i] != '\\' || i+1 >= len(value) {
			result.WriteByte(value[i])
			continue
		}
		i++
		switch value[i] {
		case '\\':
			result.WriteByte('\\')
		case '/':
			result.WriteByte('/')
		case 's':
			result.WriteByte(' ')
		case 'p':
			result.WriteByte('|')
		case 'a':
			result.WriteByte('\a')
		case 'b':
			result.WriteByte('\b')
		case 'f':
			result.WriteByte('\f')
		case 'n':
			result.WriteByte('\n')
		case 'r':
			result.WriteByte('\r')
		case 't':
			result.WriteByte('\t')
		case 'v':
			result.WriteByte('\v')
		default:
			// Preserve unknown escape sequences verbatim.
			result.WriteByte('\\')
			result.WriteByte(value[i])
		}
	}
	return result.String()
}

var ts3EscapeReplacer = strings.NewReplacer(
	`\`, `\\`,
	`/`, `\/`,
	` `, `\s`,
	`|`, `\p`,
	"\a", `\a`,
	"\b", `\b`,
	"\f", `\f`,
	"\n", `\n`,
	"\r", `\r`,
	"\t", `\t`,
	"\v", `\v`,
)

// parseTS3Line parses a "key=value key=value" ServerQuery line. Values are
// unescaped; the first record of multi-record data should be split on "|"
// by the caller.
func parseTS3Line(line string) map[string]string {
	values := make(map[string]string)
	for _, field := range strings.Split(line, " ") {
		if field == "" {
			continue
		}
		key, value, ok := strings.Cut(field, "=")
		if !ok {
			values[field] = ""
			continue
		}
		values[key] = Unescape(value)
	}
	return values
}

func parseTS3Error(line string) (int, string) {
	line = strings.TrimSpace(strings.TrimPrefix(line, "error"))
	values := parseTS3Line(line)
	id, _ := strconv.Atoi(values["id"])
	return id, values["msg"]
}
