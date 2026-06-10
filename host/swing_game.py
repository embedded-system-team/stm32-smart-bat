import random
import re
import sys
import time
from dataclasses import dataclass

import pygame
import serial


PORT = "/dev/ttyACM0"
BAUD = 115200

WIDTH = 900
HEIGHT = 500

FPS = 60

BALL_RADIUS = 16
BALL_Y = 250

PITCH_START_X = 820
HIT_ZONE_X = 220
BAT_X = HIT_ZONE_X
BALL_SPEED_PX_PER_SEC = 500

GOOD_WINDOW_MS = 120
EARLY_LATE_LIMIT_MS = 350

READY_MIN_DELAY = 1.0
READY_MAX_DELAY = 2.5


swing_start_re = re.compile(
    r"SWING_START t=(?P<t>\d+)"
)

swing_end_re = re.compile(
    r"SWING_END t=(?P<t>\d+) "
    r"dur=(?P<dur>\d+) "
    r"peak_t=(?P<peak_t>\d+) "
    r"peak_dps=(?P<peak_dps>\d+) "
    r"speed_x100=(?P<speed_x100>\d+) "
    r"drop=(?P<drop>\d+)"
)


@dataclass
class SwingEvent:
    stm32_start_t: int
    stm32_end_t: int
    duration_ms: int
    peak_t: int
    peak_dps: int
    speed_m_s: float
    dropped: int
    start_pc_time: float
    peak_pc_time: float
    end_pc_time: float


class SerialSwingReader:
    def __init__(self, port: str, baud: int):
        self.ser = serial.Serial(port, baud, timeout=0)
        self.stm32_start_t = None
        self.start_pc_time = None

    def reset(self):
        self.ser.reset_input_buffer()
        self.stm32_start_t = None
        self.start_pc_time = None

    def poll(self):
        """
        Non-blocking serial parser.
        Returns SwingEvent if a complete SWING_END is parsed.
        Otherwise returns None.
        """
        while True:
            raw = self.ser.readline()
            if not raw:
                return None

            line = raw.decode(errors="replace").strip()
            print(line)

            m_start = swing_start_re.search(line)
            if m_start:
                self.stm32_start_t = int(m_start.group("t"))
                self.start_pc_time = time.monotonic()
                continue

            m_end = swing_end_re.search(line)
            if m_end:
                end_pc_time = time.monotonic()

                stm32_end_t = int(m_end.group("t"))
                duration = int(m_end.group("dur"))
                peak_t = int(m_end.group("peak_t"))
                peak_dps = int(m_end.group("peak_dps"))
                speed = int(m_end.group("speed_x100")) / 100.0
                drop = int(m_end.group("drop"))

                if self.stm32_start_t is None or self.start_pc_time is None:
                    self.stm32_start_t = stm32_end_t - duration
                    self.start_pc_time = end_pc_time - duration / 1000.0

                peak_offset_ms = peak_t - self.stm32_start_t
                peak_pc_time = self.start_pc_time + peak_offset_ms / 1000.0

                return SwingEvent(
                    stm32_start_t=self.stm32_start_t,
                    stm32_end_t=stm32_end_t,
                    duration_ms=duration,
                    peak_t=peak_t,
                    peak_dps=peak_dps,
                    speed_m_s=speed,
                    dropped=drop,
                    start_pc_time=self.start_pc_time,
                    peak_pc_time=peak_pc_time,
                    end_pc_time=end_pc_time,
                )


def classify_timing(error_ms: float) -> str:
    if abs(error_ms) <= GOOD_WINDOW_MS:
        return "GOOD"
    if error_ms < -GOOD_WINDOW_MS:
        return "EARLY"
    return "LATE"


def draw_text(screen, font, text, x, y):
    surface = font.render(text, True, (255, 255, 255))
    screen.blit(surface, (x, y))


def main():
    pygame.init()

    screen = pygame.display.set_mode((WIDTH, HEIGHT))
    pygame.display.set_caption("Smart Bat Reaction Game")

    clock = pygame.time.Clock()
    font = pygame.font.SysFont(None, 36)
    big_font = pygame.font.SysFont(None, 64)

    try:
        reader = SerialSwingReader(PORT, BAUD)
    except serial.SerialException as e:
        print(f"Cannot open serial port {PORT}: {e}")
        pygame.quit()
        sys.exit(1)

    state = "READY_WAIT"
    state_start = time.monotonic()
    ready_delay = random.uniform(READY_MIN_DELAY, READY_MAX_DELAY)

    pitch_time = None
    ideal_hit_time = None
    ball_x = PITCH_START_X

    result_text = ""
    result_detail = ""
    last_swing = None

    running = True

    while running:
        now = time.monotonic()
        dt = clock.tick(FPS) / 1000.0

        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False

            if event.type == pygame.KEYDOWN:
                if event.key == pygame.K_SPACE:
                    state = "READY_WAIT"
                    state_start = time.monotonic()
                    ready_delay = random.uniform(READY_MIN_DELAY, READY_MAX_DELAY)
                    pitch_time = None
                    ideal_hit_time = None
                    ball_x = PITCH_START_X
                    result_text = ""
                    result_detail = ""
                    last_swing = None
                    reader.reset()

        swing = reader.poll()

        if state == "READY_WAIT":
            if now - state_start >= ready_delay:
                state = "PITCHING"
                pitch_time = time.monotonic()

                travel_time = (PITCH_START_X - HIT_ZONE_X) / BALL_SPEED_PX_PER_SEC
                ideal_hit_time = pitch_time + travel_time

                ball_x = PITCH_START_X
                reader.reset()

        elif state == "PITCHING":
            elapsed = now - pitch_time
            ball_x = PITCH_START_X - BALL_SPEED_PX_PER_SEC * elapsed

            if swing is not None:
                last_swing = swing

                timing_error_ms = (swing.peak_pc_time - ideal_hit_time) * 1000.0
                result = classify_timing(timing_error_ms)

                result_text = result
                result_detail = (
                    f"timing error={timing_error_ms:+.0f} ms | "
                    f"peak={swing.peak_dps} dps | "
                    f"speed={swing.speed_m_s:.2f} m/s | "
                    f"dur={swing.duration_ms} ms | "
                    f"drop={swing.dropped}"
                )

                state = "RESULT"
                state_start = now

            elif ball_x < -BALL_RADIUS:
                result_text = "MISS"
                result_detail = "No swing detected."
                state = "RESULT"
                state_start = now

        elif state == "RESULT":
            pass

        screen.fill((20, 20, 25))

        pygame.draw.line(screen, (100, 100, 100), (HIT_ZONE_X, 80), (HIT_ZONE_X, 420), 3)
        pygame.draw.rect(screen, (80, 80, 120), (HIT_ZONE_X - 20, 160, 40, 180), 2)

        pygame.draw.line(screen, (180, 180, 180), (BAT_X - 30, 340), (BAT_X + 30, 160), 6)

        if state == "READY_WAIT":
            draw_text(screen, big_font, "READY", 360, 80)
            draw_text(screen, font, "Wait for pitch. Swing after the ball is thrown.", 180, 150)

        elif state == "PITCHING":
            draw_text(screen, big_font, "PITCH!", 360, 80)
            pygame.draw.circle(screen, (255, 255, 255), (int(ball_x), BALL_Y), BALL_RADIUS)

        elif state == "RESULT":
            draw_text(screen, big_font, result_text, 360, 80)
            draw_text(screen, font, result_detail, 80, 160)
            draw_text(screen, font, "Press SPACE for next pitch.", 280, 420)

            if last_swing is not None:
                pygame.draw.circle(screen, (255, 255, 255), (HIT_ZONE_X, BALL_Y), BALL_RADIUS)

        draw_text(screen, font, "Hit zone", HIT_ZONE_X - 45, 430)

        pygame.display.flip()

    reader.ser.close()
    pygame.quit()


if __name__ == "__main__":
    main()