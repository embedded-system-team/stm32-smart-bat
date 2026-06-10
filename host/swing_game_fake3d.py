import random
import re
import sys
import time
from dataclasses import dataclass

import pygame
import serial


# ============================================================
# Serial settings
# ============================================================

PORT = "/dev/ttyACM0"
BAUD = 115200


# ============================================================
# Game settings
# ============================================================

WIDTH = 960
HEIGHT = 540
FPS = 60

READY_MIN_DELAY = 1.0
READY_MAX_DELAY = 2.5

BALL_TRAVEL_TIME_MS = 900.0
BALL_START_Z = 1.0
BALL_END_Z = 0.0

BALL_BASE_RADIUS = 10
BALL_MAX_RADIUS = 42

CENTER_X = WIDTH // 2
CENTER_Y = HEIGHT // 2 + 20

ZONE_NEAR_W = 240
ZONE_NEAR_H = 260
ZONE_FAR_W = 80
ZONE_FAR_H = 90

GOOD_WINDOW_MS = 120
EARLY_LATE_LIMIT_MS = 350


# ============================================================
# STM32 log regex
# ============================================================

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

        self.rx_buffer = ""
        self.pitch_sync_t = None

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


# ============================================================
# Game logic
# ============================================================

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


def perspective_scale(z: float) -> float:
    return 1.0 / (0.35 + z)


def ball_radius_from_z(z: float) -> int:
    s = perspective_scale(z)
    r = int(BALL_BASE_RADIUS * s)
    return max(BALL_BASE_RADIUS, min(BALL_MAX_RADIUS, r))


def ball_pos_from_z(z: float):
    s = perspective_scale(z)

    x = CENTER_X
    y = CENTER_Y - int(55 * z) + int(8 * s)

    return x, y


def draw_text(screen, font, text, x, y, color=(255, 255, 255)):
    surface = font.render(text, True, color)
    screen.blit(surface, (x, y))


def draw_center_text(screen, font, text, y, color=(255, 255, 255)):
    surface = font.render(text, True, color)
    rect = surface.get_rect(center=(WIDTH // 2, y))
    screen.blit(surface, rect)


def draw_strike_zone_3d(screen):
    near_rect = pygame.Rect(
        CENTER_X - ZONE_NEAR_W // 2,
        CENTER_Y - ZONE_NEAR_H // 2,
        ZONE_NEAR_W,
        ZONE_NEAR_H,
    )

    far_rect = pygame.Rect(
        CENTER_X - ZONE_FAR_W // 2,
        CENTER_Y - 90 - ZONE_FAR_H // 2,
        ZONE_FAR_W,
        ZONE_FAR_H,
    )

    color_far = (70, 70, 90)
    color_near = (130, 130, 170)

    pygame.draw.rect(screen, color_far, far_rect, 2)
    pygame.draw.rect(screen, color_near, near_rect, 3)

    # connect corners
    pygame.draw.line(screen, color_far, far_rect.topleft, near_rect.topleft, 2)
    pygame.draw.line(screen, color_far, far_rect.topright, near_rect.topright, 2)
    pygame.draw.line(screen, color_far, far_rect.bottomleft, near_rect.bottomleft, 2)
    pygame.draw.line(screen, color_far, far_rect.bottomright, near_rect.bottomright, 2)

    draw_text(screen, pygame.font.SysFont(None, 26), "Hit Zone", near_rect.left + 65, near_rect.bottom + 10)


def draw_bat(screen, swing_flash: bool):
    color = (255, 220, 120) if swing_flash else (180, 180, 180)

    # bat line
    pygame.draw.line(
        screen,
        color,
        (CENTER_X - 120, CENTER_Y + 170),
        (CENTER_X + 80, CENTER_Y - 60),
        10,
    )

    # handle
    pygame.draw.circle(screen, color, (CENTER_X - 120, CENTER_Y + 170), 9)


def draw_ball(screen, z: float):
    x, y = ball_pos_from_z(z)
    r = ball_radius_from_z(z)

    # shadow
    pygame.draw.circle(screen, (40, 40, 45), (x + 6, y + 8), r)

    # ball
    pygame.draw.circle(screen, (245, 245, 245), (x, y), r)

    # seam hint
    pygame.draw.arc(
        screen,
        (180, 40, 40),
        pygame.Rect(x - r // 2, y - r, r, 2 * r),
        -1.2,
        1.2,
        2,
    )
    pygame.draw.arc(
        screen,
        (180, 40, 40),
        pygame.Rect(x - r // 2, y - r, r, 2 * r),
        1.9,
        4.3,
        2,
    )


def main():
    pygame.init()

    screen = pygame.display.set_mode((WIDTH, HEIGHT))
    pygame.display.set_caption("Smart Bat Fake 3D Game - Synced Timing")

    clock = pygame.time.Clock()
    font = pygame.font.SysFont(None, 30)
    small_font = pygame.font.SysFont(None, 24)
    big_font = pygame.font.SysFont(None, 70)

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
    ball_z = BALL_START_Z

    result_text = ""
    result_detail = ""

    score = 0
    round_id = 1
    last_swing_time = 0.0

    running = True

    while running:
        now = time.monotonic()
        dt = clock.tick(FPS) / 1000.0

        # ------------------------------------------------------------
        # Pygame events
        # ------------------------------------------------------------

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
                    ball_z = BALL_START_Z

                    result_text = ""
                    result_detail = ""

                    reader.reset_input()

        # ------------------------------------------------------------
        # Serial
        # ------------------------------------------------------------

        swing = reader.poll()

        # ------------------------------------------------------------
        # Game state
        # ------------------------------------------------------------

        if state == "READY_WAIT":
            if now - state_start >= ready_delay:
                state = "PITCHING"

                reader.reset_input()
                reader.send_pitch()

                pitch_pc_time = time.monotonic()
                ball_z = BALL_START_Z

        elif state == "PITCHING":
            elapsed_ms = (now - pitch_pc_time) * 1000.0
            progress = elapsed_ms / BALL_TRAVEL_TIME_MS
            progress = max(0.0, min(1.0, progress))

            ball_z = BALL_START_Z + (BALL_END_Z - BALL_START_Z) * progress

            if swing is not None:
                timing_error_ms = swing.peak_rt_ms - BALL_TRAVEL_TIME_MS
                result = classify_timing(timing_error_ms)
                points = result_score(result)
                score += points

                result_text = result
                result_detail = (
                    f"timing error={timing_error_ms:+.0f} ms | "
                    f"start_rt={swing.start_rt_ms} ms | "
                    f"peak_rt={swing.peak_rt_ms} ms | "
                    f"ideal={BALL_TRAVEL_TIME_MS:.0f} ms | "
                    f"+{points} pts | "
                    f"peak={swing.peak_dps} dps | "
                    f"speed={swing.speed_m_s:.2f} m/s | "
                    f"dur={swing.duration_ms} ms | "
                    f"drop={swing.dropped}"
                )

                last_swing_time = now
                state = "RESULT"
                state_start = now

            elif elapsed_ms > BALL_TRAVEL_TIME_MS + EARLY_LATE_LIMIT_MS:
                result_text = "MISS"
                result_detail = "No swing detected. +0 pts"
                state = "RESULT"
                state_start = now

        elif state == "RESULT":
            pass

        # ------------------------------------------------------------
        # Drawing
        # ------------------------------------------------------------

        screen.fill((18, 18, 26))

        # background perspective lines
        pygame.draw.line(screen, (40, 40, 55), (CENTER_X, CENTER_Y - 135), (0, HEIGHT), 1)
        pygame.draw.line(screen, (40, 40, 55), (CENTER_X, CENTER_Y - 135), (WIDTH, HEIGHT), 1)
        pygame.draw.line(screen, (35, 35, 48), (0, CENTER_Y + 170), (WIDTH, CENTER_Y + 170), 1)

        draw_strike_zone_3d(screen)

        swing_flash = (now - last_swing_time) < 0.25
        draw_bat(screen, swing_flash=swing_flash)

        # UI
        draw_text(screen, font, f"Round: {round_id}", 30, 25)
        draw_text(screen, font, f"Score: {score}", 30, 55)
        draw_text(screen, font, f"Ideal hit: {BALL_TRAVEL_TIME_MS:.0f} ms", 30, 85)

        if reader.pitch_sync_t is not None:
            draw_text(screen, small_font, f"PITCH_SYNC t={reader.pitch_sync_t}", 30, 115)

        if state == "READY_WAIT":
            draw_center_text(screen, big_font, "READY", 90)
            draw_center_text(
                screen,
                font,
                "Swing when the ball reaches the near strike zone.",
                145,
            )

        elif state == "PITCHING":
            draw_center_text(screen, big_font, "PITCH!", 90)
            draw_ball(screen, ball_z)

            elapsed_ms = (now - pitch_pc_time) * 1000.0
            draw_text(screen, small_font, f"ball time: {elapsed_ms:.0f} ms", 30, 145)

        elif state == "RESULT":
            if result_text == "GOOD":
                color = (80, 255, 120)
            elif result_text in ("EARLY", "LATE"):
                color = (255, 220, 80)
            else:
                color = (255, 100, 100)

            draw_center_text(screen, big_font, result_text, 90, color)
            draw_text(screen, small_font, result_detail, 30, 160)
            draw_center_text(screen, font, "Press SPACE for next pitch.", 480)

            draw_ball(screen, BALL_END_Z)

        pygame.display.flip()

    reader.close()
    pygame.quit()


if __name__ == "__main__":
    main()