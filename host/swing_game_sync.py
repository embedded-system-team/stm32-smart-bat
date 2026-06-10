import random
import re
import sys
import time
from dataclasses import dataclass

import pygame
import serial


# =========================
# Serial settings
# =========================

PORT = "/dev/ttyACM0"
BAUD = 115200


# =========================
# Game settings
# =========================

WIDTH = 900
HEIGHT = 500
FPS = 60

BALL_RADIUS = 16
BALL_Y = 250

PITCH_START_X = 820
HIT_ZONE_X = 220
BAT_X = HIT_ZONE_X

# px/s
BALL_SPEED_PX_PER_SEC = 400

READY_MIN_DELAY = 1.0
READY_MAX_DELAY = 2.5

GOOD_WINDOW_MS = 120
EARLY_LATE_LIMIT_MS = 350


# =========================
# STM32 log regex
# =========================

game_ready_re = re.compile(r"GAME_READY")

pitch_sync_re = re.compile(
    r"PITCH_SYNC t=(?P<t>\d+)"
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

class SerialSwingReader:
    def __init__(self, port: str, baud: int):
        self.ser = serial.Serial(
            port=port,
            baudrate=baud,
            timeout=0,
            rtscts=False,
            dsrdtr=False,
        )

        self.ser.dtr = False
        self.ser.rts = False

        self.pitch_sync_t = None
        self.rx_buffer = ""

    def close(self):
        self.ser.close()

    def reset_input(self):
        self.ser.reset_input_buffer()
        self.rx_buffer = ""
        self.pitch_sync_t = None

    def send_pitch(self):
        self.pitch_sync_t = None
        self.ser.write(b"PITCH\n")
        self.ser.flush()

    def poll_lines(self):
        """
        Non-blocking robust line reader.
        Handles fragmented UART reads.
        """
        lines = []

        n = self.ser.in_waiting
        if n <= 0:
            return lines

        raw = self.ser.read(n)
        if not raw:
            return lines

        self.rx_buffer += raw.decode(errors="replace")

        while "\n" in self.rx_buffer:
            line, self.rx_buffer = self.rx_buffer.split("\n", 1)
            line = line.strip("\r").strip()

            if line:
                lines.append(line)

        return lines

    def poll(self):
        lines = self.poll_lines()

        for line in lines:
            print(line)

            m_sync = pitch_sync_re.search(line)
            if m_sync:
                self.pitch_sync_t = int(m_sync.group("t"))
                continue

            m_start = swing_start_re.search(line)
            if m_start:
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

    def wait_for_game_ready(self, timeout_s: float = 20.0) -> bool:
        print("waiting for STM32 GAME_READY...")

        deadline = time.monotonic() + timeout_s

        while time.monotonic() < deadline:
            lines = self.poll_lines()

            for line in lines:
                print(line)

                if game_ready_re.search(line):
                    print("STM32 GAME_READY OK")
                    return True

            time.sleep(0.01)

        return False

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


def main():
    pygame.init()

    screen = pygame.display.set_mode((WIDTH, HEIGHT))
    pygame.display.set_caption("Smart Bat Reaction Game - Synced Timing")

    clock = pygame.time.Clock()
    font = pygame.font.SysFont(None, 32)
    big_font = pygame.font.SysFont(None, 64)

    try:
        reader = SerialSwingReader(PORT, BAUD)
    except serial.SerialException as e:
        print(f"Cannot open serial port {PORT}: {e}")
        pygame.quit()
        sys.exit(1)

    if not reader.wait_for_game_ready(timeout_s=20.0):
        print("ERROR: STM32 did not send GAME_READY")
        reader.close()
        pygame.quit()
        sys.exit(1)

    state = "READY_WAIT"
    state_start = time.monotonic()
    ready_delay = random.uniform(READY_MIN_DELAY, READY_MAX_DELAY)

    pitch_pc_time = None
    ideal_hit_offset_ms = None
    ball_x = PITCH_START_X

    result_text = ""
    result_detail = ""

    score = 0
    round_id = 1
    last_timing_error_ms = None

    running = True

    while running:
        now = time.monotonic()
        dt = clock.tick(FPS) / 1000.0

        # =========================
        # Event handling
        # =========================

        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False

            elif event.type == pygame.KEYDOWN:
                if event.key == pygame.K_ESCAPE:
                    running = False

                elif event.key == pygame.K_SPACE:
                    if state == "RESULT":
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
                        last_timing_error_ms = None

                        reader.reset_input()

        # =========================
        # Serial polling
        # =========================

        swing = reader.poll()

        # =========================
        # Game state machine
        # =========================

        if state == "READY_WAIT":
            if now - state_start >= ready_delay:
                state = "PITCHING"

                reader.reset_input()

                pitch_pc_time = time.monotonic()

                travel_time_s = (
                    PITCH_START_X - HIT_ZONE_X
                ) / BALL_SPEED_PX_PER_SEC

                ideal_hit_offset_ms = travel_time_s * 1000.0

                reader.send_pitch()

                ball_x = PITCH_START_X

        elif state == "PITCHING":
            elapsed_s = now - pitch_pc_time
            ball_x = PITCH_START_X - BALL_SPEED_PX_PER_SEC * elapsed_s

            if swing is not None:
                timing_error_ms = swing.peak_rt_ms - ideal_hit_offset_ms
                last_timing_error_ms = timing_error_ms

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

        elif state == "RESULT":
            pass

        # =========================
        # Drawing
        # =========================

        screen.fill((20, 20, 25))

        # UI info
        draw_text(screen, font, f"Round: {round_id}", 30, 25)
        draw_text(screen, font, f"Score: {score}", 30, 55)

        draw_text(
            screen,
            font,
            f"Ideal hit offset: "
            f"{ideal_hit_offset_ms:.0f} ms"
            if ideal_hit_offset_ms is not None
            else "Ideal hit offset: --",
            30,
            85,
        )

        if reader.pitch_sync_t is not None:
            draw_text(
                screen,
                font,
                f"PITCH_SYNC t={reader.pitch_sync_t}",
                30,
                115,
            )

        # Hit zone
        pygame.draw.line(
            screen,
            (100, 100, 100),
            (HIT_ZONE_X, 80),
            (HIT_ZONE_X, 420),
            3,
        )

        pygame.draw.rect(
            screen,
            (80, 80, 120),
            (HIT_ZONE_X - 20, 160, 40, 180),
            2,
        )

        draw_text(screen, font, "Hit zone", HIT_ZONE_X - 45, 430)

        # Simple bat
        pygame.draw.line(
            screen,
            (180, 180, 180),
            (BAT_X - 30, 340),
            (BAT_X + 30, 160),
            6,
        )

        if state == "READY_WAIT":
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
                (255, 255, 255),
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
            draw_text(screen, font, result_detail, 40, 165)
            draw_center_text(screen, font, "Press SPACE for next pitch.", 430)

            # show ball at hit zone for result reference
            pygame.draw.circle(
                screen,
                (255, 255, 255),
                (HIT_ZONE_X, BALL_Y),
                BALL_RADIUS,
            )

        pygame.display.flip()

    reader.close()
    pygame.quit()


if __name__ == "__main__":
    main()