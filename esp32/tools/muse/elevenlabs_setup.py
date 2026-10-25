#!/usr/bin/env python3
"""Provision direct ElevenLabs credentials over Muse's physical USB console.

API keys are read with hidden input; no files, environment variables or shell
history. Requires pyserial (the same dependency as tools/muse/chat.py).
"""
import argparse
import getpass
import sys

from chat import Board, BoardError


def main() -> int:
    p = argparse.ArgumentParser(description="Configure direct ElevenLabs on Muse Gadget")
    p.add_argument("--port", required=True, help="Verified /dev/cu.* port of your ESP32")
    p.add_argument("--clear", action="store_true", help="Erase stored ElevenLabs credentials")
    args = p.parse_args()
    if not args.port.startswith("/dev/"):
        p.error("Provide the verified /dev/... serial port to avoid another board")

    if args.clear:
        cmds = ["tts.clear"]
    else:
        key = getpass.getpass("ElevenLabs API key (hidden): ").strip()
        voice = input("ElevenLabs Voice ID: ").strip()
        if not key or len(key) > 255 or any(ord(c) <= 32 or ord(c) >= 127 for c in key):
            p.error("Invalid API key (1-255 printable ASCII characters without spaces)")
        if not voice or len(voice) > 63 or not all(c.isascii() and (c.isalnum() or c in "_-") for c in voice):
            p.error("Invalid Voice ID (1-63 ASCII letters/digits/_/-)")
        cmds = ["tts.key=" + key, "tts.voice=" + voice]
    try:
        with Board(args.port) as board:
            board.wake()
            for cmd in cmds:
                board.write_line(cmd)
                # The firmware serial command handler logs only the key name.
                # Avoid printing cmd: it contains the secret.
                if cmd.startswith("tts.key="):
                    cmd = "tts.key=<hidden>"
            board.write_line("tts.status")
    except (BoardError, OSError) as exc:
        print("USB configuration failed: " + str(exc), file=sys.stderr)
        return 1
    finally:
        if not args.clear:
            key = ""
    print("Configuration commands sent. Reconnect to Muse and test a voice reply.")
    print("Check the serial logs for 'tts.key -> ok' and 'tts.voice -> ok'.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
