import argparse
import random
import re
import socket
import sys
import time
from dataclasses import dataclass


PC_BIND_HOST = "0.0.0.0"
PC_PORT = 5005

WIDTH = 900
HEIGHT = 500
FPS = 60

BALL_RADIUS = 16
BALL_Y = 250

PITCH_START_X = 820
HIT_ZONE_X = 220
BAT_X = HIT_ZONE_X

BALL_SPEED_PX_PER_SEC = 400

READY_MIN_DELAY = 1.0
READY_MAX_DELAY = 2.5

GOOD_WINDOW_MS = 120
EARLY_LATE_LIMIT_MS = 350

HEARTBEAT_INTERVAL_S = 1.0
LINK_STALE_S = 5.0


game_ready_re = re.compile(r"GAME_READY")

pitch_sync_re = re.compile(
    r"PITCH_SYNC(?: round=(?P<round>\d+))? t=(?P<t>\d+)"
)

swing_start_re = re.compile(
    r"SWING_START t=(?P<t>\d+)"
)

swing_end_re = re.compile(
    r"SWING_END t=(?P<t>\d+) "
    r"dur=(?P<dur>\d+) "
    r"peak_t=(?P<peak_t>\d+) "
    r"start_rt=(?P<start_rt>\d+) "
    r"peak_rt=(?P<peak_rt>\d+) "
    r"peak_dps=(?P<peak_dps>\d+) "
    r"speed_x100=(?P<speed_x100>\d+) "
    r"drop=(?P<drop>\d+)"
)


@dataclass
class SwingEvent:
    stm32_end_t: int
    duration_ms: int
    peak_t: int
    start_rt_ms: int
    peak_rt_ms: int
    peak_dps: int
    speed_m_s: float
    dropped: int


class UdpSwingReader:
    def __init__(self, bind_host: str, bind_port: int, stm32_addr=None):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((bind_host, bind_port))
        self.sock.setblocking(False)

        self.stm32_addr = stm32_addr
        self.pitch_sync_t = None
        self.pitch_sync_round = None
        self.last_line = ""
        self.last_rx_time = None
        self.last_ping_time = 0.0

        print(f"PC listening on UDP {bind_host}:{bind_port}")
        if self.stm32_addr is not None:
            print(f"STM32 target preset: {self.stm32_addr[0]}:{self.stm32_addr[1]}")

    def close(self):
        self.sock.close()

    def reset_input(self):
        self.pitch_sync_t = None
        self.pitch_sync_round = None

    def send_pitch(self, round_id: int):
        self.pitch_sync_t = None
        self.pitch_sync_round = None

        if self.stm32_addr is None:
            print("Cannot send PITCH yet: STM32 address unknown")
            return False

        msg = f"PITCH round={round_id}\n".encode()
        self.sock.sendto(msg, self.stm32_addr)
        print(f"sent: {msg.decode().strip()} to {self.stm32_addr}")
        return True

    def send_ping(self, now: float):
        if self.stm32_addr is None:
            return

        if now - self.last_ping_time < HEARTBEAT_INTERVAL_S:
            return

        self.sock.sendto(b"PING\n", self.stm32_addr)
        self.last_ping_time = now

    def poll_lines(self):
        lines = []

        while True:
            try:
                data, addr = self.sock.recvfrom(2048)
            except BlockingIOError:
                break

            text = data.decode(errors="replace")
            if self.stm32_addr is None:
                self.stm32_addr = addr
                print(f"STM32 addr locked: {self.stm32_addr}")

            self.last_rx_time = time.monotonic()

            for line in text.replace("\r", "\n").split("\n"):
                line = line.strip()
                if line:
                    lines.append(line)

        return lines

    def is_link_ready(self):
        return self.stm32_addr is not None

    def link_status_text(self):
        if self.stm32_addr is None:
            return "waiting for STM32"

        if self.last_rx_time is None:
            return f"{self.stm32_addr[0]}:{self.stm32_addr[1]} ready"

        age = time.monotonic() - self.last_rx_time
        if age > LINK_STALE_S:
            return f"{self.stm32_addr[0]}:{self.stm32_addr[1]} reconnecting"

        return f"{self.stm32_addr[0]}:{self.stm32_addr[1]} linked"

    def poll(self):
        for line in self.poll_lines():
            self.last_line = line
            print(line)

            m_sync = pitch_sync_re.search(line)
            if m_sync:
                self.pitch_sync_t = int(m_sync.group("t"))
                if m_sync.group("round") is not None:
                    self.pitch_sync_round = int(m_sync.group("round"))
                continue

            if swing_start_re.search(line):
                continue

            m_end = swing_end_re.search(line)
            if m_end:
                return SwingEvent(
                    stm32_end_t=int(m_end.group("t")),
                    duration_ms=int(m_end.group("dur")),
                    peak_t=int(m_end.group("peak_t")),
                    start_rt_ms=int(m_end.group("start_rt")),
                    peak_rt_ms=int(m_end.group("peak_rt")),
                    peak_dps=int(m_end.group("peak_dps")),
                    speed_m_s=int(m_end.group("speed_x100")) / 100.0,
                    dropped=int(m_end.group("drop")),
                )

        return None


def classify_timing(error_ms: float) -> str:
    if abs(error_ms) <= GOOD_WINDOW_MS:
        return "GOOD"

    if error_ms < -EARLY_LATE_LIMIT_MS:
        return "WAY EARLY"

    if error_ms < -GOOD_WINDOW_MS:
        return "EARLY"

    if error_ms > EARLY_LATE_LIMIT_MS:
        return "MISS / TOO LATE"

    return "LATE"


def result_score(result: str) -> int:
    if result == "GOOD":
        return 100
    if result in ("EARLY", "LATE"):
        return 40
    return 0


def draw_text(screen, font, text, x, y, color=(255, 255, 255)):
    surface = font.render(text, True, color)
    screen.blit(surface, (x, y))


def draw_center_text(screen, font, text, y, color=(255, 255, 255)):
    surface = font.render(text, True, color)
    rect = surface.get_rect(center=(WIDTH // 2, y))
    screen.blit(surface, rect)


def parse_args():
    parser = argparse.ArgumentParser(description="Smart Bat UDP reaction game")
    parser.add_argument("--bind-host", default=PC_BIND_HOST)
    parser.add_argument("--bind-port", type=int, default=PC_PORT)
    parser.add_argument("--stm32-ip", default=None)
    parser.add_argument("--stm32-port", type=int, default=5006)
    return parser.parse_args()


def main():
    args = parse_args()
    target = None

    if args.stm32_ip is not None:
        target = (args.stm32_ip, args.stm32_port)

    try:
        import pygame
    except ModuleNotFoundError:
        print("pygame is required to run this game. Install it with: python3 -m pip install pygame")
        sys.exit(1)

    pygame.init()

    screen = pygame.display.set_mode((WIDTH, HEIGHT))
    pygame.display.set_caption("Smart Bat UDP Reaction Game")

    clock = pygame.time.Clock()
    font = pygame.font.SysFont(None, 32)
    small_font = pygame.font.SysFont(None, 24)
    big_font = pygame.font.SysFont(None, 64)

    try:
        reader = UdpSwingReader(args.bind_host, args.bind_port, target)
    except OSError as e:
        print(f"Cannot open UDP port {args.bind_host}:{args.bind_port}: {e}")
        pygame.quit()
        sys.exit(1)

    state = "WAIT_LINK"
    state_start = time.monotonic()
    ready_delay = random.uniform(READY_MIN_DELAY, READY_MAX_DELAY)

    pitch_pc_time = None
    ideal_hit_offset_ms = None
    ball_x = PITCH_START_X

    result_text = ""
    result_detail = ""

    score = 0
    round_id = 1

    running = True

    while running:
        now = time.monotonic()
        clock.tick(FPS)
        reader.send_ping(now)

        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False

            elif event.type == pygame.KEYDOWN:
                if event.key == pygame.K_ESCAPE:
                    running = False

                elif event.key == pygame.K_SPACE and state == "RESULT":
                    round_id += 1
                    state = "READY_WAIT"
                    state_start = time.monotonic()
                    ready_delay = random.uniform(
                        READY_MIN_DELAY,
                        READY_MAX_DELAY,
                    )

                    pitch_pc_time = None
                    ideal_hit_offset_ms = None
                    ball_x = PITCH_START_X

                    result_text = ""
                    result_detail = ""

                    reader.reset_input()

        swing = reader.poll()

        if state == "WAIT_LINK":
            if reader.is_link_ready():
                state = "READY_WAIT"
                state_start = time.monotonic()
                ready_delay = random.uniform(
                    READY_MIN_DELAY,
                    READY_MAX_DELAY,
                )

        elif state == "READY_WAIT":
            if now - state_start >= ready_delay:
                state = "PITCHING"

                reader.reset_input()

                pitch_pc_time = time.monotonic()

                travel_time_s = (
                    PITCH_START_X - HIT_ZONE_X
                ) / BALL_SPEED_PX_PER_SEC

                ideal_hit_offset_ms = travel_time_s * 1000.0

                if not reader.send_pitch(round_id):
                    state = "WAIT_LINK"
                    state_start = now

                ball_x = PITCH_START_X

        elif state == "PITCHING":
            elapsed_s = now - pitch_pc_time
            ball_x = PITCH_START_X - BALL_SPEED_PX_PER_SEC * elapsed_s

            if swing is not None:
                timing_error_ms = swing.peak_rt_ms - ideal_hit_offset_ms

                result = classify_timing(timing_error_ms)
                points = result_score(result)
                score += points

                result_text = result
                result_detail = (
                    f"timing error={timing_error_ms:+.0f} ms | "
                    f"start_rt={swing.start_rt_ms} ms | "
                    f"peak_rt={swing.peak_rt_ms} ms | "
                    f"ideal={ideal_hit_offset_ms:.0f} ms | "
                    f"+{points} pts | "
                    f"peak={swing.peak_dps} dps | "
                    f"speed={swing.speed_m_s:.2f} m/s | "
                    f"dur={swing.duration_ms} ms | "
                    f"drop={swing.dropped}"
                )

                state = "RESULT"
                state_start = now

            elif ball_x < -BALL_RADIUS:
                result_text = "MISS"
                result_detail = "No swing detected. +0 pts"
                state = "RESULT"
                state_start = now

        screen.fill((18, 22, 28))

        draw_text(screen, font, f"Round: {round_id}", 30, 25)
        draw_text(screen, font, f"Score: {score}", 30, 55)
        draw_text(
            screen,
            font,
            f"UDP: {reader.link_status_text()}",
            30,
            85,
        )

        if ideal_hit_offset_ms is not None:
            draw_text(screen, font, f"Ideal hit offset: {ideal_hit_offset_ms:.0f} ms", 30, 115)

        if reader.pitch_sync_t is not None:
            sync_text = f"PITCH_SYNC t={reader.pitch_sync_t}"
            if reader.pitch_sync_round is not None:
                sync_text += f" round={reader.pitch_sync_round}"
            draw_text(screen, small_font, sync_text, 30, 145)

        pygame.draw.line(
            screen,
            (100, 110, 120),
            (HIT_ZONE_X, 80),
            (HIT_ZONE_X, 420),
            3,
        )

        pygame.draw.rect(
            screen,
            (80, 100, 130),
            (HIT_ZONE_X - 20, 160, 40, 180),
            2,
        )

        draw_text(screen, small_font, "Hit zone", HIT_ZONE_X - 35, 430)

        pygame.draw.line(
            screen,
            (210, 210, 190),
            (BAT_X - 30, 340),
            (BAT_X + 30, 160),
            6,
        )

        if state == "WAIT_LINK":
            draw_center_text(screen, big_font, "WAITING", 100)
            draw_center_text(
                screen,
                font,
                "Keep this window open. Start or reset STM32 to connect.",
                155,
            )
            if args.stm32_ip is not None:
                draw_center_text(
                    screen,
                    small_font,
                    "Sending heartbeat to configured STM32 address.",
                    190,
                )

        elif state == "READY_WAIT":
            draw_center_text(screen, big_font, "READY", 100)
            draw_center_text(
                screen,
                font,
                "Wait for pitch. Swing when the ball reaches the hit zone.",
                155,
            )

        elif state == "PITCHING":
            draw_center_text(screen, big_font, "PITCH!", 100)
            pygame.draw.circle(
                screen,
                (245, 245, 245),
                (int(ball_x), BALL_Y),
                BALL_RADIUS,
            )

        elif state == "RESULT":
            if result_text == "GOOD":
                color = (80, 255, 120)
            elif result_text in ("EARLY", "LATE"):
                color = (255, 220, 80)
            else:
                color = (255, 100, 100)

            draw_center_text(screen, big_font, result_text, 100, color)
            draw_text(screen, small_font, result_detail, 30, 165)
            draw_center_text(screen, font, "Press SPACE for next pitch.", 430)

            pygame.draw.circle(
                screen,
                (245, 245, 245),
                (HIT_ZONE_X, BALL_Y),
                BALL_RADIUS,
            )

        pygame.display.flip()

    reader.close()
    pygame.quit()


if __name__ == "__main__":
    main()
