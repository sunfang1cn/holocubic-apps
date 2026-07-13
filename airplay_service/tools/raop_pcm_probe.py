#!/usr/bin/env python3
"""Send a short unencrypted L16 RAOP session to airplay_service.

This is deliberately a protocol probe, not an AirPlay implementation.  It
validates RTSP framing, transport negotiation, DMAP metadata and the RTP/I2S
PCM path independently of the native RSA/ALAC sender implementation.
"""

from __future__ import annotations

import argparse
import math
import random
import socket
import struct
import time
from dataclasses import dataclass


@dataclass
class RtspResponse:
    code: int
    reason: str
    headers: dict[str, str]
    body: bytes


class RtspClient:
    def __init__(self, host: str, port: int, timeout: float) -> None:
        self.host = host
        self.port = port
        self.socket = socket.create_connection((host, port), timeout=timeout)
        self.socket.settimeout(timeout)
        self.cseq = 0
        self.session: str | None = None
        self.pending = bytearray()

    def close(self) -> None:
        self.socket.close()

    def request(
        self,
        method: str,
        uri: str = "*",
        *,
        headers: dict[str, str] | None = None,
        body: bytes = b"",
        expected: int = 200,
    ) -> RtspResponse:
        self.cseq += 1
        request_headers = {
            "CSeq": str(self.cseq),
            "User-Agent": "HoloCubic-RAOP-Probe/0.1",
        }
        if self.session:
            request_headers["Session"] = self.session
        if headers:
            request_headers.update(headers)
        if body:
            request_headers["Content-Length"] = str(len(body))

        lines = [f"{method} {uri} RTSP/1.0"]
        lines.extend(f"{key}: {value}" for key, value in request_headers.items())
        wire = ("\r\n".join(lines) + "\r\n\r\n").encode("ascii") + body
        self.socket.sendall(wire)
        response = self._read_response()
        if response.code != expected:
            error = response.headers.get("x-airplay-error", "")
            raise RuntimeError(
                f"{method} returned {response.code} {response.reason} {error}".rstrip()
            )
        if response.headers.get("session"):
            self.session = response.headers["session"].split(";", 1)[0]
        return response

    def _read_response(self) -> RtspResponse:
        marker = b"\r\n\r\n"
        while marker not in self.pending:
            chunk = self.socket.recv(4096)
            if not chunk:
                raise ConnectionError("RTSP socket closed while reading headers")
            self.pending.extend(chunk)
        head, remainder = bytes(self.pending).split(marker, 1)
        lines = head.decode("iso-8859-1").split("\r\n")
        protocol, code, reason = lines[0].split(" ", 2)
        if protocol != "RTSP/1.0":
            raise RuntimeError(f"unexpected RTSP protocol: {protocol}")
        headers: dict[str, str] = {}
        for line in lines[1:]:
            key, value = line.split(":", 1)
            headers[key.strip().lower()] = value.strip()
        length = int(headers.get("content-length", "0"))
        self.pending = bytearray(remainder)
        while len(self.pending) < length:
            chunk = self.socket.recv(4096)
            if not chunk:
                raise ConnectionError("RTSP socket closed while reading body")
            self.pending.extend(chunk)
        body = bytes(self.pending[:length])
        del self.pending[:length]
        return RtspResponse(int(code), reason, headers, body)


def dmap_tag(name: str, value: bytes) -> bytes:
    if len(name) != 4:
        raise ValueError("DMAP tags must contain four characters")
    return name.encode("ascii") + struct.pack(">I", len(value)) + value


def metadata(title: str, artist: str, album: str) -> bytes:
    item = b"".join(
        (
            dmap_tag("minm", title.encode("utf-8")),
            dmap_tag("asar", artist.encode("utf-8")),
            dmap_tag("asal", album.encode("utf-8")),
        )
    )
    return dmap_tag("mlit", item)


def parse_server_audio_port(transport: str) -> int:
    for item in transport.split(";"):
        key, separator, value = item.partition("=")
        if separator and key.strip().lower() == "server_port":
            return int(value)
    raise RuntimeError(f"SETUP response has no server_port: {transport}")


def send_pcm(
    target: tuple[str, int],
    *,
    duration: float,
    frequency: float,
    amplitude: float,
) -> int:
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sequence = random.randrange(0, 65536)
    timestamp = random.randrange(0, 2**32)
    ssrc = random.randrange(0, 2**32)
    sample_rate = 44100
    frames_per_packet = 352
    packet_duration = frames_per_packet / sample_rate
    packet_count = max(1, math.ceil(duration / packet_duration))
    start = time.perf_counter()

    try:
        for packet_index in range(packet_count):
            payload = bytearray()
            base_frame = packet_index * frames_per_packet
            for frame in range(frames_per_packet):
                phase = 2 * math.pi * frequency * (base_frame + frame) / sample_rate
                sample = int(max(-1.0, min(1.0, math.sin(phase) * amplitude)) * 32767)
                payload.extend(struct.pack(">hh", sample, sample))
            header = struct.pack(
                ">BBHII",
                0x80,
                0x60,
                sequence,
                timestamp,
                ssrc,
            )
            udp.sendto(header + payload, target)
            sequence = (sequence + 1) & 0xFFFF
            timestamp = (timestamp + frames_per_packet) & 0xFFFFFFFF
            deadline = start + (packet_index + 1) * packet_duration
            remaining = deadline - time.perf_counter()
            if remaining > 0:
                time.sleep(remaining)
    finally:
        udp.close()
    return packet_count


def run(args: argparse.Namespace) -> None:
    control = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    timing = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    control.bind(("0.0.0.0", 0))
    timing.bind(("0.0.0.0", 0))
    control_port = control.getsockname()[1]
    timing_port = timing.getsockname()[1]
    rtsp = RtspClient(args.host, args.port, args.timeout)
    uri = f"rtsp://{args.host}/{random.randrange(1, 2**31)}"

    try:
        print("OPTIONS")
        rtsp.request("OPTIONS")

        sdp = (
            "v=0\r\n"
            "o=HoloCubicProbe 1 0 IN IP4 127.0.0.1\r\n"
            "s=HoloCubic PCM probe\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "t=0 0\r\n"
            "m=audio 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 L16/44100/2\r\n"
        ).encode("ascii")
        print("ANNOUNCE L16/44100/2")
        rtsp.request(
            "ANNOUNCE",
            uri,
            headers={"Content-Type": "application/sdp"},
            body=sdp,
        )

        print(f"SETUP control={control_port} timing={timing_port}")
        setup = rtsp.request(
            "SETUP",
            uri,
            headers={
                "Transport": (
                    "RTP/AVP/UDP;unicast;mode=record;"
                    f"control_port={control_port};timing_port={timing_port}"
                )
            },
        )
        audio_port = parse_server_audio_port(setup.headers["transport"])

        tagged = metadata(args.title, args.artist, args.album)
        print("SET_PARAMETER metadata")
        rtsp.request(
            "SET_PARAMETER",
            uri,
            headers={"Content-Type": "application/x-dmap-tagged"},
            body=tagged,
        )
        rtsp.request(
            "SET_PARAMETER",
            uri,
            headers={"Content-Type": "text/parameters"},
            body=b"volume: -12.0\r\n",
        )

        print(f"RECORD -> UDP {args.host}:{audio_port}")
        rtsp.request("RECORD", uri, headers={"Range": "npt=0-"})
        packets = send_pcm(
            (args.host, audio_port),
            duration=args.duration,
            frequency=args.frequency,
            amplitude=args.amplitude,
        )
        print(f"sent {packets} RTP packets")
        rtsp.request("TEARDOWN", uri)
        print("PASS")
    finally:
        rtsp.close()
        control.close()
        timing.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="HoloCubic IPv4 address")
    parser.add_argument("--port", type=int, default=5000, help="RTSP port")
    parser.add_argument("--duration", type=float, default=3.0, help="tone duration")
    parser.add_argument("--frequency", type=float, default=440.0, help="tone frequency")
    parser.add_argument("--amplitude", type=float, default=0.2, help="0.0 to 1.0")
    parser.add_argument("--timeout", type=float, default=5.0, help="socket timeout")
    parser.add_argument("--title", default="RAOP PCM Probe")
    parser.add_argument("--artist", default="HoloCubic")
    parser.add_argument("--album", default="AirPlay Service Test")
    args = parser.parse_args()
    if not 0 <= args.amplitude <= 1:
        parser.error("--amplitude must be between 0 and 1")
    run(args)


if __name__ == "__main__":
    main()
