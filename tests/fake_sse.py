#!/usr/bin/env python3
"""
fake_sse.py — deterministic 50 tok/s SSE server with U+2063 ns sentinel.

Usage:
  python3 tests/fake_sse.py --port 8765 --tokens 500
  PORT=0 means pick random free port and print it to stdout.

Each token's content is: "\u2063<MONO_NS>\u2063tok<i> "
Rate: 50 tok/s => 20ms interval.

Handles any POST to *chat/completions* and GET *models*.
"""
import argparse, time, json, threading, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

U2063="\u2063"

class Handler(BaseHTTPRequestHandler):
    tokens = 500
    rate = 50
    msg = ""   # optional full readable message to stream word-by-word
    def do_GET(self):
        if "models" in self.path:
            body=json.dumps({"data":[{"id":"mock-model"},{"id":"deepseek/deepseek-chat"}]}).encode()
            self.send_response(200)
            self.send_header("Content-Type","application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(404)
        self.end_headers()
    def do_POST(self):
        # chat completions
        length=int(self.headers.get("Content-Length",0) or 0)
        if length: self.rfile.read(length)
        # accept any chat/completions path
        self.send_response(200)
        self.send_header("Content-Type","text/event-stream")
        self.send_header("Cache-Control","no-cache")
        self.send_header("Connection","close")
        # CORS
        self.send_header("Access-Control-Allow-Origin","*")
        self.end_headers()
        # SSE stream: first role chunk
        def send_chunk(obj):
            data=json.dumps(obj, ensure_ascii=False)
            msg=f"data: {data}\n\n"
            try:
                self.wfile.write(msg.encode())
                self.wfile.flush()
            except BrokenPipeError:
                return False
            return True
        # initial role
        send_chunk({"id":"1","choices":[{"index":0,"delta":{"role":"assistant"},"finish_reason":None}]})
        interval = 1.0 / Handler.rate
        for i in range(Handler.tokens):
            mono_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            if Handler.msg:
                words = Handler.msg.split(" ")
                if i < len(words):
                    tok_text = f"{U2063}{mono_ns}{U2063}{words[i]} "
                else:
                    tok_text = ""
                if tok_text == "" and i >= len(words):
                    break
            else:
                tok_text = f"{U2063}{mono_ns}{U2063}tok{i} "
            if not send_chunk({"id":"1","choices":[{"index":0,"delta":{"content":tok_text},"finish_reason":None}]}):
                break
            time.sleep(interval)
        send_chunk({"id":"1","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]})
        try:
            self.wfile.write(b"data: [DONE]\n\n")
        except: pass

    def log_message(self, fmt, *args):
        pass

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--tokens", type=int, default=500)
    ap.add_argument("--rate", type=int, default=50)
    ap.add_argument("--text", type=str, default="")
    args=ap.parse_args()
    Handler.tokens=args.tokens
    Handler.rate=args.rate
    Handler.msg=args.text
    srv=ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    port=srv.server_address[1]
    print(port, flush=True)
    # also print to stderr for harness
    print(f"[fake_sse] listening 127.0.0.1:{port} tokens={args.tokens} rate={args.rate}", file=sys.stderr)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass

if __name__=="__main__":
    main()
